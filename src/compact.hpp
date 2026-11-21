#pragma once
#include "fast_date.hpp"
#include "output.hpp"
#include "plain.hpp"

namespace shop::compact {
constexpr unsigned bucket_bits = 8, buckets = 1u << bucket_bits;
constexpr unsigned id_bits = 8, id_buckets = 1u << id_bits;
// Both operations are bijective over 64 bits. The permuted value is the
// complete event key; partitioning never discards the bits needed for equality.
inline uint64_t event_hash(Key key) {
    key = (key ^ (key >> 32)) * 0x9e3779b97f4a7c15ULL;
    return key ^ (key >> 29);
}

// One producer per arena. Blocks keep row addresses stable throughout global
// deduplication and avoid copying full partitions whenever a vector grows.
class Arena {
    static constexpr size_t page = 32u << 20;
    std::vector<void*> slabs_;
    char* current_ = nullptr;
    size_t left_ = 0;

  public:
    Arena() = default;
    Arena(const Arena&) = delete;
    ~Arena() { clear(); }
    void clear() {
        for (auto p : slabs_)
            munmap(p, page);
        slabs_.clear();
        current_ = nullptr;
        left_ = 0;
    }
    void* obtain(size_t n) {
        n = (n + 63) & ~size_t(63);
        if (left_ < n) {
            void* p = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                throw std::bad_alloc();
            madvise(p, page, MADV_HUGEPAGE);
            slabs_.push_back(p);
            current_ = static_cast<char*>(p);
            left_ = page;
        }
        char* p = current_;
        current_ += n;
        left_ -= n;
        return p;
    }
};
constexpr unsigned records_per_block = 256;
template <class T, unsigned count = buckets> class Partitions {
    struct Block {
        Block* previous;
        T data[records_per_block];
    };
    struct Chain {
        T* cur = nullptr;
        T* end = nullptr;
        Block* head = nullptr;
        size_t full_blocks = 0;
    };
    Arena pool_;
    std::array<Chain, count> chains_{};
    [[gnu::noinline]] void grow(Chain& c) {
        if (c.head)
            ++c.full_blocks;
        auto* head = static_cast<Block*>(pool_.obtain(sizeof(Block)));
        head->previous = c.head;
        c.head = head;
        c.cur = head->data;
        c.end = head->data + records_per_block;
    }

  public:
    [[gnu::always_inline]] inline T& add(unsigned part, const T& row) {
        Chain& c = chains_[part];
        if (c.cur == c.end) [[unlikely]]
            grow(c);
        T* p = c.cur++;
        *p = row;
        return *p;
    }
    template <class F> void blocks(unsigned part, F&& f) {
        const Chain& c = chains_[part];
        if (!c.head)
            return;
        size_t used = size_t(c.cur - c.head->data);
        for (Block* b = c.head; b; b = b->previous) {
            f(std::span<T>(b->data, used));
            used = records_per_block;
        }
    }
    size_t size(unsigned part) const {
        const Chain& c = chains_[part];
        return c.head ? c.full_blocks * records_per_block + size_t(c.cur - c.head->data) : 0;
    }
    void clear() {
        chains_ = {};
        pool_.clear();
    }
};

struct Hit {
    uint32_t user, time, purchase;
};
struct Raw {
    Key id;
    union {
        Key user;
        Hit* emitted;
    };
    uint64_t data;
};
struct Detail {
    int64_t ingest;
    uint32_t time, purchase;
};
struct Ad {
    Key id;
    uint32_t user, time, campaign;
};
using GroupHit = uint64_t;
struct GroupAd {
    Key id;
    uint32_t time, campaign;
};
struct Lane {
    Partitions<Raw, id_buckets> input;
    Partitions<Hit> hits;
    Partitions<Ad> ads;
    std::vector<Detail> details;
};
constexpr uint32_t detail_bit = uint32_t{1} << 31;

inline bool suitable(Window w, const Dimensions& d) {
    const uint64_t days = uint64_t(utc_day(w.end - 1)) - uint64_t(utc_day(w.start)) + 1;
    return w.end - w.start + week_ms < detail_bit && d.categories.size() <= 256 &&
           d.campaigns.size() * days <= (1u << 18);
}

[[gnu::noinline]] inline uint64_t extended_event(int64_t ingest, uint32_t time, uint32_t buy, Lane& lane,
                                                 unsigned owner) {
    if (lane.details.size() >= detail_bit)
        throw std::length_error("too many extended event records");
    uint64_t code = (uint64_t(owner) << 32) | (detail_bit + uint32_t(lane.details.size()));
    lane.details.push_back({ingest, time, buy});
    return code;
}
[[gnu::always_inline]] inline void store_event(Key id, Key user, int64_t ingest, uint32_t time,
                                               uint32_t buy, Lane& lane, unsigned owner) {
    uint64_t packed;
    if (!buy && ingest >= INT32_MIN && ingest <= INT32_MAX)
        packed = (uint64_t(uint32_t(ingest)) << 32) | time;
    else
        packed = extended_event(ingest, time, buy, lane, owner);
    uint64_t key = event_hash(id);
    lane.input.add(uint32_t(key) & (id_buckets - 1), {key, user, packed});
}
inline void put_event(const std::array<View, 7>& f, Window w, const Dates& dt, Keys& keys,
                      const Dimensions& dim, Lane& lane, unsigned owner) {
    if (f[0].empty())
        return;
    const int64_t ingest = f[3].size() == 20 ? dt.relative_ingest(f[3].data()) : bad_time;
    if (ingest == bad_time)
        return;
    Key id = keys.encode_unordered(f[0]), user = 0;
    uint32_t time = 0, buy = 0;
    int64_t ts = dt.parse(f[2]);
    if (ts >= w.start && ts < w.end && ingest * 1000 <= ts - w.start + day_ms && !f[1].empty() &&
        !f[5].empty()) {
        bool good = f[4] == "view" || f[4] == "cart";
        if (!good && f[4] == "purchase") {
            int64_t cents = decimal(f[6], 10000000);
            if (cents > 0) {
                buy = uint32_t(cents) | (dim.category(f[5]) << 24);
                good = true;
            }
        }
        if (good) {
            user = keys.encode(f[1]);
            time = uint32_t(ts - w.start + week_ms);
        }
    }
    store_event(id, user, ingest, time, buy, lane, owner);
}

#if defined(__AVX2__) && defined(__BMI2__)
inline uint32_t commas32(const char* p) {
    return _mm256_movemask_epi8(
        _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)), _mm256_set1_epi8(',')));
}
template <unsigned n>
inline bool plain_event(const char* p, const char* end, unsigned a, unsigned b, Window w, const Dates& dt,
                        Keys& keys, const Dimensions& dim, Lane& lane, unsigned owner) {
    unsigned qe = *p == '"', qu = p[a + 1] == '"';
    const char* raw_t = p + b + 1;
    unsigned qt = *raw_t == '"';
    const char* t = raw_t + qt;
    if (t + n + qt + 22 > end || t[n + qt] != ',')
        return false;
    const char* raw_i = t + n + qt + 1;
    unsigned qi = *raw_i == '"';
    const char* ip = raw_i + qi;
    if (ip + 20 + qi >= end || ip[20 + qi] != ',')
        return false;
    const char* raw_type = ip + 21 + qi;
    unsigned qkind = *raw_type == '"';
    const char* kind = raw_type + qkind;
    int64_t ts, ing;
    if constexpr (n == 20)
        dt.paired_utc(t, ip, ts, ing);
    else {
        ts = dt.padded(t, n);
        ing = dt.relative_ingest(ip);
    }
    if (ing == bad_time)
        return false;
    Key id = keys.clean_unordered(p + qe, a - 2 * qe), user = 0;
    if (!id)
        return true;
    uint32_t time = 0, buy = 0;
    if (ts == bad_time) {
        store_event(id, 0, ing, 0, 0, lane, owner);
        return true;
    }
    size_t user_size = b - a - 1 - 2 * qu;
    const uint64_t rel_ts = uint64_t(ts - w.start);
    if (user_size && rel_ts < uint64_t(w.end - w.start) && ing * 1000 <= int64_t(rel_ts) + day_ms) {
        uint32_t tag;
        std::memcpy(&tag, kind, 4);
        if ((tag == 0x77656976u || tag == 0x74726163u) && kind[4 + qkind] == ',') {
            const char* product = kind + 5 + qkind;
            if (product[0] != ',' && !(product[0] == '"' && product[1] == '"'))
                user = keys.clean(p + a + 1 + qu, user_size);
        } else if (end - kind >= 10 && read64(kind) == 0x6573616863727570ULL && kind[8 + qkind] == ',') {
            const char* product = kind + 9 + qkind;
            const char* price = static_cast<const char*>(std::memchr(product, ',', end - product));
            if (price) {
                unsigned qp = *product == '"', qv = price[1] == '"';
                View product_id(product + qp, price - product - 2 * qp);
                int64_t cents = decimal(View(price + 1 + qv, end - price - 1 - 2 * qv), 10000000);
                if (cents > 0 && !product_id.empty()) {
                    buy = uint32_t(cents) | (dim.category(product_id) << 24);
                    user = keys.clean(p + a + 1 + qu, user_size);
                }
            }
        }
        time = uint32_t(rel_ts + uint64_t(week_ms));
    }
    store_event(id, user, ing, time, buy, lane, owner);
    return true;
}
template <class Slow>
inline void parse_events(View chunk, Window w, const Dates& dt, Keys& keys, const Dimensions& dim,
                         Lane& lane, unsigned owner, Slow&& slow) {
    struct Line {
        const char* p;
        const char* end;
        unsigned a, b;
    };
    std::array<std::array<Line, 32>, 3> batch;
    std::array<unsigned, 3> counts{};
    auto flush = [&]<unsigned format>() {
        constexpr unsigned widths[] = {13, 20, 25};
        for (unsigned i = 0; i < counts[format]; ++i) {
            const Line& x = batch[format][i];
            if (!plain_event<widths[format]>(x.p, x.end, x.a, x.b, w, dt, keys, dim, lane, owner))
                scalar_rows<7>(View(x.p, x.end - x.p), slow);
        }
        counts[format] = 0;
    };
    auto finish = [&](unsigned f) {
        if (f == 0)
            flush.template operator()<0>();
        else if (f == 1)
            flush.template operator()<1>();
        else
            flush.template operator()<2>();
    };
    plain_rows<7>(
        chunk,
        [&](const char* p, const char* end) {
            uint32_t mask = commas32(p);
            unsigned a = std::countr_zero(mask), b = std::countr_zero(mask & (mask - 1));
            if (a == 0 || b >= 32 || end - p < int(b) + 38)
                return false;
            const char* t = p + b + 1 + (p[b + 1] == '"');
            unsigned iso = t[4] == '-';
            unsigned format = iso + (iso & (t[19] != 'Z'));
            batch[format][counts[format]++] = {p, end, a, b};
            if (counts[format] == batch[format].size())
                finish(format);
            return true;
        },
        slow);
    for (unsigned i = 0; i < 3; ++i)
        finish(i);
}
#endif

// Resolve users in batches so independent lookups can reach memory in parallel.
inline void parse_clicks(View chunk, Window w, const Dates& dt, Keys& keys, const Dimensions& dim,
                         People& people, Lane& lane) {
    struct Pending {
        Key id, user;
        uint32_t time, campaign;
    };
    std::array<Pending, 32> batch;
    size_t n = 0;
    auto flush = [&] {
        for (size_t i = 0; i < n; ++i) {
            const auto& r = batch[i];
            uint32_t name = people.number(r.user);
            lane.ads.add(name & (buckets - 1), {r.id, name, r.time, r.campaign});
        }
        n = 0;
    };
    auto push = [&](Key id, Key user, int64_t ts, uint32_t campaign) {
        people.prefetch(user);
        batch[n++] = {id, user, uint32_t(ts - w.start + week_ms), campaign};
        if (n == batch.size())
            flush();
    };
    auto slow = [&](const auto& f) {
        if (f[0].empty() || f[1].empty())
            return;
        uint32_t campaign = dim.campaign_index(f[2]);
        if (campaign == UINT32_MAX)
            return;
        int64_t ts = dt.parse(f[3]);
        if (ts >= w.start - week_ms && ts < w.end)
            push(keys.encode(f[0]), keys.encode(f[1]), ts, campaign);
    };
#if defined(__AVX2__) && defined(__BMI2__)
    plain_rows<4>(
        chunk,
        [&](const char* p, const char* end) {
            uint32_t mask = commas32(p);
            unsigned a = std::countr_zero(mask);
            mask &= mask - 1;
            unsigned b = std::countr_zero(mask);
            mask &= mask - 1;
            unsigned c = std::countr_zero(mask);
            if (c >= 32 || p + c >= end)
                return false;
            unsigned qa = *p == '"', qb = p[a + 1] == '"', qc = p[b + 1] == '"', qt = p[c + 1] == '"';
            unsigned ni = a - 2 * qa, nu = b - a - 1 - 2 * qb;
            if (!ni || !nu)
                return true;
            uint32_t campaign = dim.campaign_index(View(p + b + 1 + qc, c - b - 1 - 2 * qc));
            if (campaign == UINT32_MAX)
                return true;
            int64_t ts = dt.padded(p + c + 1 + qt, end - p - c - 1 - 2 * qt);
            if (ts >= w.start - week_ms && ts < w.end)
                push(keys.clean(p + qa, ni), keys.clean(p + a + 1 + qb, nu), ts, campaign);
            return true;
        },
        slow);
#else
    rows<4>(chunk, slow);
#endif
    flush();
}

inline void filter(unsigned part, std::vector<Lane>& lanes, Lane& dst, People& people,
                   std::vector<uint32_t>& table, std::vector<Raw*>& blocks) {
    size_t n = 0;
    for (auto& lane : lanes)
        n += lane.input.size(part);
    if (!n)
        return;
    size_t mask = std::bit_ceil(n + n / 2 + 1) - 1;
    if (n + lanes.size() * records_per_block + 1 > UINT32_MAX)
        throw std::length_error("event partition too large");
    uint32_t index_mask = std::bit_ceil(uint64_t(n + lanes.size() * records_per_block + 1)) - 1;
    table.assign(mask + 1, 0);
    blocks.clear();
    auto each = [&](auto&& f) {
        for (auto& lane : lanes)
            lane.input.blocks(part, f);
    };
    auto detail = [&](const Raw& r) -> const Detail& {
        return lanes[r.data >> 32].details[uint32_t(r.data) ^ detail_bit];
    };
    auto ingest = [&](const Raw& r) {
        return (uint32_t(r.data) & detail_bit) ? detail(r).ingest : int64_t(int32_t(r.data >> 32));
    };
    auto emit_new = [&](Raw& r) {
        Key user = r.user;
        if (!user) {
            r.emitted = nullptr;
            return;
        }
        uint32_t name = people.number(user), time = uint32_t(r.data), buy = 0;
        if (time & detail_bit) {
            auto& d = detail(r);
            time = d.time;
            buy = d.purchase;
        }
        r.emitted = &dst.hits.add(name & (buckets - 1), {name, time, buy});
    };
    auto emit_replace = [&](Raw& r, Hit* earlier) {
        Key user = r.user;
        if (!user) {
            if (earlier)
                earlier->time = 0;
            r.emitted = nullptr;
            return;
        }
        uint32_t name = people.number(user), time = uint32_t(r.data), buy = 0;
        if (time & detail_bit) {
            auto& d = detail(r);
            time = d.time;
            buy = d.purchase;
        }
        if (earlier && earlier->user == name) {
            *earlier = {name, time, buy};
            r.emitted = earlier;
        } else {
            if (earlier)
                earlier->time = 0;
            r.emitted = &dst.hits.add(name & (buckets - 1), {name, time, buy});
        }
    };
    constexpr size_t pf = 16;
    each([&](auto block) {
        uint32_t base = uint32_t(blocks.size()) * records_per_block;
        blocks.push_back(block.data());
        const size_t sz = block.size();
        const size_t init = std::min(sz, pf);
        for (size_t k = 0; k < init; ++k) {
            people.prefetch(block[k].user);
            __builtin_prefetch(&table[(uint32_t(block[k].id) >> id_bits) & mask], 1, 1);
        }
        for (size_t j = 0; j < sz; ++j) {
            if (j + pf < sz) {
                people.prefetch(block[j + pf].user);
                __builtin_prefetch(&table[(uint32_t(block[j + pf].id) >> id_bits) & mask], 1, 1);
            }
            Raw& r = block[j];
            uint32_t h = uint32_t(r.id);
            uint32_t tag = uint32_t(r.id >> 32) & ~index_mask;
            uint32_t entry = tag | (base + uint32_t(j) + 1);
            size_t i = (h >> id_bits) & mask;
            for (;;) {
                uint32_t old = table[i];
                if (!old) {
                    table[i] = entry;
                    emit_new(r);
                    break;
                }
                if ((old & ~index_mask) == tag) {
                    uint32_t code = (old & index_mask) - 1;
                    auto& previous = blocks[code / records_per_block][code % records_per_block];
                    if (previous.id == r.id) {
                        if (ingest(r) < ingest(previous)) {
                            emit_replace(r, previous.emitted);
                            table[i] = entry;
                        }
                        break;
                    }
                }
                i = (i + 1) & mask;
            }
        }
    });
}

struct Ranges {
    uint32_t ne = 0, nc = 0, ee = 0, ce = 0;
};
class Totals {
    uint32_t channels_;
    int32_t first_;
    uint32_t day_bias_;
    [[gnu::always_inline]] inline uint32_t day_index(uint32_t t) const {
        return (t - uint32_t(week_ms) + day_bias_) / uint32_t(day_ms);
    }
    struct Session {
        uint32_t start, day, end_of_day = 0, best_time = 0, ad = 0;
        Key best_id = 0;
        int64_t revenue = 0;
    };
    std::vector<uint32_t> seen_, touched_, categories_, starts_, slots_;
    std::vector<Session> sessions_;
    std::vector<uint64_t> masks_, mask1_;
    uint32_t serial_ = 0;
    std::vector<int64_t> amounts_;
    struct Bin {
        uint32_t low, high, count;
        int64_t revenue;
    };
    std::vector<Bin> bins_;
    std::vector<uint64_t> occupied_;
    struct Purchase {
        uint32_t bin, buy;
    };
    std::vector<Purchase> bought_;
    std::vector<uint32_t> day_row_, bin_day_, reset_amounts_;
    std::vector<int64_t> daily_amounts_, best_amount_;
    void bin_person(Key name, std::span<GroupHit> hits, std::span<GroupAd> ads, const Keys& keys,
                    int64_t, std::vector<UserDay>& out) {
        const size_t nc = amounts_.size();
        if (bins_.empty()) {
            // The compact path spans fewer than 2^31 milliseconds, so bounded
            // bins shorter than one session gap cover every event.
            size_t capacity = (uint64_t(detail_bit) >> 20) + 1;
            bins_.resize(capacity);
            bin_day_.resize(capacity);
            occupied_.resize((capacity + 63) / 64);
            auto days = by_day.size() / channels_;
            day_row_.resize(days);
            daily_amounts_.resize(days * nc);
            best_amount_.resize(days);
        }
        sessions_.clear();
        starts_.clear();
        bought_.clear();
        unsigned first = occupied_.size(), last = 0;
        for (uint64_t v : hits) {
            uint32_t t = v >> 32, i = t >> 20, word = i >> 6;
            uint64_t bit = uint64_t{1} << (i & 63);
            Bin& b = bins_[i];
            if (!(occupied_[word] & bit)) {
                occupied_[word] |= bit;
                b = {t, t, 0, 0};
                first = std::min(first, word);
                last = std::max(last, word);
            } else {
                b.low = std::min(b.low, t);
                b.high = std::max(b.high, t);
            }
            ++b.count;
            uint32_t buy = v;
            if (buy) {
                b.revenue += buy & 0xffffff;
                bought_.push_back({i, buy});
            }
        }
        uint32_t prev = 0, day = 0;
        int32_t previous_day = INT32_MIN;
        for (unsigned w = first; w <= last; ++w) {
            uint64_t bits = occupied_[w];
            occupied_[w] = 0;
            while (bits) {
                unsigned i = w * 64 + std::countr_zero(bits);
                bits &= bits - 1;
                auto& bin = bins_[i];
                if (sessions_.empty() || bin.low - prev > gap_ms) {
                    day = day_index(bin.low);
                    int32_t date = first_ + int32_t(day);
                    if (date != previous_day) {
                        previous_day = date;
                        day_row_[day] = out.size();
                        best_amount_[day] = 0;
                        out.push_back({name, date, no_category, 0, 0, 0});
                    }
                    ++out.back().sessions;
                    starts_.push_back(bin.low);
                    sessions_.push_back({bin.low, day, 0, 0, 0, 0, 0});
                }
                prev = bin.high;
                bin_day_[i] = day;
                sessions_.back().revenue += bin.revenue;
                out.back().events += bin.count;
                out.back().revenue += bin.revenue;
            }
        }
        for (auto purchase : bought_) {
            uint32_t d = bin_day_[purchase.bin], cat = purchase.buy >> 24;
            size_t i = d * nc + cat;
            if (!daily_amounts_[i])
                reset_amounts_.push_back(i);
            int64_t value = (daily_amounts_[i] += purchase.buy & 0xffffff);
            auto& row = out[day_row_[d]];
            if (value > best_amount_[d] || (value == best_amount_[d] && cat < row.category)) {
                best_amount_[d] = value;
                row.category = cat;
            }
        }
        for (auto i : reset_amounts_)
            daily_amounts_[i] = 0;
        reset_amounts_.clear();
        if (ads.empty()) {
            for (const auto& session : sessions_) {
                auto& credit = by_day[size_t(session.day) * channels_];
                ++credit.sessions;
                credit.purchasers += session.revenue > 0;
                credit.revenue += session.revenue;
            }
        } else {
            attribute(ads, keys);
        }
    }

  public:
    std::vector<Counts> by_day;
    std::vector<int64_t> assists;
    Totals(const Dimensions& d, Window w)
        : channels_(d.campaigns.size()), first_(utc_day(w.start)),
          day_bias_(uint32_t(w.start - int64_t(first_) * day_ms)), seen_(channels_), mask1_(channels_),
          amounts_(d.categories.size()), by_day(size_t(utc_day(w.end - 1) - first_ + 1) * channels_),
          assists(by_day.size()) {}
    void person(Key name, std::span<GroupHit> hits, std::span<GroupAd> ads, const Keys& keys,
                int64_t origin, std::vector<UserDay>& out) {
        const size_t nh = hits.size();
        if (nh == 1) {
            uint64_t v = hits[0];
            uint32_t start = uint32_t(v >> 32), buy = uint32_t(v);
            uint32_t d = day_index(start);
            uint32_t cat = buy ? (buy >> 24) : no_category;
            int64_t rev = buy ? int64_t(buy & 0xffffff) : 0;
            out.push_back({name, first_ + int32_t(d), cat, 1, 1, rev});
            size_t day_off = size_t(d) * channels_;
            uint32_t winner = 0;
            if (!ads.empty()) {
                uint32_t best = 0;
                Key id = 0;
                if (++serial_ == 0) {
                    std::fill(seen_.begin(), seen_.end(), 0);
                    ++serial_;
                }
                for (const GroupAd& a : ads) {
                    if (a.time > start || start - a.time > week_ms)
                        continue;
                    if (seen_[a.campaign] != serial_) {
                        seen_[a.campaign] = serial_;
                        ++assists[day_off + a.campaign];
                    }
                    if (!winner || a.time > best || (a.time == best && keys.less(a.id, id))) {
                        winner = a.campaign;
                        best = a.time;
                        id = a.id;
                    }
                }
            }
            auto& credit = by_day[day_off + winner];
            ++credit.sessions;
            credit.purchasers += rev > 0;
            credit.revenue += rev;
            return;
        }
        if (nh >= 48) {
            bin_person(name, hits, ads, keys, origin, out);
            return;
        }
        if (nh == 2) {
            if (hits[1] < hits[0])
                std::swap(hits[0], hits[1]);
        } else if (nh <= 24) {
            for (size_t k = 1; k < nh; ++k) {
                uint64_t key = hits[k];
                size_t j = k;
                while (j > 0 && hits[j - 1] > key) {
                    hits[j] = hits[j - 1];
                    --j;
                }
                hits[j] = key;
            }
        } else {
            std::sort(hits.begin(), hits.end());
        }
        sessions_.clear();
        starts_.clear();
        size_t i = 0;
        uint32_t cur_d = day_index(uint32_t(hits.front() >> 32));
        uint32_t next_day_t = (cur_d + 1) * uint32_t(day_ms) + uint32_t(week_ms) - day_bias_;
        UserDay row{name, first_ + int32_t(cur_d), no_category, 0, 0, 0};
        auto flush = [&] {
            if (!categories_.empty()) {
                int64_t best = 0;
                for (auto c : categories_) {
                    auto a = amounts_[c];
                    if (a > best || (a == best && c < row.category)) {
                        best = a;
                        row.category = c;
                    }
                    amounts_[c] = 0;
                }
                categories_.clear();
            }
            out.push_back(row);
        };
        while (i < nh) {
            size_t begin = i;
            uint32_t start = uint32_t(hits[i] >> 32), prev;
            if (start >= next_day_t) {
                flush();
                cur_d = day_index(start);
                next_day_t = (cur_d + 1) * uint32_t(day_ms) + uint32_t(week_ms) - day_bias_;
                row = {name, first_ + int32_t(cur_d), no_category, 0, 0, 0};
            }
            int64_t revenue = 0;
            do {
                uint64_t v = hits[i++];
                prev = uint32_t(v >> 32);
                uint32_t buy = uint32_t(v);
                if (buy) {
                    uint32_t cat = buy >> 24, cents = buy & 0xffffff;
                    if (!amounts_[cat])
                        categories_.push_back(cat);
                    amounts_[cat] += cents;
                    revenue += cents;
                }
            } while (i < nh && uint32_t(hits[i] >> 32) - prev <= gap_ms);
            ++row.sessions;
            row.events += i - begin;
            row.revenue += revenue;
            if (ads.empty()) {
                auto& credit = by_day[size_t(cur_d) * channels_];
                ++credit.sessions;
                credit.purchasers += revenue > 0;
                credit.revenue += revenue;
            } else {
                starts_.push_back(start);
                sessions_.push_back({start, cur_d, 0, 0, 0, 0, revenue});
            }
        }
        flush();
        if (!ads.empty())
            attribute(ads, keys);
    }

  private:
    static uint64_t below(unsigned bit) { return bit == 64 ? ~uint64_t{0} : (uint64_t{1} << bit) - 1; }
    // At most one session starts in a bin shorter than the session gap. For long
    // session lists, one lookup and one comparison give an exact lower bound.
    uint32_t lower_bound(uint64_t t) const {
        if (t <= starts_.front())
            return 0;
        if (t > starts_.back())
            return starts_.size();
        if (!slots_.empty()) {
            uint32_t pos = slots_[(t - starts_.front()) >> 20];
            return pos + (starts_[pos] < t);
        }
        return std::lower_bound(starts_.begin(), starts_.end(), uint32_t(t)) - starts_.begin();
    }
    void attribute(std::span<GroupAd> ads, const Keys& keys) {
        uint32_t n = starts_.size(), words = (n + 63) / 64;
        if (n == 1) {
            const auto& session = sessions_.front();
            size_t day = size_t(session.day) * channels_;
            uint32_t winner = 0, best = 0;
            Key id = 0;
            if (++serial_ == 0) {
                std::fill(seen_.begin(), seen_.end(), 0);
                ++serial_;
            }
            for (const GroupAd& a : ads) {
                if (a.time > session.start || session.start - a.time > week_ms)
                    continue;
                if (seen_[a.campaign] != serial_) {
                    seen_[a.campaign] = serial_;
                    ++assists[day + a.campaign];
                }
                if (!winner || a.time > best || (a.time == best && keys.less(a.id, id))) {
                    winner = a.campaign;
                    best = a.time;
                    id = a.id;
                }
            }
            auto& credit = by_day[day + winner];
            ++credit.sessions;
            credit.purchasers += session.revenue > 0;
            credit.revenue += session.revenue;
            return;
        }
        if (!ads.empty()) {
            if (++serial_ == 0) {
                std::fill(seen_.begin(), seen_.end(), 0);
                ++serial_;
            }
            touched_.clear();
            if (words == 1) {
                slots_.clear();
                if (n > 32 && ads.size() >= 64) {
                    size_t slots = ((starts_.back() - starts_.front()) >> 20) + 1;
                    slots_.resize(slots);
                    size_t at = 0;
                    for (size_t j = 0; j < slots; ++j) {
                        uint64_t t = starts_.front() + (j << 20);
                        while (starts_[at] < t)
                            ++at;
                        slots_[j] = at;
                    }
                }
#if defined(__AVX512F__)
                if (n <= 16) {
                    uint16_t valid_mask = uint16_t((1u << n) - 1u);
                    __m512i sv = _mm512_mask_loadu_epi32(_mm512_set1_epi32(-1), valid_mask, starts_.data());
                    for (const GroupAd& ad : ads) {
                        uint32_t lo = uint32_t(std::popcount(
                            uint32_t(_mm512_cmplt_epu32_mask(sv, _mm512_set1_epi32(int32_t(ad.time))))));
                        if (lo == n)
                            continue;
                        uint32_t hi_t = ad.time + uint32_t(week_ms);
                        if (hi_t < starts_[lo])
                            continue;
                        uint32_t hi = uint32_t(std::popcount(
                            uint32_t(_mm512_cmple_epu32_mask(sv, _mm512_set1_epi32(int32_t(hi_t))))));
                        auto& winner = sessions_[lo];
                        if (!winner.ad || ad.time > winner.best_time ||
                            (ad.time == winner.best_time && keys.less(ad.id, winner.best_id))) {
                            winner.best_time = ad.time;
                            winner.ad = ad.campaign;
                            winner.best_id = ad.id;
                        }
                        uint64_t range_bits = (~uint64_t{0} << lo) & below(hi);
                        if (seen_[ad.campaign] != serial_) {
                            seen_[ad.campaign] = serial_;
                            touched_.push_back(ad.campaign);
                            mask1_[ad.campaign] = range_bits;
                        } else {
                            mask1_[ad.campaign] |= range_bits;
                        }
                    }
                } else if (n <= 32) {
                    uint16_t valid_mask = uint16_t((1u << (n - 16)) - 1u);
                    __m512i sv0 = _mm512_loadu_si512(starts_.data());
                    __m512i sv1 = _mm512_mask_loadu_epi32(_mm512_set1_epi32(-1), valid_mask, starts_.data() + 16);
                    for (const GroupAd& ad : ads) {
                        __m512i vt = _mm512_set1_epi32(int32_t(ad.time));
                        uint32_t lo = uint32_t(std::popcount(uint32_t(_mm512_cmplt_epu32_mask(sv0, vt)))) +
                                      uint32_t(std::popcount(uint32_t(_mm512_cmplt_epu32_mask(sv1, vt))));
                        if (lo == n)
                            continue;
                        uint32_t hi_t = ad.time + uint32_t(week_ms);
                        if (hi_t < starts_[lo])
                            continue;
                        __m512i vh = _mm512_set1_epi32(int32_t(hi_t));
                        uint32_t hi = uint32_t(std::popcount(uint32_t(_mm512_cmple_epu32_mask(sv0, vh)))) +
                                      uint32_t(std::popcount(uint32_t(_mm512_cmple_epu32_mask(sv1, vh))));
                        auto& winner = sessions_[lo];
                        if (!winner.ad || ad.time > winner.best_time ||
                            (ad.time == winner.best_time && keys.less(ad.id, winner.best_id))) {
                            winner.best_time = ad.time;
                            winner.ad = ad.campaign;
                            winner.best_id = ad.id;
                        }
                        uint64_t range_bits = (~uint64_t{0} << lo) & below(hi);
                        if (seen_[ad.campaign] != serial_) {
                            seen_[ad.campaign] = serial_;
                            touched_.push_back(ad.campaign);
                            mask1_[ad.campaign] = range_bits;
                        } else {
                            mask1_[ad.campaign] |= range_bits;
                        }
                    }
                } else
#endif
                {
                    for (const GroupAd& ad : ads) {
                        uint32_t lo = lower_bound(ad.time);
                        if (lo == n || uint64_t(ad.time) + week_ms < starts_[lo])
                            continue;
                        uint32_t hi = lower_bound(uint64_t(ad.time) + week_ms + 1);
                        auto& winner = sessions_[lo];
                        if (!winner.ad || ad.time > winner.best_time ||
                            (ad.time == winner.best_time && keys.less(ad.id, winner.best_id))) {
                            winner.best_time = ad.time;
                            winner.ad = ad.campaign;
                            winner.best_id = ad.id;
                        }
                        uint64_t range_bits = (~uint64_t{0} << lo) & below(hi);
                        if (seen_[ad.campaign] != serial_) {
                            seen_[ad.campaign] = serial_;
                            touched_.push_back(ad.campaign);
                            mask1_[ad.campaign] = range_bits;
                        } else {
                            mask1_[ad.campaign] |= range_bits;
                        }
                    }
                }
                if (!touched_.empty()) {
                    for (uint32_t end = n, j = n; j--;) {
                        if (j + 1 < n && sessions_[j].day != sessions_[j + 1].day)
                            end = j + 1;
                        sessions_[j].end_of_day = end;
                    }
                    for (uint32_t c : touched_) {
                        uint64_t bits = mask1_[c];
                        while (bits) {
                            const Session& first = sessions_[std::countr_zero(bits)];
                            uint64_t daymask = below(first.end_of_day);
                            assists[size_t(first.day) * channels_ + c] += std::popcount(bits & daymask);
                            bits &= ~daymask;
                        }
                    }
                }
            } else {
                slots_.clear();
                if (ads.size() >= 64) {
                size_t slots = ((starts_.back() - starts_.front()) >> 20) + 1;
                slots_.resize(slots);
                size_t at = 0;
                for (size_t j = 0; j < slots; ++j) {
                    uint64_t t = starts_.front() + (j << 20);
                    while (starts_[at] < t)
                        ++at;
                    slots_[j] = at;
                }
                }
                if (masks_.size() < size_t(channels_) * words)
                    masks_.resize(size_t(channels_) * words);
                for (const GroupAd& ad : ads) {
                    uint32_t lo = lower_bound(ad.time);
                    if (lo == n || uint64_t(ad.time) + week_ms < starts_[lo])
                        continue;
                    uint32_t hi = lower_bound(uint64_t(ad.time) + week_ms + 1);
                    auto& winner = sessions_[lo];
                    if (!winner.ad || ad.time > winner.best_time ||
                        (ad.time == winner.best_time && keys.less(ad.id, winner.best_id))) {
                        winner.best_time = ad.time;
                        winner.ad = ad.campaign;
                        winner.best_id = ad.id;
                    }
                    uint64_t* m = masks_.data() + size_t(ad.campaign) * words;
                    if (seen_[ad.campaign] != serial_) {
                        seen_[ad.campaign] = serial_;
                        touched_.push_back(ad.campaign);
                        std::fill_n(m, words, 0);
                    }
                    unsigned first = lo / 64, last = (hi - 1) / 64;
                    uint64_t l = ~uint64_t{0} << (lo & 63), r = below(((hi - 1) & 63) + 1);
                    if (first == last)
                        m[first] |= l & r;
                    else {
                        m[first] |= l;
                        std::fill(m + first + 1, m + last, ~uint64_t{0});
                        m[last] |= r;
                    }
                }
                if (!touched_.empty()) {
                    for (uint32_t end = n, j = n; j--;) {
                        if (j + 1 < n && sessions_[j].day != sessions_[j + 1].day)
                            end = j + 1;
                        sessions_[j].end_of_day = end;
                    }
                    for (uint32_t c : touched_) {
                        const uint64_t* m = masks_.data() + size_t(c) * words;
                        for (unsigned j = 0; j < words; ++j) {
                            uint64_t bits = m[j];
                            unsigned base = j * 64;
                            while (bits) {
                                const Session& first = sessions_[base + std::countr_zero(bits)];
                                uint64_t daymask = below(std::min(64u, first.end_of_day - base));
                                assists[size_t(first.day) * channels_ + c] += std::popcount(bits & daymask);
                                bits &= ~daymask;
                            }
                        }
                    }
                }
            }
        }
        uint32_t ad = 0, last_time = 0;
        for (const auto& session : sessions_) {
            if (session.ad) {
                ad = session.ad;
                last_time = session.best_time;
            }
            if (session.start - last_time > week_ms)
                ad = 0;
            auto& credit = by_day[size_t(session.day) * channels_ + ad];
            ++credit.sessions;
            credit.purchasers += session.revenue > 0;
            credit.revenue += session.revenue;
        }
    }
};

struct GroupBuffers {
    std::vector<Ranges> ranges;
    std::unique_ptr<GroupHit[]> hits;
    std::unique_ptr<GroupAd[]> ads;
    size_t event_capacity = 0, click_capacity = 0;
    template <class T> static void reserve(std::unique_ptr<T[]>& buffer, size_t& capacity, size_t n) {
        if (n > capacity || !buffer) {
            capacity = std::max(size_t(1), std::bit_ceil(n));
            buffer.reset(new T[capacity]);
        }
    }
};

inline void aggregate(unsigned part, std::vector<Lane>& lanes, Totals& totals, const People& people,
                      const Keys& keys, int64_t origin, std::vector<UserDay>& out, GroupBuffers& scratch) {
    size_t nh = 0, na = 0;
    for (auto& lane : lanes) {
        nh += lane.hits.size(part);
        na += lane.ads.size(part);
    }
    if (!nh)
        return;
    out.reserve(nh / 8 + 32);
    auto& groups = scratch.ranges;
    groups.assign(people.local_bound(part), {});
    for (auto& lane : lanes) {
        lane.hits.blocks(part, [&](auto b) {
            for (auto r : b)
                if (r.time)
                    ++groups[r.user >> bucket_bits].ne;
        });
        lane.ads.blocks(part, [&](auto b) {
            for (auto r : b)
                ++groups[r.user >> bucket_bits].nc;
        });
    }
    uint32_t e = 0, c = 0;
    for (auto& g : groups) {
        g.ee = e;
        g.ce = c;
        e += g.ne;
        c += g.nc;
    }
    auto& hits = scratch.hits;
    auto& ads = scratch.ads;
    GroupBuffers::reserve(hits, scratch.event_capacity, e);
    GroupBuffers::reserve(ads, scratch.click_capacity, c);
    for (auto& lane : lanes) {
        lane.hits.blocks(part, [&](auto b) {
            for (auto r : b)
                if (r.time)
                    hits[groups[r.user >> bucket_bits].ee++] = (uint64_t(r.time) << 32) | r.purchase;
        });
        lane.ads.blocks(part, [&](auto b) {
            for (auto r : b)
                ads[groups[r.user >> bucket_bits].ce++] = {r.id, r.time, r.campaign};
        });
    }
    for (size_t i = 0; i < groups.size(); ++i) {
        auto g = groups[i];
        if (g.ne)
            totals.person(people.name((i << bucket_bits) | part), {hits.get() + g.ee - g.ne, g.ne},
                          {ads.get() + g.ce - g.nc, g.nc}, keys, origin, out);
    }
}

template <class Timer>
void run(Input& events, Input& clicks, Input& links, const fs::path& out, Window w, const Dates& dt,
         Keys& keys, const Dimensions& dim, unsigned cpu, Timer mark) {
    unsigned workers = std::min<size_t>(cpu, events.chunks.size() + clicks.chunks.size() + 1);
    std::vector<Lane> lanes(workers);
    std::promise<std::shared_ptr<People>> ready;
    auto people_future = ready.get_future().share();
    size_t ne = events.chunks.size(), nc = clicks.chunks.size();
    jobs(ne + nc + 1, workers, [&](unsigned t, size_t j) {
        if (!j) {
            try {
                ready.set_value(std::make_shared<People>(links, keys, events.size + clicks.size, cpu));
            } catch (...) {
                ready.set_exception(std::current_exception());
                throw;
            }
        } else if (j <= ne) {
            auto slow = [&](const auto& f) { put_event(f, w, dt, keys, dim, lanes[t], t); };
#if defined(__AVX2__) && defined(__BMI2__)
            parse_events(events.chunks[j - 1], w, dt, keys, dim, lanes[t], t, slow);
#else
            rows<7>(events.chunks[j-1],slow);
#endif
        } else {
            auto& people = *people_future.get();
            parse_clicks(clicks.chunks[j - ne - 1], w, dt, keys, dim, people, lanes[t]);
        }
    });
    mark("parsed input and links");
    auto& people = *people_future.get();
    std::vector<std::vector<uint32_t>> filter_tables(workers);
    std::vector<std::vector<Raw*>> filter_blocks(workers);
    jobs(id_buckets + 1, workers, [&](unsigned t, size_t job) {
        if (job)
            filter(job - 1, lanes, lanes[t], people, filter_tables[t], filter_blocks[t]);
        else {
            events.files.clear();
            clicks.files.clear();
            links.files.clear();
        }
    });
    mark("deduplicated and joined");
    unsigned writers = std::min(workers, Output::parts);
    Output writer(out, keys, dim);
    std::vector<std::vector<UserDay>> users(writers);
    std::vector<Totals> totals;
    for (unsigned i = 0; i < workers; ++i)
        totals.emplace_back(dim, w);
    std::vector<GroupBuffers> buffers(writers);
    std::array<std::pair<size_t, unsigned>, buckets> order;
    for (unsigned part = 0; part < buckets; ++part) {
        size_t cost = 0;
        for (auto& lane : lanes)
            cost += lane.hits.size(part) + lane.ads.size(part);
        order[part] = {cost, part};
    }
    std::sort(order.rbegin(), order.rend());
    jobs(buckets + workers, writers, [&](unsigned t, size_t job) {
        if (job < workers)
            lanes[job].input.clear();
        else {
            unsigned part = order[job - workers].second;
            auto& rows = users[t];
            rows.clear();
            aggregate(part, lanes, totals[t], people, keys, w.start - week_ms, rows, buffers[t]);
            writer.append(t, rows);
        }
    });
    mark("grouped and aggregated");
    CampaignDays result;
    for (const auto& ent : totals)
        for (size_t i = 0; i < ent.by_day.size(); ++i) {
            Counts a = ent.by_day[i];
            a.assists = ent.assists[i];
            if (a.sessions || a.assists)
                result[campaign_day(i % dim.campaigns.size(),
                                    utc_day(w.start) + i / dim.campaigns.size())] += a;
        }
    writer.finish(result, cpu);
    mark("wrote parquet");
}
} // namespace shop::compact
