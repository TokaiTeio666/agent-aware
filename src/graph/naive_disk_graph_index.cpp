#include "agent_aware/graph/naive_disk_graph_index.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agent_aware/core/async_page_reader.h"
#include "agent_aware/core/brute_force.h"
#include "agent_aware/graph/disk_page_codec.h"

namespace agent_aware {
namespace {

constexpr char kMagic[8] = {'A', 'M', 'F', 'G', 'V', '1', '\0', '\0'};
constexpr std::uint32_t kVersion = 1;

std::size_t record_bytes(std::size_t dim, std::size_t degree,
                         std::size_t neighbor_pq_code_bytes = 0) {
  return graph_record_bytes(dim, degree, neighbor_pq_code_bytes);
}

struct CloserFirst {
  bool operator()(const SearchResult& lhs, const SearchResult& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id > rhs.id;
    }
    return lhs.distance > rhs.distance;
  }
};

struct WorseResultFirst {
  bool operator()(const SearchResult& lhs, const SearchResult& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id < rhs.id;
    }
    return lhs.distance < rhs.distance;
  }
};

std::vector<std::uint32_t> entry_points(std::uint64_t count,
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
    const auto id32 = static_cast<std::uint32_t>(id);
    if (seen.insert(id32).second) {
      entries.push_back(id32);
    }
  }

  return entries;
}

std::vector<SearchResult> sorted_results(
    std::priority_queue<SearchResult, std::vector<SearchResult>,
                        WorseResultFirst>& heap) {
  std::vector<SearchResult> results;
  results.reserve(heap.size());
  while (!heap.empty()) {
    results.push_back(heap.top());
    heap.pop();
  }
  std::sort(results.begin(), results.end(),
            [](const SearchResult& lhs, const SearchResult& rhs) {
              if (lhs.distance == rhs.distance) {
                return lhs.id < rhs.id;
              }
              return lhs.distance < rhs.distance;
            });
  return results;
}

}  // namespace

NaiveDiskGraphIndex::NaiveDiskGraphIndex(const std::string& path)
    : path_(path), input_(path, std::ios::binary) {
  if (!input_) {
    throw std::runtime_error("Cannot open graph index: " + path);
  }

  char magic[8] = {};
  input_.read(magic, sizeof(magic));
  if (!input_ || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
    throw std::runtime_error("Invalid graph index magic: " + path);
  }

  const auto version = read_value<std::uint32_t>(input_);
  if (version != kVersion) {
    throw std::runtime_error("Unsupported graph index version");
  }

  metadata_.vector_count = read_value<std::uint64_t>(input_);
  metadata_.dim = read_value<std::uint32_t>(input_);
  metadata_.degree = read_value<std::uint32_t>(input_);
  metadata_.page_size = read_value<std::uint32_t>(input_);
  metadata_.records_offset = read_value<std::uint64_t>(input_);
  metadata_.neighbor_pq_code_bytes = read_value<std::uint32_t>(input_);
  metadata_.directory_offset = 0;
  metadata_.page_count = metadata_.vector_count;
  metadata_.nodes_per_page = 1;

  if (metadata_.vector_count == 0 || metadata_.dim == 0 ||
      metadata_.degree == 0 || metadata_.page_size == 0) {
    throw std::runtime_error("Graph index metadata is invalid");
  }
  if (record_bytes(metadata_.dim, metadata_.degree,
                   metadata_.neighbor_pq_code_bytes) > metadata_.page_size) {
    throw std::runtime_error("Graph index record is larger than page size");
  }
  page_reader_ = std::make_unique<AsyncPageReader>(path_, metadata_.page_size);
}

NaiveDiskGraphIndex::~NaiveDiskGraphIndex() = default;

void NaiveDiskGraphIndex::configure_io(const std::string& mode,
                                       std::size_t batch_size,
                                       std::size_t io_depth) {
  page_reader_->configure(mode, batch_size, io_depth);
}

const DiskGraphIoStatus& NaiveDiskGraphIndex::io_status() const {
  return page_reader_->status();
}

NaiveDiskGraphIndex::DiskNode NaiveDiskGraphIndex::read_node(
    std::uint32_t id, DiskGraphSearchStats& stats) {
  if (id >= metadata_.vector_count) {
    throw std::runtime_error("Graph node id out of range");
  }
  ++stats.page_requests;
  ++stats.page_requests_before_dedup;
  ++stats.page_requests_after_dedup;
  ++stats.page_cache_misses;
  ++stats.demand_reads;

  const std::uint64_t offset =
      metadata_.records_offset +
      static_cast<std::uint64_t>(id) * metadata_.page_size;
  std::vector<char> page =
      page_reader_->read(offset, metadata_.page_size, stats);

  std::size_t cursor = 0;
  DiskNode node;
  node.id = get_bytes<std::uint32_t>(page, cursor);
  const auto degree = get_bytes<std::uint32_t>(page, cursor);
  if (node.id != id || degree > metadata_.degree) {
    throw std::runtime_error("Corrupted graph node page");
  }

  node.vector.resize(metadata_.dim);
  std::memcpy(node.vector.data(), page.data() + cursor,
              metadata_.dim * sizeof(float));
  cursor += metadata_.dim * sizeof(float);

  node.neighbors.reserve(degree);
  for (std::uint32_t i = 0; i < metadata_.degree; ++i) {
    const auto neighbor = get_bytes<std::uint32_t>(page, cursor);
    if (i < degree && neighbor != std::numeric_limits<std::uint32_t>::max()) {
      node.neighbors.push_back(neighbor);
    }
  }
  if (metadata_.neighbor_pq_code_bytes > 0) {
    node.neighbor_pq_codes.resize(
        static_cast<std::size_t>(degree) * metadata_.neighbor_pq_code_bytes);
    std::size_t written = 0;
    for (std::uint32_t i = 0; i < metadata_.degree; ++i) {
      for (std::size_t b = 0; b < metadata_.neighbor_pq_code_bytes; ++b) {
        const auto code = get_bytes<std::uint8_t>(page, cursor);
        if (i < degree) {
          node.neighbor_pq_codes[written++] = code;
        }
      }
    }
  }
  return node;
}

DiskGraphSearchResult NaiveDiskGraphIndex::search_one(
    const float* query, const DiskGraphSearchConfig& config) {
  if (config.top_k == 0 || config.search_width == 0 ||
      config.entry_count == 0) {
    throw std::runtime_error("Graph search top_k, search_width, and entry_count must be positive");
  }
  const std::size_t effective_beam_width =
      std::clamp(config.beam_width == 0 ? config.search_width
                                         : config.beam_width,
                 std::size_t{1}, config.search_width);

  DiskGraphSearchResult output;
  std::unordered_set<std::uint32_t> visited;
  std::unordered_map<std::uint32_t, DiskNode> local_nodes;
  std::priority_queue<SearchResult, std::vector<SearchResult>, CloserFirst>
      candidates;
  std::priority_queue<SearchResult, std::vector<SearchResult>, WorseResultFirst>
      best;

  auto load_node = [&](std::uint32_t id) -> DiskNode& {
    auto found = local_nodes.find(id);
    if (found != local_nodes.end()) {
      return found->second;
    }
    DiskNode node = read_node(id, output.stats);
    ++output.stats.node_reads;
    auto inserted = local_nodes.emplace(id, std::move(node));
    return inserted.first->second;
  };

  std::vector<float> adc_table;
  if (config.adc_enable && config.pq_model != nullptr) {
    const auto adc_start = std::chrono::steady_clock::now();
    adc_table = config.pq_model->build_adc_table(query);
    const auto adc_end = std::chrono::steady_clock::now();
    output.stats.adc_table_build_us =
        std::chrono::duration<double, std::micro>(adc_end - adc_start).count();
  }
  auto candidate_distance = [&](std::uint32_t id) {
    if (!adc_table.empty()) {
      ++output.stats.pq_filter_accept_count;
      return config.pq_model->adc_distance(id, adc_table);
    }
    const DiskNode& node = load_node(id);
    return squared_l2(query, node.vector.data(), metadata_.dim);
  };

  const std::size_t effective_k =
      std::min<std::size_t>(config.top_k,
                            static_cast<std::size_t>(metadata_.vector_count));
  const std::size_t result_capacity =
      std::min<std::size_t>(
          std::max(effective_k, config.rerank_topk),
          static_cast<std::size_t>(metadata_.vector_count));

  auto add_best = [&](const SearchResult& result) {
    if (best.size() < result_capacity) {
      best.push(result);
    } else {
      const auto& worst = best.top();
      if (result.distance < worst.distance ||
          (result.distance == worst.distance && result.id < worst.id)) {
        best.pop();
        best.push(result);
      }
    }
  };

  auto should_stop = [&](const SearchResult& next) {
    if (!config.early_stop || best.size() < result_capacity ||
        output.stats.expanded < config.early_stop_min_expansions) {
      return false;
    }
    const auto& worst = best.top();
    return next.distance > worst.distance;
  };
  std::size_t stagnant_expansions = 0;
  double previous_worst_distance = std::numeric_limits<double>::infinity();
  auto update_adaptive_stop = [&]() {
    if (!config.adaptive_early_stop || best.size() < result_capacity) {
      return;
    }
    const double current = best.top().distance;
    const double improvement =
        std::isfinite(previous_worst_distance)
            ? (previous_worst_distance - current) /
                  std::max(1.0, std::abs(previous_worst_distance))
            : std::numeric_limits<double>::infinity();
    if (improvement > config.early_stop_eps) {
      stagnant_expansions = 0;
    } else {
      ++stagnant_expansions;
    }
    previous_worst_distance = current;
  };

  const std::vector<std::uint32_t> entries =
      config.seed_ids.empty()
          ? entry_points(metadata_.vector_count, config.entry_count)
          : config.seed_ids;

  for (const auto entry : entries) {
    if (entry >= metadata_.vector_count) {
      continue;
    }
    if (!visited.insert(entry).second) {
      continue;
    }
    const float distance = candidate_distance(entry);
    const SearchResult result{entry, distance};
    candidates.push(result);
    add_best(result);
  }

  while (!candidates.empty() && output.stats.expanded < config.search_width) {
    std::size_t expanded_this_round = 0;
    while (!candidates.empty() && output.stats.expanded < config.search_width &&
           expanded_this_round < effective_beam_width) {
      if (config.adaptive_early_stop &&
          output.stats.expanded >= config.min_expansions &&
          stagnant_expansions >= config.early_stop_patience) {
        candidates = {};
        break;
      }
      const SearchResult current = candidates.top();
      if (should_stop(current)) {
        candidates = {};
        break;
      }
      candidates.pop();
      const DiskNode& node = load_node(current.id);
      ++output.stats.expanded;

      for (const auto neighbor_id : node.neighbors) {
        if (neighbor_id >= metadata_.vector_count ||
            !visited.insert(neighbor_id).second) {
          continue;
        }
        const float distance = candidate_distance(neighbor_id);
        const SearchResult candidate{neighbor_id, distance};
        candidates.push(candidate);
        add_best(candidate);
      }
      update_adaptive_stop();
      ++expanded_this_round;
    }
    if (expanded_this_round > 0) {
      ++output.stats.batch_count;
      output.stats.batch_expanded += expanded_this_round;
      output.stats.max_batch_size =
          std::max(output.stats.max_batch_size, expanded_this_round);
    }
  }

  output.stats.visited = visited.size();
  output.topk = sorted_results(best);
  if (!adc_table.empty() && config.rerank_topk > 0) {
    const std::size_t reads_before_rerank = output.stats.node_reads;
    for (auto& result : output.topk) {
      const DiskNode& node = load_node(result.id);
      result.distance = squared_l2(query, node.vector.data(), metadata_.dim);
    }
    output.stats.rerank_reads += output.stats.node_reads - reads_before_rerank;
    std::sort(output.topk.begin(), output.topk.end(),
              [](const SearchResult& lhs, const SearchResult& rhs) {
                return lhs.distance == rhs.distance ? lhs.id < rhs.id
                                                    : lhs.distance < rhs.distance;
              });
  }
  if (output.topk.size() > effective_k) {
    output.topk.resize(effective_k);
  }
  return output;
}


}  // namespace agent_aware
