#include "output.hpp"
#include "compact.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <span>

namespace shop {
namespace {
constexpr size_t partitions = 256;
template<class T> using Bins = std::array<std::vector<T>, partitions>;
template<class T> void release(std::vector<T>& v) { std::vector<T>().swap(v); }
size_t shard(Key k) { return mix(k) & (partitions - 1); }

Event validate(const std::array<View,7>& f, int64_t ingest, Window w, const Dates& dt, Keys& keys,
               const Dimensions& dim) {
    if (f[1].empty() || f[5].empty()) return {};
    int64_t ts = dt.parse(f[2]);
    if (ts < w.start || ts >= w.end || ingest > ts + day_ms) return {};
    uint32_t cents = 0, category = 0;
    if (f[4] == "purchase") {
        int64_t price = decimal(f[6], 10000000);
        if (price <= 0) return {};
        cents = static_cast<uint32_t>(price);
        category = dim.category(f[5]);
    } else if (f[4] != "view" && f[4] != "cart") {
        return {};
    }
    return {keys.encode(f[1]), ts, cents, category};
}

void read_events(const Input& input, Window w, const Dates& dt, Keys& keys, const Dimensions& dim,
                 std::vector<Bins<Candidate>>& bins) {
    jobs(input.chunks.size(), bins.size(), [&](unsigned lane, size_t i) {
        rows<7>(input.chunks[i], [&](const auto& f) {
            if (f[0].empty()) return;
            Key id = keys.encode(f[0]);
            int64_t ingest = dt.parse(f[3]);
            if (ingest == bad_time) return; // ingest_ts is guaranteed valid F1.
            // Invalid candidates still compete. A winning tombstone suppresses
            // all later, valid copies, including copies naming another user.
            bins[lane][shard(id)].push_back({id, ingest, validate(f, ingest, w, dt, keys, dim)});
        });
    });
}

void deduplicate(std::vector<Bins<Candidate>>& candidates, const People& people,
                 std::vector<Bins<Event>>& by_person) {
    jobs(partitions, by_person.size(), [&](unsigned lane, size_t part) {
        size_t count = 0;
        for (auto& src : candidates) count += src[part].size();
        if (!count) return;
        size_t size = std::bit_ceil(count + count / 2 + 1);
        std::vector<const Candidate*> slots(size, nullptr);
        for (auto& src : candidates) for (const Candidate& row : src[part]) {
            size_t at = (mix(row.id) / partitions) & (size - 1);
            while (slots[at] && slots[at]->id != row.id) at = (at + 1) & (size - 1);
            if (!slots[at] || row.ingest < slots[at]->ingest) slots[at] = &row;
        }
        for (const auto* winner : slots) if (winner && winner->event.person) {
            Event e = winner->event;
            e.person = people.canonical(e.person);
            by_person[lane][shard(e.person)].push_back(e);
        }
        for (auto& src : candidates) release(src[part]);
    });
}

void read_clicks(const Input& input, Window w, const Dates& dt, Keys& keys, const People& people,
                 const Dimensions& dim, std::vector<Bins<Click>>& bins) {
    jobs(input.chunks.size(), bins.size(), [&](unsigned lane, size_t i) {
        rows<4>(input.chunks[i], [&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            uint32_t ad = dim.campaign_index(f[2]);
            if (ad == UINT32_MAX) return;
            int64_t ts = dt.parse(f[3]);
            if (ts < w.start - week_ms || ts >= w.end) return;
            Key person = people.canonical(keys.encode(f[1]));
            bins[lane][shard(person)].push_back({person, ts, keys.encode(f[0]), ad});
        });
    });
}

template<class T>
std::vector<T> gather(std::vector<Bins<T>>& sources, size_t part) {
    size_t count = 0;
    for (auto& source : sources) count += source[part].size();
    std::vector<T> out;
    out.reserve(count);
    for (auto& source : sources) {
        auto& in = source[part];
        out.insert(out.end(), in.begin(), in.end());
        release(in);
    }
    return out;
}

// Distinct active campaigns support O(1) insertion/removal. Expired clicks
// need never be inserted when the next session is more than a week later.
class Attribution {
    std::vector<size_t> counts_, position_;
    std::vector<uint32_t> active_;
    std::span<const Click> clicks_;
    size_t first_ = 0, past_ = 0;
public:
    explicit Attribution(size_t campaigns) : counts_(campaigns), position_(campaigns) {}
    void reset(std::span<const Click> clicks) {
        for (uint32_t c : active_) counts_[c] = 0;
        active_.clear();
        clicks_ = clicks;
        first_ = past_ = 0;
    }
    void count(int64_t start, int32_t day, int64_t revenue, CampaignDays& totals) {
        const int64_t lower = start - week_ms;
        while (first_ < past_ && clicks_[first_].ts < lower) {
            uint32_t c = clicks_[first_++].campaign;
            if (--counts_[c] == 0) {
                uint32_t last = active_.back();
                active_[position_[c]] = last;
                position_[last] = position_[c];
                active_.pop_back();
            }
        }
        while (past_ < clicks_.size() && clicks_[past_].ts < lower) ++past_;
        if (active_.empty()) first_ = past_;
        while (past_ < clicks_.size() && clicks_[past_].ts <= start) {
            uint32_t c = clicks_[past_++].campaign;
            if (counts_[c]++ == 0) {
                position_[c] = active_.size();
                active_.push_back(c);
            }
        }
        // Within each timestamp, click IDs were sorted in descending byte order.
        uint32_t winner = first_ == past_ ? 0 : clicks_[past_ - 1].campaign;
        Counts& credit = totals[campaign_day(winner, day)];
        ++credit.sessions;
        credit.purchasers += revenue != 0;
        credit.revenue += revenue;
        for (uint32_t c : active_) ++totals[campaign_day(c, day)].assists;
    }
};

class Aggregate {
    Attribution attribution_;
    std::vector<int64_t> category_sum_;
    std::vector<uint32_t> purchased_;
public:
    CampaignDays campaigns;
    explicit Aggregate(const Dimensions& d)
        : attribution_(d.campaigns.size()), category_sum_(d.categories.size()) {}
    void person(std::span<const Event> events, std::span<const Click> clicks,
                std::vector<UserDay>& out) {
        attribution_.reset(clicks);
        UserDay daily{events.front().person, utc_day(events.front().ts), no_category, 0, 0, 0};
        auto flush = [&] {
            int64_t best = 0;
            for (uint32_t c : purchased_) {
                int64_t amount = category_sum_[c];
                if (amount > best || (amount == best && c < daily.category)) {
                    best = amount;
                    daily.category = c;
                }
                category_sum_[c] = 0;
            }
            purchased_.clear();
            out.push_back(daily);
        };
        size_t i = 0;
        while (i < events.size()) {
            size_t begin = i;
            int64_t start = events[i].ts, revenue = 0;
            int32_t day = utc_day(start);
            if (day != daily.day) {
                flush();
                daily = {daily.person, day, no_category, 0, 0, 0};
            }
            do {
                const auto& e = events[i++];
                if (e.cents) {
                    if (!category_sum_[e.category]) purchased_.push_back(e.category);
                    category_sum_[e.category] += e.cents;
                    revenue += e.cents;
                }
            } while (i < events.size() && events[i].ts - events[i-1].ts <= gap_ms);
            ++daily.sessions;
            daily.events += i - begin;
            daily.revenue += revenue;
            attribution_.count(start, day, revenue, campaigns);
        }
        flush();
    }
};

void summarize(std::vector<Event>& events, std::vector<Click>& clicks, const Keys& keys,
               Aggregate& aggregate, std::vector<UserDay>& out) {
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.person == b.person ? a.ts < b.ts : a.person < b.person;
    });
    std::sort(clicks.begin(), clicks.end(), [&](const Click& a, const Click& b) {
        if (a.person != b.person) return a.person < b.person;
        if (a.ts != b.ts) return a.ts < b.ts;
        return keys.less(b.id, a.id);
    });
    // All copies of a click agree in every decoded field and hence in
    // validation, canonical person and time. Sorting placed them together.
    auto last = std::unique(clicks.begin(), clicks.end(), [](const Click& a, const Click& b) {
        return a.id == b.id;
    });
    clicks.erase(last, clicks.end());
    size_t e = 0, c = 0;
    while (e < events.size()) {
        size_t begin = e;
        Key user = events[e].person;
        do { ++e; } while (e < events.size() && events[e].person == user);
        while (c < clicks.size() && clicks[c].person < user) ++c;
        size_t cb = c;
        while (c < clicks.size() && clicks[c].person == user) ++c;
        aggregate.person(std::span(events).subspan(begin, e-begin),
                         std::span(clicks).subspan(cb, c-cb), out);
    }
}

void run(const fs::path& in, const fs::path& out, unsigned cpu) {
    auto mark = [started=std::chrono::steady_clock::now()](const char* step) {
        if (std::getenv("SHOP_TIMING")) {
            auto sec = std::chrono::duration<double>(std::chrono::steady_clock::now()-started);
            std::cerr << step << " " << sec.count() << " s elapsed\n";
        }
    };
    Window window = read_window(in / "manifest.json");
    Input products(in/"products.csv"), ads(in/"campaigns.csv"), links(in/"links");
    Input events(in/"events"), clicks(in/"clicks");
    Keys keys;
    Dates dt(window);
    Dimensions dim(products, ads);
    if (compact::suitable(window,dim)) {
        compact::run(events,clicks,links,out,window,dt,keys,dim,cpu,mark);
        return;
    }
    auto linking = std::async(std::launch::async, [&] { return People(links, keys); });
    mark("opened inputs");
    unsigned lanes = std::min<unsigned>(cpu, std::max<size_t>(1, events.chunks.size() + clicks.chunks.size()));
    std::vector<Bins<Candidate>> candidates(lanes);
    std::vector<Bins<Event>> event_bins(lanes);
    std::vector<Bins<Click>> click_bins(lanes);
    read_events(events, window, dt, keys, dim, candidates);
    People people = linking.get();
    mark("linked IDs and parsed events");
    deduplicate(candidates, people, event_bins);
    mark("deduplicated events");
    read_clicks(clicks, window, dt, keys, people, dim, click_bins);
    mark("parsed clicks");
    std::vector<Aggregate> workers;
    for (unsigned i=0; i<lanes; ++i) workers.emplace_back(dim);
    std::vector<std::vector<UserDay>> users(partitions);
    jobs(partitions, lanes, [&](unsigned lane, size_t part) {
        auto ev = gather(event_bins, part);
        auto cl = gather(click_bins, part);
        if (!ev.empty()) summarize(ev, cl, keys, workers[lane], users[part]);
    });
    CampaignDays totals;
    for (const auto& worker : workers)
        for (const auto& [key, count] : worker.campaigns) totals[key] += count;
    mark("aggregated sessions");
    write_output(out, keys, dim, users, totals, cpu);
    mark("wrote parquet");
}
}
}

int main(int argc, char** argv) {
    try {
        auto env = [](const char* name, const char* fallback) {
            const char* v = std::getenv(name);
            return v && *v ? v : fallback;
        };
        const char* input = argc > 1 ? argv[1] : env("DATA_DIR", "/data");
        const char* output = argc > 2 ? argv[2] : env("OUTPUT_DIR", "/output");
        unsigned cpu = std::clamp(std::stoi(env("NCPU", "8")), 1, 64);
        shop::run(input, output, cpu);
        std::cerr.flush();
        _exit(0);
    } catch (const std::exception& e) {
        std::cerr << "shopping-sessions: " << e.what() << '\n';
        return 1;
    }
}
