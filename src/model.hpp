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
    std::unique_ptr<uint64_t[]> bloom_;
    size_t mask_ = 0, bloom_mask_ = 0;
    bool has_links_ = false;

    static inline size_t hash_user(Key k) {
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
    void build_readonly(size_t non_roots, auto&& emit_aliases) {
        if (!non_roots) {
            storage_.reset();
            table_ = {};
            mask_ = 0;
            has_links_ = false;
            return;
        }
        size_t n = std::bit_ceil(std::max<size_t>(256, non_roots * 2 + 1));
        size_t bytes = n * sizeof(Slot);
        void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
            throw std::bad_alloc();
        madvise(mem, bytes, MADV_HUGEPAGE);
        SlotStorage fresh(::new (mem) Slot[n], UnmapSlots{bytes});
        Slot* dst = fresh.get();
        size_t dst_mask = n - 1;

        // 16 bits per non-root alias (~6% false positive rate), capped at 2 MiB (2^24 bits)
        size_t bloom_bits = std::clamp<size_t>(std::bit_ceil(non_roots * 16), 4096, size_t(1) << 24);
        size_t bloom_words = bloom_bits / 64;
        bloom_.reset(new uint64_t[bloom_words]());
        bloom_mask_ = bloom_words - 1;

        emit_aliases([&](Key id, Key canon) {
            size_t h = hash_user(id);
            bloom_[(h >> 6) & bloom_mask_] |= uint64_t{1} << (h & 63);
            size_t at = h & dst_mask;
            while (dst[at].id)
                at = (at + 1) & dst_mask;
            dst[at] = {id, canon};
        });

        storage_ = std::move(fresh);
        table_ = {storage_.get(), n};
        mask_ = dst_mask;
        has_links_ = true;
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
                __builtin_prefetch(&table_[hash_user(a) & mask_], 1, 1);
                __builtin_prefetch(&table_[hash_user(b) & mask_], 1, 1);
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
            rows<2>(links.chunks[chunk], slow);
#endif
            merge();
        });
        if (retry.load()) {
            std::fill(table_.begin(), table_.end(), Slot{});
            return false;
        }
        std::vector<size_t> counts(vertices.size(), 0);
        jobs(vertices.size(), threads, [&](unsigned, size_t i) {
            size_t nr = 0;
            for (auto& v : vertices[i]) {
                v.root = find_root(v.slot);
                if (v.root != v.slot)
                    ++nr;
            }
            counts[i] = nr;
        });
        size_t non_roots = 0;
        for (size_t c : counts)
            non_roots += c;
        build_readonly(non_roots, [&](auto&& insert) {
            for (const auto& vec : vertices)
                for (auto v : vec)
                    if (v.root != v.slot)
                        insert(table_[v.slot].id, table_[v.root].id);
        });
        return true;
    }

  public:
    People(const Input& links, Keys& keys, size_t = 0, unsigned threads = 1) {
        if (!links.size)
            return;
        size_t guess = std::clamp<size_t>(links.size / 6, 256, 1u << 24);
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
                __builtin_prefetch(&table_[hash_user(edge[i].first) & mask_], 0, 1);
                __builtin_prefetch(&table_[hash_user(edge[i].second) & mask_], 0, 1);
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
        size_t non_roots = 0;
        for (size_t i = 0; i < table_.size(); ++i) {
            auto& slot = table_[i];
            if (!slot.id)
                continue;
            Key canon = nodes_[root(static_cast<uint32_t>(slot.value))].name;
            slot.value = canon;
            if (canon != slot.id)
                ++non_roots;
        }
        std::vector<Component>().swap(nodes_);
        build_readonly(non_roots, [&](auto&& insert) {
            for (const auto& slot : table_)
                if (slot.id && slot.value != slot.id)
                    insert(slot.id, slot.value);
        });
    }
    [[gnu::always_inline]] inline void prefetch(Key id) const {
        if (has_links_) {
            size_t h = hash_user(id);
            __builtin_prefetch(&bloom_[(h >> 6) & bloom_mask_], 0, 1);
        }
    }
    [[gnu::always_inline]] inline Key canonical(Key id) const {
        if (!has_links_)
            return id;
        size_t h = hash_user(id);
        if (!((bloom_[(h >> 6) & bloom_mask_] >> (h & 63)) & 1))
            return id;
        size_t at = h & mask_;
        while (table_[at].id) {
            if (table_[at].id == id)
                return table_[at].value;
            at = (at + 1) & mask_;
        }
        return id;
    }
    // Two-pass MLP batch lookup: pass 1 checks L2 bloom and prefetches L3/DRAM table_
    // for all potentially aliased keys; pass 2 resolves only those keys in parallel.
    template <size_t MaxN>
    inline void batch_canonical(const Key* in, Key* out, size_t n) const {
        if (!has_links_) {
            std::memcpy(out, in, n * sizeof(Key));
            return;
        }
        size_t hashes[MaxN];
        uint8_t aliased[MaxN];
        size_t na = 0;
        for (size_t i = 0; i < n; ++i) {
            Key k = in[i];
            out[i] = k;
            if (!k)
                continue;
            size_t h = hash_user(k);
            if ((bloom_[(h >> 6) & bloom_mask_] >> (h & 63)) & 1) {
                __builtin_prefetch(&table_[h & mask_], 0, 1);
                hashes[na] = h;
                aliased[na] = uint8_t(i);
                ++na;
            }
        }
        for (size_t j = 0; j < na; ++j) {
            size_t i = aliased[j];
            Key id = in[i];
            size_t at = hashes[j] & mask_;
            while (table_[at].id) {
                if (table_[at].id == id) {
                    out[i] = table_[at].value;
                    break;
                }
                at = (at + 1) & mask_;
            }
        }
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
