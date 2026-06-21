#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "agent_aware/graph/entry_selector.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool contains(const std::vector<std::uint32_t>& values, std::uint32_t value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

agent_aware::VectorSet line_vectors(std::size_t count) {
  agent_aware::VectorSet vectors(count, 1);
  for (std::size_t i = 0; i < count; ++i) {
    vectors.values[i] = static_cast<float>(i);
  }
  return vectors;
}

void test_single_medoid() {
  const auto vectors = line_vectors(5);
  require(agent_aware::select_global_medoid(vectors) == 2,
          "global medoid is nearest to centroid");

  agent_aware::EntrySelectionConfig config;
  config.strategy = agent_aware::EntryStrategy::SingleMedoid;
  config.entry_count = 64;
  const auto entries = agent_aware::select_entry_points(vectors, config);
  require(entries.size() == 1, "single-medoid returns one entry");
  require(entries[0] == 2, "single-medoid selected center node");
}

void test_evenly_spaced() {
  const auto entries = agent_aware::select_evenly_spaced_entries(5, 3);
  require((entries == std::vector<std::uint32_t>{0, 2, 4}),
          "evenly-spaced keeps current deterministic layout");

  agent_aware::EntrySelectionConfig config;
  config.strategy = agent_aware::EntryStrategy::EvenlySpaced;
  config.entry_count = 3;
  const auto selected = agent_aware::select_entry_points(line_vectors(5), config);
  require(selected == entries, "entry selector delegates evenly-spaced");
}

void test_hybrid_medoid_hubs_clusters() {
  const auto vectors = line_vectors(10);
  std::vector<std::uint32_t> in_degrees(10, 1);
  in_degrees[7] = 99;
  in_degrees[3] = 50;

  agent_aware::EntrySelectionConfig config;
  config.strategy = agent_aware::EntryStrategy::HybridMedoidHubsClusters;
  config.entry_count = 5;
  config.hub_count = 2;
  config.cluster_count = 2;
  config.cluster_train_limit = 10;
  config.cluster_iterations = 2;

  const auto entries =
      agent_aware::select_entry_points(vectors, config, &in_degrees);
  require(entries.size() == 5, "hybrid fills requested entries");
  require(contains(entries, 4), "hybrid contains global medoid");
  require(contains(entries, 7), "hybrid contains top hub");
  require(contains(entries, 3), "hybrid contains second hub");

  std::unordered_set<std::uint32_t> unique(entries.begin(), entries.end());
  require(unique.size() == entries.size(), "hybrid entries are unique");
}

void test_strategy_parsing() {
  require(agent_aware::parse_entry_strategy("single-medoid") ==
              agent_aware::EntryStrategy::SingleMedoid,
          "parse single-medoid");
  require(agent_aware::parse_entry_strategy("evenly_spaced") ==
              agent_aware::EntryStrategy::EvenlySpaced,
          "parse evenly_spaced alias");
  require(agent_aware::parse_entry_strategy("hybrid") ==
              agent_aware::EntryStrategy::HybridMedoidHubsClusters,
          "parse hybrid");
  require(agent_aware::entry_strategy_name(
              agent_aware::EntryStrategy::HybridMedoidHubsClusters) == "hybrid",
          "strategy name");
}

}  // namespace

int main() {
  test_single_medoid();
  test_evenly_spaced();
  test_hybrid_medoid_hubs_clusters();
  test_strategy_parsing();
  return 0;
}
