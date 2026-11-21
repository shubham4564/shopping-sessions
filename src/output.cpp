#include "output.hpp"
#include "parquet.hpp"

namespace shop {
namespace {
using namespace parquet;
constexpr std::array<Field, 6> user_schema{{{"user_id", bytes, utf8},
                                            {"day", i32, date},
                                            {"sessions", i64},
                                            {"events", i64},
                                            {"revenue_cents", i64},
                                            {"top_category", bytes, utf8, true}}};
constexpr std::array<Field, 7> campaign_schema{{{"campaign_id", bytes, utf8},
                                                {"channel", bytes, utf8},
                                                {"day", i32, date},
                                                {"sessions", i64},
                                                {"purchasing_sessions", i64},
                                                {"revenue_cents", i64},
                                                {"assisted_sessions", i64}}};

void campaign_file(const fs::path& path, const Dimensions& dim, const CampaignDays& rows) {
    File file(path, campaign_schema);
    auto& v = file.values;
    for (const auto& [key, c] : rows) {
        const auto& campaign = dim.campaigns[uint32_t(key)];
        v[0].string(campaign.id);
        v[1].string(campaign.channel);
        v[2].number(int32_t(key >> 32));
        v[3].number(c.sessions);
        v[4].number(c.purchasers);
        v[5].number(c.revenue);
        v[6].number(c.assists);
        file.finish_row();
    }
    file.close();
}
} // namespace
struct Output::State {
    const Keys& keys;
    const Dimensions& dim;
    std::array<fs::path, parts + 1> temp, dest;
    std::array<std::unique_ptr<parquet::File>, parts> users;
    bool committed = false;
    State(const fs::path& out, const Keys& k, const Dimensions& d) : keys(k), dim(d) {
        auto u = out / "user_daily", c = out / "campaign_daily";
        fs::create_directories(u);
        fs::create_directories(c);
        std::string prefix = ".write-" + std::to_string(getpid()) + "-";
        for (unsigned i = 0; i <= parts; ++i) {
            const auto& folder = i == parts ? c : u;
            temp[i] = folder / (prefix + std::to_string(i));
            dest[i] = folder / ("part-" + std::to_string(i == parts ? 0 : i) + ".parquet");
        }
    }
    ~State() {
        if (!committed) {
            std::error_code ec;
            for (auto& p : temp)
                fs::remove(p, ec);
        }
    }
};
Output::Output(const fs::path& out, const Keys& keys, const Dimensions& dim)
    : state_(std::make_unique<State>(out, keys, dim)) {
}
Output::~Output() = default;

void Output::append(unsigned part, std::span<const UserDay> rows) {
    auto& file = state_->users[part];
    if (!file)
        file = std::make_unique<parquet::File>(state_->temp[part], user_schema);
    auto& v = file->values;
    char name[9];
    for (const auto& row : rows) {
        v[0].string(state_->keys.decode(row.person, name));
        v[1].number(row.day);
        v[2].number(row.sessions);
        v[3].number(row.events);
        v[4].number(row.revenue);
        if (row.category != no_category) {
            file->defined();
            v[5].string(state_->dim.categories[row.category]);
        }
        file->finish_row();
    }
}
void Output::finish(const CampaignDays& totals, unsigned workers) {
    jobs(parts + 1, workers, [&](unsigned, size_t i) {
        if (i == parts)
            campaign_file(state_->temp[i], state_->dim, totals);
        else {
            if (!state_->users[i])
                append(i, {});
            state_->users[i]->close();
        }
    });
    for (unsigned i = 0; i <= parts; ++i)
        fs::rename(state_->temp[i], state_->dest[i]);
    state_->committed = true;
}
void write_output(const fs::path& out, const Keys& keys, const Dimensions& dim,
                  const std::vector<std::vector<UserDay>>& people, const CampaignDays& totals,
                  unsigned workers) {
    Output writer(out, keys, dim);
    jobs(Output::parts, workers, [&](unsigned, size_t i) {
        for (size_t j = people.size() * i / Output::parts; j < people.size() * (i + 1) / Output::parts; ++j)
            writer.append(i, people[j]);
    });
    writer.finish(totals, workers);
}
} // namespace shop
