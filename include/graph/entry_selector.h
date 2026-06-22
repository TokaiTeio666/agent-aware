#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent_aware/core/types.h"

namespace agent_aware {

enum class EntryStrategy {
  SingleMedoid,
  EvenlySpaced,
  HybridMedoidHubsClusters,
};

struct EntrySelectionConfig {
  EntryStrategy strategy = EntryStrategy::EvenlySpaced;
  std::size_t entry_count = 64;
  std::size_t hub_count = 15;
  std::size_t cluster_count = 0;
  std::size_t cluster_train_limit = 100000;
  std::size_t cluster_iterations = 4;
  std::uint32_t seed = 42;
};

EntryStrategy parse_entry_strategy(const std::string& value);
std::string entry_strategy_name(EntryStrategy strategy);

std::uint32_t select_global_medoid(const VectorSet& base);

std::vector<std::uint32_t> select_evenly_spaced_entries(std::uint64_t count,
                                                        std::size_t requested);

std::vector<std::uint32_t> select_entry_points(
    const VectorSet& base, const EntrySelectionConfig& config,
    const std::vector<std::uint32_t>* node_in_degrees = nullptr);

}  // namespace agent_aware
