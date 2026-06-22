#include "agent_aware/graph/entry_selector.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace agent_aware {
namespace {

float squared_l2_to_vector(const float* lhs, const std::vector<float>& rhs,
                           std::size_t dim) {
  float distance = 0.0f;
  for (std::size_t d = 0; d < dim; ++d) {
    const float diff = lhs[d] - rhs[d];
    distance += diff * diff;
  }
  return distance;
}

bool append_unique(std::vector<std::uint32_t>& out,
                   std::unordered_set<std::uint32_t>& seen,
                   std::uint32_t id, std::size_t limit) {
  if (out.size() >= limit) {
    return false;
  }
  if (!seen.insert(id).second) {
    return false;
  }
  out.push_back(id);
  return true;
}

std::vector<std::uint32_t> select_hub_entries(
    const std::vector<std::uint32_t>& node_in_degrees, std::uint64_t count,
    std::size_t requested) {
  std::vector<std::uint32_t> ids;
  if (requested == 0 || count == 0 || node_in_degrees.empty()) {
    return ids;
  }

  const std::size_t available =
      std::min<std::size_t>(node_in_degrees.size(),
                            static_cast<std::size_t>(count));
  ids.resize(available);
  std::iota(ids.begin(), ids.end(), std::uint32_t{0});
  std::sort(ids.begin(), ids.end(),
            [&](std::uint32_t lhs, std::uint32_t rhs) {
              if (node_in_degrees[lhs] == node_in_degrees[rhs]) {
                return lhs < rhs;
              }
              return node_in_degrees[lhs] > node_in_degrees[rhs];
            });
  if (ids.size() > requested) {
    ids.resize(requested);
  }
  return ids;
}

std::size_t nearest_centroid(const float* vector,
                             const std::vector<float>& centroids,
                             std::size_t centroid_count, std::size_t dim) {
  std::size_t best = 0;
  float best_distance = std::numeric_limits<float>::infinity();
  for (std::size_t centroid = 0; centroid < centroid_count; ++centroid) {
    float distance = 0.0f;
    const float* center = centroids.data() + centroid * dim;
    for (std::size_t d = 0; d < dim; ++d) {
      const float diff = vector[d] - center[d];
      distance += diff * diff;
    }
    if (distance < best_distance) {
      best = centroid;
      best_distance = distance;
    }
  }
  return best;
}

std::vector<std::uint32_t> select_cluster_medoids(
    const VectorSet& base, std::size_t requested, std::size_t train_limit,
    std::size_t iterations) {
  std::vector<std::uint32_t> medoids;
  if (base.empty() || requested == 0) {
    return medoids;
  }

  const std::size_t train_count =
      std::min<std::size_t>(base.size(),
                            std::max<std::size_t>(requested, train_limit));
  const auto sample_ids = select_evenly_spaced_entries(base.size(), train_count);
  const std::size_t centroid_count =
      std::min<std::size_t>(requested, sample_ids.size());
  if (centroid_count == 0) {
    return medoids;
  }

  std::vector<float> centroids(centroid_count * base.dim, 0.0f);
  for (std::size_t centroid = 0; centroid < centroid_count; ++centroid) {
    std::size_t sample_index = 0;
    if (centroid_count == 1) {
      sample_index = sample_ids.size() / 2;
    } else {
      sample_index = (centroid * (sample_ids.size() - 1)) /
                     (centroid_count - 1);
    }
    const float* row = base.row(sample_ids[sample_index]);
    std::copy(row, row + base.dim, centroids.begin() + centroid * base.dim);
  }

  std::vector<std::size_t> assignments(sample_ids.size(), 0);
  for (std::size_t iter = 0; iter < iterations; ++iter) {
    std::vector<double> sums(centroid_count * base.dim, 0.0);
    std::vector<std::size_t> counts(centroid_count, 0);
    for (std::size_t i = 0; i < sample_ids.size(); ++i) {
      const float* row = base.row(sample_ids[i]);
      const std::size_t centroid =
          nearest_centroid(row, centroids, centroid_count, base.dim);
      assignments[i] = centroid;
      ++counts[centroid];
      for (std::size_t d = 0; d < base.dim; ++d) {
        sums[centroid * base.dim + d] += row[d];
      }
    }
    for (std::size_t centroid = 0; centroid < centroid_count; ++centroid) {
      if (counts[centroid] == 0) {
        continue;
      }
      for (std::size_t d = 0; d < base.dim; ++d) {
        centroids[centroid * base.dim + d] =
            static_cast<float>(sums[centroid * base.dim + d] /
                               static_cast<double>(counts[centroid]));
      }
    }
  }

  std::unordered_set<std::uint32_t> used;
  medoids.reserve(centroid_count);
  for (std::size_t centroid = 0; centroid < centroid_count; ++centroid) {
    std::uint32_t best_id = 0;
    float best_distance = std::numeric_limits<float>::infinity();
    bool found = false;
    const std::vector<float> center(centroids.begin() + centroid * base.dim,
                                    centroids.begin() +
                                        (centroid + 1) * base.dim);
    for (std::size_t i = 0; i < sample_ids.size(); ++i) {
      if (assignments[i] != centroid) {
        continue;
      }
      const std::uint32_t id = sample_ids[i];
      if (used.find(id) != used.end()) {
        continue;
      }
      const float distance = squared_l2_to_vector(base.row(id), center,
                                                  base.dim);
      if (!found || distance < best_distance ||
          (distance == best_distance && id < best_id)) {
        found = true;
        best_distance = distance;
        best_id = id;
      }
    }
    if (found) {
      used.insert(best_id);
      medoids.push_back(best_id);
    }
  }
  return medoids;
}

}  // namespace

EntryStrategy parse_entry_strategy(const std::string& value) {
  if (value == "single-medoid" || value == "single_medoid" ||
      value == "medoid") {
    return EntryStrategy::SingleMedoid;
  }
  if (value == "evenly-spaced" || value == "evenly_spaced" ||
      value == "evenly") {
    return EntryStrategy::EvenlySpaced;
  }
  if (value == "hybrid" || value == "medoid-hubs-clusters" ||
      value == "medoid_hubs_clusters") {
    return EntryStrategy::HybridMedoidHubsClusters;
  }
  throw std::runtime_error(
      "Invalid entry strategy: expected single-medoid, evenly-spaced, or hybrid");
}

std::string entry_strategy_name(EntryStrategy strategy) {
  switch (strategy) {
    case EntryStrategy::SingleMedoid:
      return "single-medoid";
    case EntryStrategy::EvenlySpaced:
      return "evenly-spaced";
    case EntryStrategy::HybridMedoidHubsClusters:
      return "hybrid";
  }
  return "unknown";
}

std::uint32_t select_global_medoid(const VectorSet& base) {
  if (base.empty()) {
    throw std::runtime_error("Cannot select a medoid from an empty base set");
  }

  std::vector<float> centroid(base.dim, 0.0f);
  for (std::size_t row = 0; row < base.size(); ++row) {
    const float* values = base.row(row);
    for (std::size_t d = 0; d < base.dim; ++d) {
      centroid[d] += values[d];
    }
  }
  const float scale = 1.0f / static_cast<float>(base.size());
  for (float& value : centroid) {
    value *= scale;
  }

  std::uint32_t best_id = 0;
  float best_distance = std::numeric_limits<float>::infinity();
  for (std::size_t row = 0; row < base.size(); ++row) {
    const auto id = static_cast<std::uint32_t>(row);
    const float distance = squared_l2_to_vector(base.row(row), centroid,
                                                base.dim);
    if (distance < best_distance ||
        (distance == best_distance && id < best_id)) {
      best_id = id;
      best_distance = distance;
    }
  }
  return best_id;
}

std::vector<std::uint32_t> select_evenly_spaced_entries(std::uint64_t count,
                                                        std::size_t requested) {
  std::vector<std::uint32_t> entries;
  if (count == 0 || requested == 0) {
    return entries;
  }

  const std::size_t actual =
      std::min<std::size_t>(requested, static_cast<std::size_t>(count));
  std::unordered_set<std::uint32_t> seen;
  entries.reserve(actual);

  for (std::size_t i = 0; i < actual; ++i) {
    std::uint64_t id = 0;
    if (actual == 1) {
      id = 0;
    } else {
      id = (static_cast<std::uint64_t>(i) * (count - 1)) /
           static_cast<std::uint64_t>(actual - 1);
    }
    if (id > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("Entry id exceeds uint32_t range");
    }
    const auto id32 = static_cast<std::uint32_t>(id);
    if (seen.insert(id32).second) {
      entries.push_back(id32);
    }
  }

  return entries;
}

std::vector<std::uint32_t> select_entry_points(
    const VectorSet& base, const EntrySelectionConfig& config,
    const std::vector<std::uint32_t>* node_in_degrees) {
  std::vector<std::uint32_t> entries;
  if (base.empty() || config.entry_count == 0) {
    return entries;
  }

  if (config.strategy == EntryStrategy::SingleMedoid) {
    entries.push_back(select_global_medoid(base));
    return entries;
  }

  if (config.strategy == EntryStrategy::EvenlySpaced) {
    return select_evenly_spaced_entries(base.size(), config.entry_count);
  }

  std::unordered_set<std::uint32_t> seen;
  entries.reserve(config.entry_count);
  append_unique(entries, seen, select_global_medoid(base), config.entry_count);

  if (node_in_degrees != nullptr) {
    const auto hubs = select_hub_entries(*node_in_degrees, base.size(),
                                         config.hub_count);
    for (const auto hub : hubs) {
      append_unique(entries, seen, hub, config.entry_count);
    }
  }

  const std::size_t remaining_after_hubs =
      config.entry_count > entries.size() ? config.entry_count - entries.size()
                                          : 0;
  const std::size_t cluster_count =
      config.cluster_count == 0
          ? remaining_after_hubs
          : std::min(config.cluster_count, remaining_after_hubs);
  const auto cluster_medoids =
      select_cluster_medoids(base, cluster_count, config.cluster_train_limit,
                             config.cluster_iterations);
  for (const auto id : cluster_medoids) {
    append_unique(entries, seen, id, config.entry_count);
  }

  if (entries.size() < config.entry_count) {
    const auto fallback =
        select_evenly_spaced_entries(base.size(), config.entry_count);
    for (const auto id : fallback) {
      append_unique(entries, seen, id, config.entry_count);
    }
  }
  return entries;
}

}  // namespace agent_aware
