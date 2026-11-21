#pragma once
#include "input.hpp"

namespace shop {
class People {
    struct Slot { Key id = 0, value = 0; };
    struct Component { uint32_t parent, count; Key name; };
    std::vector<Slot> table_;
    std::vector<Component> nodes_;
    size_t mask_ = 0;

    size_t locate(Key id) const {
        size_t at = mix(id) & mask_;
        while (table_[at].id && table_[at].id != id) at = (at + 1) & mask_;
        return at;
    }
    void resize(size_t n) {
        auto old = std::move(table_);
        table_.assign(n, {});
        mask_ = n - 1;
        for (auto slot : old) if (slot.id) table_[locate(slot.id)] = slot;
    }
    uint32_t index(Key k) {
        if (nodes_.size() * 4 >= table_.size() * 3) resize(table_.size() * 2);
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
public:
    People(const Input& links, Keys& keys) {
        // The size estimate only affects allocations. Oversized fields cannot
        // force an excessive initial table size.
        size_t guess = std::clamp<size_t>(links.size / 12, 16, 1u << 22);
        resize(std::bit_ceil(guess));
        for (View chunk : links.chunks) rows<2>(chunk, [&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            uint32_t a = root(index(keys.encode(f[0])));
            uint32_t b = root(index(keys.encode(f[1])));
            if (a == b) return;
            if (nodes_[a].count < nodes_[b].count) std::swap(a, b);
            nodes_[b].parent = a;
            nodes_[a].count += nodes_[b].count;
            if (keys.less(nodes_[b].name, nodes_[a].name)) nodes_[a].name = nodes_[b].name;
        });
        for (auto& slot : table_)
            if (slot.id) slot.value = nodes_[root(static_cast<uint32_t>(slot.value))].name;
        std::vector<Component>().swap(nodes_);
    }
    Key canonical(Key id) const {
        const auto& slot = table_[locate(id)];
        return slot.id ? slot.value : id;
    }
};

struct Campaign { View id, channel; };
struct Dimensions {
    std::unordered_map<View, uint32_t> product, campaign;
    std::vector<View> categories;
    std::vector<Campaign> campaigns{{"organic","organic"}};
    uint32_t unknown;
    Dimensions(const Input& products, const Input& ads) {
        categories.push_back("unknown");
        std::unordered_map<View, uint32_t> ranks;
        for (View chunk : products.chunks) rows<2>(chunk, [&](const auto& f) {
            if (ranks.try_emplace(f[1], 0).second) categories.push_back(f[1]);
        });
        std::sort(categories.begin(), categories.end());
        categories.erase(std::unique(categories.begin(), categories.end()), categories.end());
        for (uint32_t i = 0; i < categories.size(); ++i) ranks[categories[i]] = i;
        unknown = ranks.at("unknown");
        for (View chunk : products.chunks) rows<2>(chunk, [&](const auto& f) {
            product.emplace(f[0], ranks.at(f[1]));
        });
        for (View chunk : ads.chunks) rows<2>(chunk, [&](const auto& f) {
            campaign.emplace(f[0], campaigns.size());
            campaigns.push_back({f[0], f[1]});
        });
    }
    uint32_t category(View id) const {
        auto it = product.find(id);
        return it == product.end() ? unknown : it->second;
    }
};

struct Event { Key person; int64_t ts; uint32_t cents, category; };
struct Candidate { Key id; int64_t ingest; Event event; };
struct Click { Key person; int64_t ts; Key id; uint32_t campaign; };
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
}
