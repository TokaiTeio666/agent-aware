#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agentmem/types.h"

namespace agentmem {

VectorSet load_fvecs(const std::string& path, std::size_t limit = 0);

std::vector<std::vector<std::uint32_t>> load_ivecs(const std::string& path,
                                                   std::size_t limit = 0);

SyntheticData generate_synthetic(const SyntheticConfig& config);

}  // namespace agentmem

