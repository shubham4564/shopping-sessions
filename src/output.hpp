#pragma once
#include "model.hpp"
namespace shop {
void write_output(const fs::path& out, const Keys& keys, const Dimensions& dim,
                  const std::vector<std::vector<UserDay>>& people, const CampaignDays& campaigns);
}
