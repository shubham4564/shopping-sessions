#pragma once
#include "plain.hpp"

namespace shop {
class People {
    struct Slot {
        Key id, value;
    };
    struct Component {
        uint32_t parent, count;
        Key name;
    };
    struct UnmapSlots {
        size_t bytes;
        void operator()(Slot* p) const { munmap(p, bytes); }
    };
    using SlotStorage = std::unique_ptr<Slot[], UnmapSlots>;
    SlotStorage storage_{nullptr, UnmapSlots{0}};
    std::span<Slot> table_;
    std::vector<Component> nodes_;
    struct alignas(64) ShardSize {
        std::atomic<uint32_t> count{0};
    };
    std::unique_ptr<ShardSize[]> sizes_{new ShardSize[256]};
    std::unique_ptr<Key[]> names_;

    uint32_t record_name(size_t slot, Key name) {
        unsigned shard = slot & 255;
        uint32_t sequence = sizes_[shard].count.fetch_add(1, std::memory_order_relaxed);
        names_[shard * (table_.size() / 256) + sequence] = name;
        return (sequence << 8) | shard;
    }
    size_t mask_ = 0, probe_limit_ = 32;
    struct Overflow {
        std::mutex lock;
        std::unordered_map<Key, uint32_t> codes;
        std::vector<Key> names;
    };
    std::unique_ptr<Overflow[]> overflow_{new Overflow[256]};
    static size_t hash_user(Key k) {
#if defined(__SSE4_2__)
        return _mm_crc32_u64(0, k);
#else
        return mix(k);
#endif
    }

    size_t locate(Key id) const {
        size_t at = hash_user(id) & mask_;
        while (table_[at].id && table_[at].id != id)
            at = (at + 1) & mask_;
        return at;
    }
    void resize(size_t n) {
        if (n > SIZE_MAX / sizeof(Slot))
            throw std::bad_alloc();
        size_t bytes = n * sizeof(Slot);
        void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
            throw std::bad_alloc();
        madvise(mem, bytes, MADV_HUGEPAGE);
        // Anonymous pages start at zero. Start the trivial objects' lifetimes
        // without touching all pages on the graph coordinator thread.
        SlotStorage fresh(::new (mem) Slot[n], UnmapSlots{bytes});
        auto previous = std::move(storage_);
        auto old = table_;
        storage_ = std::move(fresh);
        table_ = {storage_.get(), n};
        mask_ = n - 1;
        for (auto slot : old)
            if (slot.id)
                table_[locate(slot.id)] = slot;
    }
    uint32_t index(Key k) {
        if (nodes_.size() * 4 >= table_.size() * 3)
            resize(table_.size() * 2);
        auto& slot = table_[locate(k)];
        if (!slot.id) {
            uint32_t i = static_cast<uint32_t>(nodes_.size());
            slot = {k, i};
            nodes_.push_back({i, 1, k});
        }
        return static_cast<uint32_t>(slot.value);
    }
    uint32_t root(uint32_t i) {
        while (i != nodes_[i].parent) {
            nodes_[i].parent = nodes_[nodes_[i].parent].parent;
            i = nodes_[i].parent;
        }
        return i;
    }
    bool parallel_links(const Input& links, Keys& keys, unsigned threads) {
        struct Vertex {
            uint32_t slot, root;
        };
        std::vector<std::vector<Vertex>> vertices(threads);
        std::atomic<bool> retry{false};
        auto index = [&](Key k, std::vector<Vertex>& inserted) {
            size_t at = hash_user(k) & mask_;
            for (unsigned probes = 0; probes < 48; ++probes) {
                std::atomic_ref<Key> ref(table_[at].id);
                Key found = ref.load(std::memory_order_relaxed);
                if (!found && ref.compare_exchange_strong(found, k, std::memory_order_relaxed)) {
                    inserted.push_back({uint32_t(at), 0});
                    return uint32_t(at);
                }
                if (found == k)
                    return uint32_t(at);
                at = (at + 1) & mask_;
            }
            retry.store(true, std::memory_order_relaxed);
            return UINT32_MAX;
        };
        auto find_root = [&](uint32_t i) {
            for (;;) {
                std::atomic_ref<Key> parent(table_[i].value);
                Key next = parent.load(std::memory_order_acquire);
                if (!next)
                    return i;
                uint32_t j = next - 1;
                Key grand = std::atomic_ref<Key>(table_[j].value).load(std::memory_order_acquire);
                if (grand)
                    parent.compare_exchange_weak(next, grand, std::memory_order_acq_rel);
                i = j;
            }
        };
        jobs(links.chunks.size(), threads, [&](unsigned lane, size_t chunk) {
            if (retry.load(std::memory_order_relaxed))
                return;
            std::array<std::pair<Key, Key>, 96> edges;
            size_t n = 0;
            auto merge = [&] {
                for (size_t i = 0; i < n; ++i) {
                    auto [ka, kb] = edges[i];
                    uint32_t a = index(ka, vertices[lane]), b = index(kb, vertices[lane]);
                    if (a == UINT32_MAX || b == UINT32_MAX)
                        continue;
                    for (;;) {
                        a = find_root(a);
                        b = find_root(b);
                        if (a == b)
                            break;
                        Key aid = std::atomic_ref<Key>(table_[a].id).load(std::memory_order_relaxed);
                        Key bid = std::atomic_ref<Key>(table_[b].id).load(std::memory_order_relaxed);
                        // Every parent is strictly smaller in byte order. A
                        // concurrent union cannot introduce a cycle.
                        if (keys.less(aid, bid))
                            std::swap(a, b);
                        Key empty = 0;
                        if (std::atomic_ref<Key>(table_[a].value)
                                .compare_exchange_strong(empty, Key(b) + 1, std::memory_order_acq_rel))
                            break;
                    }
                }
                n = 0;
            };
            auto add = [&](Key a, Key b) {
                prefetch(a);
                prefetch(b);
                edges[n++] = {a, b};
                if (n == edges.size())
                    merge();
            };
            auto slow = [&](const auto& f) {
                if (!f[0].empty() && !f[1].empty())
                    add(keys.encode(f[0]), keys.encode(f[1]));
            };
#if defined(__AVX2__) && defined(__BMI2__)
            plain_rows<2>(
                links.chunks[chunk],
                [&](const char* p, const char* end) {
                    auto v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
                    uint32_t mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(',')));
                    unsigned sep = std::countr_zero(mask);
                    if (sep >= 32 || p + sep >= end)
                        return false;
                    unsigned a = *p == '"', b = p[sep + 1] == '"';
                    size_t na = sep - 2 * a, nb = end - p - sep - 1 - 2 * b;
                    if (na && nb)
                        add(keys.clean(p + a, na), keys.clean(p + sep + 1 + b, nb));
                    return true;
                },
                slow);
#else
            rows<2>(links.chunks[chunk],slow);
#endif
            merge();
        });
        if (retry.load()) {
            std::fill(table_.begin(), table_.end(), Slot{});
            return false;
        }
        jobs(vertices.size(), threads, [&](unsigned, size_t i) {
            for (auto& v : vertices[i])
                v.root = find_root(v.slot);
        });
        names_.reset(new Key[table_.size()]);
        jobs(vertices.size(), threads, [&](unsigned, size_t i) {
            for (auto v : vertices[i])
                if (v.root == v.slot)
                    table_[v.slot].value = uint64_t(record_name(v.slot, table_[v.slot].id)) + 1;
        });
        jobs(vertices.size(), threads, [&](unsigned, size_t i) {
            for (auto v : vertices[i])
                if (v.root != v.slot)
                    table_[v.slot].value = table_[v.root].value;
        });
        probe_limit_ = 48;
        return true;
    }

  public:
    People(const Input& links, Keys& keys, size_t log_bytes = 0, unsigned threads = 1) {
        // The size estimate only affects allocations. Oversized fields cannot
        // force an excessive initial table size.
        size_t guess = std::clamp<size_t>(std::max(links.size / 12, log_bytes / 850), 256, 1u << 24);
        resize(std::bit_ceil(guess));
        if (threads > 1 && links.size > (256u << 10) && parallel_links(links, keys, std::min(threads, 4u)))
            return;
        nodes_.reserve(std::min(guess, links.size / 12 + 16));
        using Edge = std::pair<Key, Key>;
        std::array<Edge, 128> edge;
        std::array<std::pair<uint32_t, uint32_t>, 128> refs;
        size_t used = 0;
        auto join_batch = [&] {
            for (size_t i = 0; i < used; ++i) {
                prefetch(edge[i].first);
                prefetch(edge[i].second);
            }
            for (size_t i = 0; i < used; ++i) {
                refs[i] = {index(edge[i].first), index(edge[i].second)};
                __builtin_prefetch(&nodes_[refs[i].first], 1, 1);
                __builtin_prefetch(&nodes_[refs[i].second], 1, 1);
            }
            for (size_t i = 0; i < used; ++i) {
                uint32_t a = root(refs[i].first), b = root(refs[i].second);
                if (a == b)
                    continue;
                if (nodes_[a].count < nodes_[b].count)
                    std::swap(a, b);
                nodes_[b].parent = a;
                nodes_[a].count += nodes_[b].count;
                if (keys.less(nodes_[b].name, nodes_[a].name))
                    nodes_[a].name = nodes_[b].name;
            }
            used = 0;
        };
        for (View chunk : links.chunks)
            rows<2>(chunk, [&](const auto& f) {
                if (!f[0].empty() && !f[1].empty()) {
                    edge[used++] = {keys.encode(f[0]), keys.encode(f[1])};
                    if (used == edge.size())
                        join_batch();
                }
            });
        join_batch();
        names_.reset(new Key[table_.size()]);
        for (uint32_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].parent == i)
                nodes_[i].count = record_name(locate(nodes_[i].name), nodes_[i].name);
        for (size_t i = 0; i < table_.size(); ++i) {
            auto& slot = table_[i];
            if (!slot.id)
                continue;
            slot.value = uint64_t(nodes_[root(static_cast<uint32_t>(slot.value))].count) + 1;
            probe_limit_ = std::max(probe_limit_, ((i - hash_user(slot.id)) & mask_) + 1);
        }
        std::vector<Component>().swap(nodes_);
    }
    void prefetch(Key id) const { __builtin_prefetch(&table_[hash_user(id) & mask_], 0, 1); }
    Key canonical(Key id) const {
        const auto& slot = table_[locate(id)];
        return slot.id ? name(slot.value - 1) : id;
    }
    // Linked components already have a representative slot. Install isolated
    // IDs concurrently into empty slots; full-byte keys resolve every collision.
    uint32_t number(Key k) {
        size_t hash = hash_user(k), at = hash & mask_;
        for (size_t probe = 0; probe < probe_limit_; ++probe) {
            auto& slot = table_[at];
            std::atomic_ref<Key> id(slot.id);
            Key seen = id.load(std::memory_order_relaxed);
            if (!seen && id.compare_exchange_strong(seen, k, std::memory_order_relaxed)) {
                uint32_t code = record_name(at, k);
                std::atomic_ref<Key>(slot.value).store(uint64_t(code) + 1, std::memory_order_release);
                return code;
            }
            if (seen == k) {
                auto value = std::atomic_ref<Key>(slot.value);
                Key code;
                while (!(code = value.load(std::memory_order_acquire)))
                    _mm_pause();
                return uint32_t(code - 1);
            }
            at = (at + 1) & mask_;
        }
        unsigned b = hash & 255;
        auto& ext = overflow_[b];
        std::lock_guard lock(ext.lock);
        auto [it, added] = ext.codes.try_emplace(k, 0);
        if (added) {
            uint64_t code = table_.size() + uint64_t(ext.names.size()) * 256 + b;
            if (code > UINT32_MAX)
                throw std::length_error("too many distinct people");
            it->second = code;
            ext.names.push_back(k);
        }
        return it->second;
    }
    size_t local_bound(unsigned shard) const {
        if (overflow_[shard].names.empty())
            return sizes_[shard].count.load(std::memory_order_relaxed);
        return table_.size() / 256 + overflow_[shard].names.size();
    }
    Key name(uint32_t code) const {
        if (code < table_.size())
            return names_[(code & 255) * (table_.size() / 256) + (code >> 8)];
        return overflow_[code & 255].names[(code - table_.size()) / 256];
    }
};

struct Campaign {
    View id, channel;
};
struct Dimensions {
    std::unordered_map<View, uint32_t> product, campaign;
    std::vector<View> categories;
    std::vector<Campaign> campaigns{{"organic", "organic"}};
    uint32_t unknown;
    std::vector<uint32_t> numeric_product, numeric_campaign;
    static uint32_t suffix(View s, View prefix) {
        if (!s.starts_with(prefix))
            return UINT32_MAX;
        s.remove_prefix(prefix.size());
        if (s.empty() || s.size() > 8 || (s.size() > 1 && s[0] == '0'))
            return UINT32_MAX;
        uint32_t n = 0;
        for (unsigned char ch : s) {
            unsigned d = unsigned(ch) - '0';
            if (d > 9)
                return UINT32_MAX;
            n = n * 10 + d;
        }
        return n;
    }
    Dimensions(const Input& products, const Input& ads) {
        std::unordered_map<View, uint32_t> category_numbers{{"unknown", 0}};
        categories.push_back("unknown");
        std::vector<std::pair<View, uint32_t>> products_and_categories;
        for (View chunk : products.chunks) {
            rows<2>(chunk, [&](const auto& f) {
                auto [it, added] = category_numbers.try_emplace(f[1], category_numbers.size());
                if (added)
                    categories.push_back(f[1]);
                products_and_categories.emplace_back(f[0], it->second);
            });
        }
        std::sort(categories.begin(), categories.end());
        std::vector<uint32_t> rank(categories.size());
        for (uint32_t i = 0; i < categories.size(); ++i)
            rank[category_numbers.at(categories[i])] = i;
        unknown = rank[0];
        numeric_product.assign(products_and_categories.size() * 2 + 1, UINT32_MAX);
        for (auto [id, category_number] : products_and_categories) {
            uint32_t n = suffix(id, "p");
            if (n < numeric_product.size())
                numeric_product[n] = rank[category_number];
            else
                product.emplace(id, rank[category_number]);
        }
        for (View chunk : ads.chunks)
            rows<2>(chunk, [&](const auto& f) { campaigns.push_back({f[0], f[1]}); });
        numeric_campaign.assign((campaigns.size() - 1) * 2 + 1, UINT32_MAX);
        for (uint32_t i = 1; i < campaigns.size(); ++i) {
            View id = campaigns[i].id;
            uint32_t n = suffix(id, "c_");
            if (n < numeric_campaign.size())
                numeric_campaign[n] = i;
            else
                campaign.emplace(id, i);
        }
    }
    uint32_t category(View id) const {
        auto n = suffix(id, "p");
        if (n < numeric_product.size())
            return numeric_product[n] == UINT32_MAX ? unknown : numeric_product[n];
        auto it = product.find(id);
        return it == product.end() ? unknown : it->second;
    }
    uint32_t campaign_index(View id) const {
        auto n = suffix(id, "c_");
        if (n < numeric_campaign.size())
            return numeric_campaign[n];
        auto it = campaign.find(id);
        return it == campaign.end() ? UINT32_MAX : it->second;
    }
};

struct Event {
    Key person;
    int64_t ts;
    uint32_t cents, category;
};
struct Candidate {
    Key id;
    int64_t ingest;
    Event event;
};
struct Click {
    Key person;
    int64_t ts;
    Key id;
    uint32_t campaign;
};
constexpr uint32_t no_category = std::numeric_limits<uint32_t>::max();
struct UserDay {
    Key person;
    int32_t day;
    uint32_t category;
    int64_t sessions, events, revenue;
};
struct Counts {
    int64_t sessions = 0, purchasers = 0, revenue = 0, assists = 0;
    Counts& operator+=(const Counts& b) {
        sessions += b.sessions;
        purchasers += b.purchasers;
        revenue += b.revenue;
        assists += b.assists;
        return *this;
    }
};
inline uint64_t campaign_day(uint32_t campaign, int32_t day) {
    return (uint64_t(uint32_t(day)) << 32) | campaign;
}
using CampaignDays = std::unordered_map<uint64_t, Counts>;
} // namespace shop
