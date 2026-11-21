#pragma once
#include "plain.hpp"

namespace shop {
class People {
    struct Slot {
        Key id, value;
    };
    struct UnmapDirect {
        size_t bytes;
        void operator()(uint32_t* p) const { if (p) munmap(p, bytes); }
    };
    using DirectStorage = std::unique_ptr<uint32_t[], UnmapDirect>;
    DirectStorage direct_storage_{nullptr, UnmapDirect{0}};
    uint32_t* direct_ = nullptr;
    std::vector<uint64_t> remapped_;
    uint32_t direct_cap_ = 0;
    uint8_t direct_mode_ = 0; // 0=none, 1=fixed 'u'+7d, 2=fixed 'u'+Wd, 3=var 'u'+d
    uint8_t direct_width_ = 0;

    static constexpr uint32_t parse_fixed7_key(Key k) {
        if ((k & 0x7fu) != 0 || (k >> 56) != 'u')
            return UINT32_MAX;
#if defined(__BMI2__)
        uint64_t w = __builtin_bswap64(_pdep_u64(k >> 7, 0x7f7f7f7f7f7f7f7fULL));
        uint64_t d = (w & ~0xFFULL) | uint64_t('0');
        if (((d & 0xF0F0F0F0F0F0F0F0ULL) != 0x3030303030303030ULL) |
            (((d + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) != 0x3030303030303030ULL))
            return UINT32_MAX;
        uint64_t val = d & 0x0F0F0F0F0F0F0F0FULL;
        val = (val * 10 + (val >> 8)) & 0x00FF00FF00FF00FFULL;
        val = (val * 100 + (val >> 16)) & 0x0000FFFF0000FFFFULL;
        val = (val * 10000 + (val >> 32)) & 0x00000000FFFFFFFFULL;
        return uint32_t(val);
#else
        uint32_t val = 0;
        for (int shift = 49; shift >= 7; shift -= 7) {
            unsigned d = unsigned((k >> shift) & 127u) - '0';
            if (d > 9) return UINT32_MAX;
            val = val * 10 + d;
        }
        return val;
#endif
    }
    static constexpr uint32_t parse_fixedw_key(Key k, unsigned w) {
        if ((k >> 56) != 'u')
            return UINT32_MAX;
        if (w < 8 && (k & ((Key{1} << (7 * (8 - w))) - 1)) != 0)
            return UINT32_MAX;
        uint32_t val = 0;
        for (unsigned i = 0; i < w; ++i) {
            unsigned d = unsigned((k >> (49 - 7 * i)) & 127u) - '0';
            if (d > 9) return UINT32_MAX;
            val = val * 10 + d;
        }
        return val;
    }
    static constexpr uint32_t parse_var_key(Key k) {
        if ((k >> 56) != 'u')
            return UINT32_MAX;
        unsigned first = unsigned((k >> 49) & 127u) - '0';
        if (first > 9) return UINT32_MAX;
        if (first == 0) {
            return (k & ((Key{1} << 49) - 1)) == 0 ? 0u : UINT32_MAX;
        }
        uint32_t val = first;
        for (int shift = 42; shift >= 0; shift -= 7) {
            unsigned ch = unsigned((k >> shift) & 127u);
            if (!ch) return val;
            unsigned d = ch - '0';
            if (d > 9) return UINT32_MAX;
            val = val * 10 + d;
        }
        return val;
    }
    [[gnu::always_inline]] inline uint32_t parse_direct(Key k) const {
        if (direct_mode_ == 1) [[likely]]
            return parse_fixed7_key(k);
        if (direct_mode_ == 2)
            return parse_fixedw_key(k, direct_width_);
        return parse_var_key(k);
    }
    Key format_direct(uint32_t n) const {
        char buf[9];
        buf[0] = 'u';
        size_t len = 1;
        if (direct_mode_ == 1 || direct_mode_ == 2) {
            unsigned w = direct_mode_ == 1 ? 7u : direct_width_;
            for (unsigned i = w; i-- > 0;) {
                buf[1 + i] = char('0' + (n % 10));
                n /= 10;
            }
            len = 1 + w;
        } else {
            char tmp[10];
            unsigned t = 0;
            do {
                tmp[t++] = char('0' + (n % 10));
                n /= 10;
            } while (n);
            while (t--)
                buf[len++] = tmp[t];
        }
        Key k = 0;
        for (size_t i = 0; i < len; ++i)
            k = (k << 7) | static_cast<unsigned char>(buf[i]);
        return k << (7 * (9 - len));
    }
    void init_direct_storage(uint32_t cap) {
        if (!cap) return;
        cap = (cap + 255u) & ~255u;
        size_t bytes = size_t(cap) * sizeof(uint32_t);
        void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
            return;
        madvise(mem, bytes, MADV_HUGEPAGE);
        direct_storage_ = DirectStorage(::new (mem) uint32_t[cap], UnmapDirect{bytes});
        direct_ = direct_storage_.get();
        remapped_.assign((cap + 63u) >> 6, 0);
        direct_cap_ = cap;
    }
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
        // Auto-detect numeric 'u' user ID pattern from linked vertices.
        size_t sampled = 0, fixed_hits[9]{}, var_hits = 0;
        for (size_t i = 0; i < vertices.size() && sampled < 1024; ++i) {
            for (auto v : vertices[i]) {
                if (sampled >= 1024) break;
                Key k = table_[v.slot].id;
                ++sampled;
                if ((k >> 56) != 'u' || (k & (Key{1} << 63))) continue;
                unsigned len = k ? 9 - std::countr_zero(k) / 7 : 0;
                if (len >= 6 && len <= 9 && parse_fixedw_key(k, len - 1) != UINT32_MAX)
                    ++fixed_hits[len - 1];
                if (parse_var_key(k) != UINT32_MAX)
                    ++var_hits;
            }
        }
        unsigned best_w = 0;
        size_t best_fixed = 0;
        for (unsigned w = 5; w <= 8; ++w) {
            if (fixed_hits[w] > best_fixed) {
                best_fixed = fixed_hits[w];
                best_w = w;
            }
        }
        if (sampled >= 32) {
            if (best_fixed * 2 >= sampled) {
                direct_mode_ = (best_w == 7) ? 1 : 2;
                direct_width_ = uint8_t(best_w);
            } else if (var_hits * 2 >= sampled) {
                direct_mode_ = 3;
            }
        }
        if (direct_mode_) {
            uint32_t max_id = 0;
            for (size_t i = 0; i < vertices.size(); ++i) {
                for (auto v : vertices[i]) {
                    uint32_t id = parse_direct(table_[v.slot].id);
                    if (id < (16u << 20) && id > max_id)
                        max_id = id;
                }
            }
            uint32_t cap = std::clamp<uint32_t>(max_id + max_id / 4 + 65536u, 65536u, 16u << 20);
            init_direct_storage(cap);
        }
        names_.reset(new Key[table_.size()]);
        jobs(vertices.size(), threads, [&](unsigned, size_t i) {
            for (auto v : vertices[i]) {
                if (v.root == v.slot) {
                    Key k = table_[v.slot].id;
                    uint32_t code;
                    uint32_t did = direct_cap_ ? parse_direct(k) : UINT32_MAX;
                    if (did < direct_cap_) {
                        code = did;
                        direct_[did] = code + 1;
                    } else {
                        code = direct_cap_ + record_name(v.slot, k);
                    }
                    table_[v.slot].value = uint64_t(code) + 1;
                }
            }
        });
        jobs(vertices.size(), threads, [&](unsigned, size_t i) {
            for (auto v : vertices[i]) {
                if (v.root != v.slot) {
                    uint64_t val = table_[v.root].value;
                    table_[v.slot].value = val;
                    if (direct_cap_) {
                        uint32_t did = parse_direct(table_[v.slot].id);
                        if (did < direct_cap_) {
                            direct_[did] = uint32_t(val);
                            if (uint32_t(val) - 1u != did) {
                                std::atomic_ref<uint64_t>(remapped_[did >> 6])
                                    .fetch_or(uint64_t{1} << (did & 63), std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
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
    void prefetch(Key id) const {
        if (direct_cap_) {
            uint32_t did = parse_direct(id);
            if (did < direct_cap_) [[likely]] {
                __builtin_prefetch(&remapped_[did >> 6], 0, 1);
                return;
            }
        }
        __builtin_prefetch(&table_[hash_user(id) & mask_], 0, 1);
    }
    Key canonical(Key id) const {
        if (direct_cap_) {
            uint32_t did = parse_direct(id);
            if (did < direct_cap_) {
                uint32_t v = direct_[did];
                return v ? name(v - 1) : id;
            }
        }
        const auto& slot = table_[locate(id)];
        return slot.id ? name(slot.value - 1) : id;
    }
    // Linked components already have a representative slot. Install isolated
    // IDs concurrently into empty slots; full-byte keys resolve every collision.
    static inline uint32_t parse_fixed7_raw(const char* p) {
        if (p[0] != 'u')
            return UINT32_MAX;
        uint64_t w;
        std::memcpy(&w, p, 8);
        uint64_t d = (w & ~0xFFULL) | uint64_t('0');
        if (((d & 0xF0F0F0F0F0F0F0F0ULL) != 0x3030303030303030ULL) |
            (((d + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) != 0x3030303030303030ULL))
            return UINT32_MAX;
        uint64_t val = d & 0x0F0F0F0F0F0F0F0FULL;
        val = (val * 10 + (val >> 8)) & 0x00FF00FF00FF00FFULL;
        val = (val * 100 + (val >> 16)) & 0x0000FFFF0000FFFFULL;
        val = (val * 10000 + (val >> 32)) & 0x00000000FFFFFFFFULL;
        return uint32_t(val);
    }
    [[gnu::always_inline]] inline uint32_t try_parse_direct_raw(const char* p, size_t n) const {
        if (direct_mode_ == 1 && n == 8) [[likely]] {
            uint32_t did = parse_fixed7_raw(p);
            if (did < direct_cap_) [[likely]] return did;
        }
        return UINT32_MAX;
    }
    [[gnu::always_inline]] inline void prefetch_did(uint32_t did) const {
        if (did != UINT32_MAX) [[likely]]
            __builtin_prefetch(&remapped_[did >> 6], 0, 1);
    }
    [[gnu::always_inline]] inline uint32_t resolve_did_or_clean(uint32_t did, const char* p, size_t n, Keys& keys) {
        if (did != UINT32_MAX) [[likely]] {
            if (!((remapped_[did >> 6] >> (did & 63)) & 1ULL)) [[likely]]
                return did;
            return direct_[did] - 1;
        }
        return number(keys.clean(p, n));
    }
    [[gnu::always_inline]] inline void prefetch_raw_clean(const char* p, size_t n) const {
        if (direct_mode_ == 1 && n == 8) [[likely]] {
            uint32_t did = parse_fixed7_raw(p);
            if (did < direct_cap_) [[likely]] {
                __builtin_prefetch(&remapped_[did >> 6], 0, 1);
            }
        }
    }
    [[gnu::always_inline]] inline uint32_t number_raw_clean(const char* p, size_t n, Keys& keys) {
        if (direct_mode_ == 1 && n == 8) [[likely]] {
            uint32_t did = parse_fixed7_raw(p);
            if (did < direct_cap_) [[likely]] {
                if (!((remapped_[did >> 6] >> (did & 63)) & 1ULL)) [[likely]]
                    return did;
                return direct_[did] - 1;
            }
        }
        return number(keys.clean(p, n));
    }
    [[gnu::always_inline]] inline uint32_t number_raw(View s, Keys& keys) {
        if (direct_mode_ == 1 && s.size() == 8) [[likely]] {
            uint32_t did = parse_fixed7_raw(s.data());
            if (did < direct_cap_) [[likely]] {
                if (!((remapped_[did >> 6] >> (did & 63)) & 1ULL)) [[likely]]
                    return did;
                return direct_[did] - 1;
            }
        }
        return number(keys.encode(s));
    }
    [[gnu::always_inline]] inline uint32_t number(Key k) {
        if (direct_cap_) {
            uint32_t did = parse_direct(k);
            if (did < direct_cap_) [[likely]] {
                if (!((remapped_[did >> 6] >> (did & 63)) & 1ULL)) [[likely]]
                    return did;
                return direct_[did] - 1;
            }
        }
        return number_slow(k);
    }
    [[gnu::noinline]] uint32_t number_slow(Key k) {
        size_t hash = hash_user(k), at = hash & mask_;
        for (size_t probe = 0; probe < probe_limit_; ++probe) {
            auto& slot = table_[at];
            std::atomic_ref<Key> id(slot.id);
            Key seen = id.load(std::memory_order_relaxed);
            if (!seen && id.compare_exchange_strong(seen, k, std::memory_order_relaxed)) {
                uint32_t code = direct_cap_ + record_name(at, k);
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
            uint64_t code = uint64_t(direct_cap_) + table_.size() + uint64_t(ext.names.size()) * 256 + b;
            if (code > UINT32_MAX)
                throw std::length_error("too many distinct people");
            it->second = uint32_t(code);
            ext.names.push_back(k);
        }
        return it->second;
    }
    size_t local_bound(unsigned shard) const {
        size_t base = direct_cap_ >> 8;
        if (overflow_[shard].names.empty())
            return base + sizes_[shard].count.load(std::memory_order_relaxed);
        return base + table_.size() / 256 + overflow_[shard].names.size();
    }
    Key name(uint32_t code) const {
        if (code < direct_cap_)
            return format_direct(code);
        uint32_t h = code - direct_cap_;
        if (h < table_.size())
            return names_[(h & 255) * (table_.size() / 256) + (h >> 8)];
        return overflow_[h & 255].names[(h - table_.size()) / 256];
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
    int32_t sessions, events;
    int64_t revenue;
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
