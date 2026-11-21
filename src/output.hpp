#pragma once
#include "model.hpp"
namespace shop {
class Output {
    struct State;
    std::unique_ptr<State> state_;

  public:
    static constexpr unsigned parts = 8;
    Output(const fs::path& out, const Keys& keys, const Dimensions& dimensions);
    ~Output();
    void append(unsigned part, std::span<const UserDay> rows);
    void finish(const CampaignDays& campaigns, unsigned workers);
};
void write_output(const fs::path& out, const Keys& keys, const Dimensions& dimensions,
                  const std::vector<std::vector<UserDay>>& people, const CampaignDays& campaigns,
                  unsigned workers);
} // namespace shop
