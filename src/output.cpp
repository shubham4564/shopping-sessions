#include "output.hpp"
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

namespace shop {
namespace {
void check(const arrow::Status& s) {
    if (!s.ok()) throw std::runtime_error(s.ToString());
}
template<class T> T take(arrow::Result<T> r) {
    check(r.status());
    return std::move(r).ValueUnsafe();
}
template<class T> std::shared_ptr<arrow::Array> finish(T& builder) { return take(builder.Finish()); }
using Schema = std::shared_ptr<arrow::Schema>;
const auto text_type = arrow::utf8();
const auto date_type = arrow::date32();
const auto integer = arrow::int64();

class Writer {
    std::shared_ptr<arrow::io::FileOutputStream> file_;
    std::unique_ptr<parquet::arrow::FileWriter> writer_;
public:
    Schema schema;
    Writer(const fs::path& path, Schema s) : schema(std::move(s)) {
        file_ = take(arrow::io::FileOutputStream::Open(path.string()));
        parquet::WriterProperties::Builder properties;
        properties.compression(parquet::Compression::SNAPPY);
        writer_ = take(parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(),
                                                       file_, properties.build()));
    }
    void batch(const std::vector<std::shared_ptr<arrow::Array>>& columns) {
        auto table = arrow::Table::Make(schema, columns);
        check(writer_->WriteTable(*table, table->num_rows()));
    }
    void close() { check(writer_->Close()); check(file_->Close()); }
};

void user_file(const fs::path& path, const Keys& keys, const Dimensions& dim,
               const std::vector<std::vector<UserDay>>& groups) {
    Writer out(path, arrow::schema({arrow::field("user_id", text_type, false),
        arrow::field("day", date_type, false), arrow::field("sessions",integer,false),
        arrow::field("events",integer,false), arrow::field("revenue_cents",integer,false),
        arrow::field("top_category",text_type)}));
    arrow::StringBuilder users, categories;
    arrow::Date32Builder days;
    arrow::Int64Builder sessions, events, revenue;
    auto flush = [&] {
        if (days.length()) out.batch({finish(users), finish(days), finish(sessions),
                                     finish(events), finish(revenue), finish(categories)});
    };
    for (const auto& group : groups) for (const auto& row : group) {
        char buf[9];
        check(users.Append(keys.decode(row.person, buf)));
        check(days.Append(row.day));
        check(sessions.Append(row.sessions));
        check(events.Append(row.events));
        check(revenue.Append(row.revenue));
        if (row.category == no_category) check(categories.AppendNull());
        else check(categories.Append(dim.categories[row.category]));
        if (days.length() == 65536) flush();
    }
    flush();
    out.close();
}

void campaign_file(const fs::path& path, const Dimensions& dim, const CampaignDays& rows) {
    Writer out(path, arrow::schema({arrow::field("campaign_id",text_type,false),
        arrow::field("channel",text_type,false), arrow::field("day",date_type,false),
        arrow::field("sessions",integer,false), arrow::field("purchasing_sessions",integer,false),
        arrow::field("revenue_cents",integer,false), arrow::field("assisted_sessions",integer,false)}));
    arrow::StringBuilder campaigns, channels;
    arrow::Date32Builder days;
    arrow::Int64Builder sessions, purchases, revenue, assists;
    auto flush = [&] {
        if (days.length()) out.batch({finish(campaigns), finish(channels), finish(days), finish(sessions),
                                     finish(purchases), finish(revenue), finish(assists)});
    };
    for (const auto& [key, count] : rows) {
        const auto& ad = dim.campaigns[static_cast<uint32_t>(key)];
        check(campaigns.Append(ad.id));
        check(channels.Append(ad.channel));
        check(days.Append(static_cast<int32_t>(key >> 32)));
        check(sessions.Append(count.sessions));
        check(purchases.Append(count.purchasers));
        check(revenue.Append(count.revenue));
        check(assists.Append(count.assists));
        if (days.length() == 65536) flush();
    }
    flush();
    out.close();
}
}
void write_output(const fs::path& out, const Keys& keys, const Dimensions& dim,
                  const std::vector<std::vector<UserDay>>& users, const CampaignDays& campaigns) {
    const fs::path u = out / "user_daily", c = out / "campaign_daily";
    fs::create_directories(u);
    fs::create_directories(c);
    std::string tmp = ".part-" + std::to_string(getpid()) + ".tmp";
    try {
        user_file(u / tmp, keys, dim, users);
        campaign_file(c / tmp, dim, campaigns);
        fs::rename(u / tmp, u / "part-0.parquet");
        fs::rename(c / tmp, c / "part-0.parquet");
    } catch (...) {
        std::error_code ec;
        fs::remove(u / tmp, ec);
        fs::remove(c / tmp, ec);
        throw;
    }
}
}
