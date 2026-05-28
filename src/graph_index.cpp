#include "agentmem/graph_index.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "agentmem/brute_force.h"

namespace agentmem {
namespace {

constexpr char kMagic[8] = {'A', 'M', 'F', 'G', 'V', '1', '\0', '\0'};
constexpr char kPackedMagic[8] = {'A', 'M', 'F', 'P', 'V', '2', '\0', '\0'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kPackedVersion = 2;
constexpr std::uint64_t kRecordsOffset = 4096;

struct NeighborCandidate {
  std::uint32_t id = 0;
  float distance = 0.0f;
};

struct WorseNeighborFirst {
  bool operator()(const NeighborCandidate& lhs,
                  const NeighborCandidate& rhs) const {
    if (lhs.distance == rhs.distance) {
      return lhs.id < rhs.id;
    }
    return lhs.distance < rhs.distance;
  }
};

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

template <typename T>
void write_value(std::ofstream& output, const T& value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(T));
  if (!output) {
    throw std::runtime_error("Failed to write graph index");
  }
}

template <typename T>
T read_value(std::ifstream& input) {
  T value{};
  input.read(reinterpret_cast<char*>(&value), sizeof(T));
  if (!input) {
    throw std::runtime_error("Failed to read graph index header");
  }
  return value;
}

template <typename T>
void put_bytes(std::vector<char>& page, std::size_t& offset, const T& value) {
  if (offset + sizeof(T) > page.size()) {
    throw std::runtime_error("Graph node page overflow");
  }
  std::memcpy(page.data() + offset, &value, sizeof(T));
  offset += sizeof(T);
}

template <typename T>
T get_bytes(const std::vector<char>& page, std::size_t& offset) {
  if (offset + sizeof(T) > page.size()) {
    throw std::runtime_error("Graph node page is truncated");
  }
  T value{};
  std::memcpy(&value, page.data() + offset, sizeof(T));
  offset += sizeof(T);
  return value;
}

std::size_t record_bytes(std::size_t dim, std::size_t degree) {
  return sizeof(std::uint32_t) * 2 + dim * sizeof(float) +
         degree * sizeof(std::uint32_t);
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
  if (alignment == 0) {
    return value;
  }
  const std::uint64_t remainder = value % alignment;
  return remainder == 0 ? value : value + alignment - remainder;
}

std::uint64_t pair_key(std::uint32_t lhs, std::uint32_t rhs) {
  const std::uint32_t a = std::min(lhs, rhs);
  const std::uint32_t b = std::max(lhs, rhs);
  return (static_cast<std::uint64_t>(a) << 32) | b;
}

bool contains_id(const std::vector<std::uint32_t>& ids, std::uint32_t target) {
  return std::find(ids.begin(), ids.end(), target) != ids.end();
}

std::vector<std::uint32_t> exact_neighbors(const VectorSet& base,
                                           std::size_t node_id,
                                           std::size_t degree) {
  const std::size_t effective_degree =
      base.size() == 0 ? 0 : std::min(degree, base.size() - 1);
  std::priority_queue<NeighborCandidate, std::vector<NeighborCandidate>,
                      WorseNeighborFirst>
      heap;
  const float* source = base.row(node_id);

  for (std::size_t other = 0; other < base.size(); ++other) {
    if (other == node_id) {
      continue;
    }
    const float distance = squared_l2(source, base.row(other), base.dim);
    const NeighborCandidate item{static_cast<std::uint32_t>(other), distance};
    if (heap.size() < effective_degree) {
      heap.push(item);
    } else {
      const auto& worst = heap.top();
      if (distance < worst.distance ||
          (distance == worst.distance && item.id < worst.id)) {
        heap.pop();
        heap.push(item);
      }
    }
  }

  std::vector<NeighborCandidate> candidates;
  candidates.reserve(heap.size());
  while (!heap.empty()) {
    candidates.push_back(heap.top());
    heap.pop();
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const NeighborCandidate& lhs, const NeighborCandidate& rhs) {
              if (lhs.distance == rhs.distance) {
                return lhs.id < rhs.id;
              }
              return lhs.distance < rhs.distance;
            });

  std::vector<std::uint32_t> ids;
  ids.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    ids.push_back(candidate.id);
  }
  return ids;
}

std::vector<std::vector<std::uint32_t>> build_exact_graph(
    const VectorSet& base, std::size_t degree) {
  std::vector<std::vector<std::uint32_t>> graph;
  graph.reserve(base.size());
  for (std::size_t i = 0; i < base.size(); ++i) {
    graph.push_back(exact_neighbors(base, i, degree));
  }
  return graph;
}

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

std::size_t packed_nodes_per_page(std::size_t dim, std::size_t degree,
                                  std::size_t page_size) {
  if (page_size <= sizeof(std::uint32_t) * 2) {
    return 0;
  }
  const std::size_t per_node = sizeof(std::uint32_t) * 2 +
                               dim * sizeof(float) +
                               degree * sizeof(std::uint32_t);
  return (page_size - sizeof(std::uint32_t) * 2) / per_node;
}

std::vector<std::uint32_t> natural_order(std::size_t count) {
  std::vector<std::uint32_t> order;
  order.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    order.push_back(static_cast<std::uint32_t>(i));
  }
  return order;
}

std::vector<std::uint32_t> random_order(std::size_t count, std::uint32_t seed) {
  auto order = natural_order(count);
  std::mt19937 rng(seed);
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

std::vector<std::uint32_t> bfs_order(
    const std::vector<std::vector<std::uint32_t>>& graph) {
  std::vector<std::uint32_t> order;
  order.reserve(graph.size());
  std::vector<char> seen(graph.size(), 0);
  std::deque<std::uint32_t> queue;

  for (std::size_t start = 0; start < graph.size(); ++start) {
    if (seen[start]) {
      continue;
    }
    seen[start] = 1;
    queue.push_back(static_cast<std::uint32_t>(start));
    while (!queue.empty()) {
      const auto current = queue.front();
      queue.pop_front();
      order.push_back(current);
      for (const auto neighbor : graph[current]) {
        if (neighbor < seen.size() && !seen[neighbor]) {
          seen[neighbor] = 1;
          queue.push_back(neighbor);
        }
      }
    }
  }

  return order;
}

std::uint32_t pick_best_unplaced(const std::vector<char>& placed,
                                 const std::vector<double>& hotness) {
  std::uint32_t best = 0;
  double best_score = -1.0;
  for (std::size_t i = 0; i < placed.size(); ++i) {
    if (!placed[i] && hotness[i] > best_score) {
      best_score = hotness[i];
      best = static_cast<std::uint32_t>(i);
    }
  }
  return best;
}

std::vector<std::uint32_t> coaccess_order(
    const std::vector<std::vector<std::uint32_t>>& graph,
    std::size_t nodes_per_page, std::size_t sessions, std::size_t trace_length) {
  std::vector<std::uint32_t> order;
  order.reserve(graph.size());
  if (graph.empty()) {
    return order;
  }

  sessions = std::max<std::size_t>(1, std::min(sessions, graph.size()));
  trace_length = std::max<std::size_t>(1, trace_length);

  std::vector<double> hotness(graph.size(), 1.0);
  std::unordered_map<std::uint64_t, double> coaccess;

  for (std::size_t s = 0; s < sessions; ++s) {
    std::uint32_t current = static_cast<std::uint32_t>(
        (s * graph.size()) / sessions);
    std::uint32_t previous = std::numeric_limits<std::uint32_t>::max();

    for (std::size_t step = 0; step < trace_length; ++step) {
      hotness[current] += 2.0;
      if (previous != std::numeric_limits<std::uint32_t>::max()) {
        coaccess[pair_key(previous, current)] += 4.0;
      }

      const auto& neighbors = graph[current];
      for (std::size_t i = 0; i < std::min<std::size_t>(4, neighbors.size());
           ++i) {
        coaccess[pair_key(current, neighbors[i])] += 1.0;
      }

      if (neighbors.empty()) {
        break;
      }
      previous = current;
      current = neighbors[(step + s) % neighbors.size()];
    }
  }

  std::vector<char> placed(graph.size(), 0);
  std::size_t remaining = graph.size();

  while (remaining > 0) {
    std::vector<std::uint32_t> page_nodes;
    page_nodes.reserve(nodes_per_page);
    const std::uint32_t anchor = pick_best_unplaced(placed, hotness);
    placed[anchor] = 1;
    --remaining;
    page_nodes.push_back(anchor);
    order.push_back(anchor);

    while (remaining > 0 && page_nodes.size() < nodes_per_page) {
      std::uint32_t best = 0;
      double best_score = -1.0;

      for (std::size_t candidate = 0; candidate < graph.size(); ++candidate) {
        if (placed[candidate]) {
          continue;
        }

        double score = hotness[candidate] * 0.001;
        const auto candidate_id = static_cast<std::uint32_t>(candidate);
        for (const auto page_node : page_nodes) {
          const auto found = coaccess.find(pair_key(candidate_id, page_node));
          if (found != coaccess.end()) {
            score += found->second * 20.0;
          }
          if (contains_id(graph[candidate], page_node) ||
              contains_id(graph[page_node], candidate_id)) {
            score += 10.0;
          }
        }

        if (score > best_score) {
          best_score = score;
          best = candidate_id;
        }
      }

      placed[best] = 1;
      --remaining;
      page_nodes.push_back(best);
      order.push_back(best);
    }
  }

  return order;
}

std::vector<std::uint32_t> packed_order(
    const std::vector<std::vector<std::uint32_t>>& graph,
    const DiskGraphBuildConfig& config, std::size_t nodes_per_page) {
  if (config.packing_strategy == "random") {
    return random_order(graph.size(), config.random_seed);
  }
  if (config.packing_strategy == "bfs") {
    return bfs_order(graph);
  }
  if (config.packing_strategy == "coaccess") {
    return coaccess_order(graph, nodes_per_page, config.coaccess_sessions,
                          config.coaccess_trace_length);
  }
  throw std::runtime_error("Unknown packing strategy: " +
                           config.packing_strategy);
}

}  // namespace

void NaiveDiskGraphBuilder::build(const VectorSet& base,
                                  const std::string& path,
                                  const DiskGraphBuildConfig& config) {
  if (base.empty()) {
    throw std::runtime_error("Cannot build graph index for empty base vectors");
  }
  if (base.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("V1 graph index supports at most uint32_t ids");
  }
  if (config.degree == 0) {
    throw std::runtime_error("Graph degree must be positive");
  }
  if (config.page_size < kRecordsOffset) {
    throw std::runtime_error("Page size must be at least 4096 bytes in V1");
  }
  if (record_bytes(base.dim, config.degree) > config.page_size) {
    throw std::runtime_error("Node record does not fit in configured page size");
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create graph index: " + path);
  }

  output.write(kMagic, sizeof(kMagic));
  write_value(output, kVersion);
  write_value(output, static_cast<std::uint64_t>(base.size()));
  write_value(output, static_cast<std::uint32_t>(base.dim));
  write_value(output, static_cast<std::uint32_t>(config.degree));
  write_value(output, static_cast<std::uint32_t>(config.page_size));
  write_value(output, kRecordsOffset);

  const auto header_pos = static_cast<std::uint64_t>(output.tellp());
  if (header_pos > kRecordsOffset) {
    throw std::runtime_error("Graph header exceeds reserved header page");
  }
  std::vector<char> header_padding(
      static_cast<std::size_t>(kRecordsOffset - header_pos), 0);
  output.write(header_padding.data(),
               static_cast<std::streamsize>(header_padding.size()));

  for (std::size_t i = 0; i < base.size(); ++i) {
    const auto neighbors = exact_neighbors(base, i, config.degree);
    std::vector<char> page(config.page_size, 0);
    std::size_t offset = 0;
    const auto id = static_cast<std::uint32_t>(i);
    const auto degree = static_cast<std::uint32_t>(neighbors.size());
    put_bytes(page, offset, id);
    put_bytes(page, offset, degree);
    std::memcpy(page.data() + offset, base.row(i), base.dim * sizeof(float));
    offset += base.dim * sizeof(float);

    for (std::size_t n = 0; n < config.degree; ++n) {
      const std::uint32_t neighbor =
          n < neighbors.size() ? neighbors[n] : std::numeric_limits<std::uint32_t>::max();
      put_bytes(page, offset, neighbor);
    }

    output.write(page.data(), static_cast<std::streamsize>(page.size()));
    if (!output) {
      throw std::runtime_error("Failed while writing graph node page");
    }
  }
}

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
  metadata_.directory_offset = 0;
  metadata_.page_count = metadata_.vector_count;
  metadata_.nodes_per_page = 1;

  if (metadata_.vector_count == 0 || metadata_.dim == 0 ||
      metadata_.degree == 0 || metadata_.page_size == 0) {
    throw std::runtime_error("Graph index metadata is invalid");
  }
  if (record_bytes(metadata_.dim, metadata_.degree) > metadata_.page_size) {
    throw std::runtime_error("Graph index record is larger than page size");
  }
}

NaiveDiskGraphIndex::DiskNode NaiveDiskGraphIndex::read_node(std::uint32_t id) {
  if (id >= metadata_.vector_count) {
    throw std::runtime_error("Graph node id out of range");
  }

  const std::uint64_t offset =
      metadata_.records_offset +
      static_cast<std::uint64_t>(id) * metadata_.page_size;
  input_.clear();
  input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input_) {
    throw std::runtime_error("Failed to seek graph index: " + path_);
  }

  std::vector<char> page(metadata_.page_size, 0);
  input_.read(page.data(), static_cast<std::streamsize>(page.size()));
  if (!input_) {
    throw std::runtime_error("Failed to read graph node page");
  }

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
  return node;
}

DiskGraphSearchResult NaiveDiskGraphIndex::search_one(
    const float* query, const DiskGraphSearchConfig& config) {
  if (config.top_k == 0 || config.search_width == 0 ||
      config.entry_count == 0) {
    throw std::runtime_error("Graph search top_k, search_width, and entry_count must be positive");
  }

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
    DiskNode node = read_node(id);
    ++output.stats.node_reads;
    auto inserted = local_nodes.emplace(id, std::move(node));
    return inserted.first->second;
  };

  auto add_best = [&](const SearchResult& result) {
    const std::size_t effective_k =
        std::min<std::size_t>(config.top_k,
                              static_cast<std::size_t>(metadata_.vector_count));
    if (best.size() < effective_k) {
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
    const DiskNode& node = load_node(entry);
    const float distance = squared_l2(query, node.vector.data(), metadata_.dim);
    const SearchResult result{entry, distance};
    candidates.push(result);
    add_best(result);
  }

  while (!candidates.empty() && output.stats.expanded < config.search_width) {
    const SearchResult current = candidates.top();
    candidates.pop();
    const DiskNode& node = load_node(current.id);
    ++output.stats.expanded;

    for (const auto neighbor_id : node.neighbors) {
      if (neighbor_id >= metadata_.vector_count ||
          !visited.insert(neighbor_id).second) {
        continue;
      }
      const DiskNode& neighbor = load_node(neighbor_id);
      const float distance =
          squared_l2(query, neighbor.vector.data(), metadata_.dim);
      const SearchResult candidate{neighbor_id, distance};
      candidates.push(candidate);
      add_best(candidate);
    }
  }

  output.stats.visited = visited.size();
  output.topk = sorted_results(best);
  return output;
}

void PackedDiskGraphBuilder::build(const VectorSet& base,
                                   const std::string& path,
                                   const DiskGraphBuildConfig& config) {
  if (base.empty()) {
    throw std::runtime_error("Cannot build packed graph index for empty base vectors");
  }
  if (base.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("V2 packed graph index supports at most uint32_t ids");
  }
  if (config.degree == 0) {
    throw std::runtime_error("Graph degree must be positive");
  }
  if (config.page_size < kRecordsOffset) {
    throw std::runtime_error("Page size must be at least 4096 bytes in V2");
  }

  const std::size_t nodes_per_page =
      packed_nodes_per_page(base.dim, config.degree, config.page_size);
  if (nodes_per_page == 0) {
    throw std::runtime_error("Packed page cannot hold even one node");
  }

  const auto graph = build_exact_graph(base, config.degree);
  const auto order = packed_order(graph, config, nodes_per_page);
  if (order.size() != base.size()) {
    throw std::runtime_error("Packed order does not cover all nodes");
  }

  const std::uint64_t page_count =
      (static_cast<std::uint64_t>(base.size()) + nodes_per_page - 1) /
      nodes_per_page;
  if (page_count > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("Too many packed pages for V2 directory");
  }

  const std::uint64_t directory_offset = kRecordsOffset;
  const std::uint64_t directory_bytes =
      static_cast<std::uint64_t>(base.size()) * sizeof(std::uint32_t);
  const std::uint64_t records_offset =
      align_up(directory_offset + directory_bytes, config.page_size);

  std::vector<std::uint32_t> node_to_page(base.size(), 0);
  for (std::size_t position = 0; position < order.size(); ++position) {
    const std::uint32_t page_id =
        static_cast<std::uint32_t>(position / nodes_per_page);
    node_to_page[order[position]] = page_id;
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create packed graph index: " + path);
  }

  output.write(kPackedMagic, sizeof(kPackedMagic));
  write_value(output, kPackedVersion);
  write_value(output, static_cast<std::uint64_t>(base.size()));
  write_value(output, static_cast<std::uint32_t>(base.dim));
  write_value(output, static_cast<std::uint32_t>(config.degree));
  write_value(output, static_cast<std::uint32_t>(config.page_size));
  write_value(output, records_offset);
  write_value(output, directory_offset);
  write_value(output, page_count);
  write_value(output, static_cast<std::uint32_t>(nodes_per_page));

  const auto header_pos = static_cast<std::uint64_t>(output.tellp());
  if (header_pos > kRecordsOffset) {
    throw std::runtime_error("Packed graph header exceeds reserved header page");
  }
  std::vector<char> header_padding(
      static_cast<std::size_t>(kRecordsOffset - header_pos), 0);
  output.write(header_padding.data(),
               static_cast<std::streamsize>(header_padding.size()));

  output.write(reinterpret_cast<const char*>(node_to_page.data()),
               static_cast<std::streamsize>(directory_bytes));
  const auto after_directory = static_cast<std::uint64_t>(output.tellp());
  if (after_directory > records_offset) {
    throw std::runtime_error("Packed graph directory exceeds records offset");
  }
  std::vector<char> directory_padding(
      static_cast<std::size_t>(records_offset - after_directory), 0);
  output.write(directory_padding.data(),
               static_cast<std::streamsize>(directory_padding.size()));

  for (std::uint64_t page_id = 0; page_id < page_count; ++page_id) {
    const std::size_t begin =
        static_cast<std::size_t>(page_id) * nodes_per_page;
    const std::size_t end = std::min(begin + nodes_per_page, order.size());
    const auto node_count = static_cast<std::uint32_t>(end - begin);

    std::vector<char> page(config.page_size, 0);
    std::size_t offset = 0;
    put_bytes(page, offset, static_cast<std::uint32_t>(page_id));
    put_bytes(page, offset, node_count);

    for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {
      const std::uint32_t id =
          slot < node_count ? order[begin + slot]
                            : std::numeric_limits<std::uint32_t>::max();
      put_bytes(page, offset, id);
    }

    for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {
      const std::uint32_t degree =
          slot < node_count
              ? static_cast<std::uint32_t>(graph[order[begin + slot]].size())
              : 0;
      put_bytes(page, offset, degree);
    }

    for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {
      if (offset + base.dim * sizeof(float) > page.size()) {
        throw std::runtime_error("Packed vector block exceeds page size");
      }
      if (slot < node_count) {
        std::memcpy(page.data() + offset, base.row(order[begin + slot]),
                    base.dim * sizeof(float));
      }
      offset += base.dim * sizeof(float);
    }

    for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {
      const std::uint32_t node_id =
          slot < node_count ? order[begin + slot]
                            : std::numeric_limits<std::uint32_t>::max();
      for (std::size_t n = 0; n < config.degree; ++n) {
        std::uint32_t neighbor = std::numeric_limits<std::uint32_t>::max();
        if (slot < node_count && n < graph[node_id].size()) {
          neighbor = graph[node_id][n];
        }
        put_bytes(page, offset, neighbor);
      }
    }

    output.write(page.data(), static_cast<std::streamsize>(page.size()));
    if (!output) {
      throw std::runtime_error("Failed while writing packed graph page");
    }
  }
}

PackedDiskGraphIndex::PackedDiskGraphIndex(const std::string& path)
    : path_(path), input_(path, std::ios::binary) {
  if (!input_) {
    throw std::runtime_error("Cannot open packed graph index: " + path);
  }

  char magic[8] = {};
  input_.read(magic, sizeof(magic));
  if (!input_ || std::memcmp(magic, kPackedMagic, sizeof(kPackedMagic)) != 0) {
    throw std::runtime_error("Invalid packed graph index magic: " + path);
  }

  const auto version = read_value<std::uint32_t>(input_);
  if (version != kPackedVersion) {
    throw std::runtime_error("Unsupported packed graph index version");
  }

  metadata_.vector_count = read_value<std::uint64_t>(input_);
  metadata_.dim = read_value<std::uint32_t>(input_);
  metadata_.degree = read_value<std::uint32_t>(input_);
  metadata_.page_size = read_value<std::uint32_t>(input_);
  metadata_.records_offset = read_value<std::uint64_t>(input_);
  metadata_.directory_offset = read_value<std::uint64_t>(input_);
  metadata_.page_count = read_value<std::uint64_t>(input_);
  metadata_.nodes_per_page = read_value<std::uint32_t>(input_);

  if (metadata_.vector_count == 0 || metadata_.dim == 0 ||
      metadata_.degree == 0 || metadata_.page_size == 0 ||
      metadata_.page_count == 0 || metadata_.nodes_per_page == 0) {
    throw std::runtime_error("Packed graph index metadata is invalid");
  }

  node_to_page_.resize(static_cast<std::size_t>(metadata_.vector_count));
  input_.clear();
  input_.seekg(static_cast<std::streamoff>(metadata_.directory_offset),
               std::ios::beg);
  input_.read(reinterpret_cast<char*>(node_to_page_.data()),
              static_cast<std::streamsize>(node_to_page_.size() *
                                           sizeof(std::uint32_t)));
  if (!input_) {
    throw std::runtime_error("Failed to read packed graph directory");
  }
}

void PackedDiskGraphIndex::configure_cache(const std::string& policy,
                                           std::size_t capacity_pages) {
  if (policy != "none" && policy != "lru" && policy != "agent") {
    throw std::runtime_error("Cache policy must be none, lru, or agent");
  }
  if (policy != cache_policy_ || capacity_pages != cache_capacity_pages_) {
    page_cache_.clear();
    cache_clock_ = 0;
  }
  cache_policy_ = policy;
  cache_capacity_pages_ = capacity_pages;
}

PackedDiskGraphIndex::DecodedPage PackedDiskGraphIndex::read_page(
    std::uint32_t page_id) {
  if (page_id >= metadata_.page_count) {
    throw std::runtime_error("Packed page id out of range");
  }

  const std::uint64_t offset =
      metadata_.records_offset +
      static_cast<std::uint64_t>(page_id) * metadata_.page_size;
  input_.clear();
  input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input_) {
    throw std::runtime_error("Failed to seek packed graph index: " + path_);
  }

  std::vector<char> page(metadata_.page_size, 0);
  input_.read(page.data(), static_cast<std::streamsize>(page.size()));
  if (!input_) {
    throw std::runtime_error("Failed to read packed graph page");
  }

  std::size_t cursor = 0;
  DecodedPage decoded;
  decoded.page_id = get_bytes<std::uint32_t>(page, cursor);
  const auto node_count = get_bytes<std::uint32_t>(page, cursor);
  if (decoded.page_id != page_id || node_count > metadata_.nodes_per_page) {
    throw std::runtime_error("Corrupted packed graph page header");
  }

  std::vector<std::uint32_t> ids(metadata_.nodes_per_page);
  std::vector<std::uint32_t> degrees(metadata_.nodes_per_page);
  for (std::uint32_t i = 0; i < metadata_.nodes_per_page; ++i) {
    ids[i] = get_bytes<std::uint32_t>(page, cursor);
  }
  for (std::uint32_t i = 0; i < metadata_.nodes_per_page; ++i) {
    degrees[i] = get_bytes<std::uint32_t>(page, cursor);
  }

  decoded.nodes.reserve(node_count);
  for (std::uint32_t slot = 0; slot < metadata_.nodes_per_page; ++slot) {
    if (cursor + metadata_.dim * sizeof(float) > page.size()) {
      throw std::runtime_error("Packed vector block is truncated");
    }
    if (slot < node_count) {
      DiskNode node;
      node.id = ids[slot];
      node.vector.resize(metadata_.dim);
      std::memcpy(node.vector.data(), page.data() + cursor,
                  metadata_.dim * sizeof(float));
      decoded.nodes.push_back(std::move(node));
    }
    cursor += metadata_.dim * sizeof(float);
  }

  for (std::uint32_t slot = 0; slot < metadata_.nodes_per_page; ++slot) {
    std::vector<std::uint32_t> neighbors;
    if (slot < node_count) {
      neighbors.reserve(degrees[slot]);
    }
    for (std::uint32_t n = 0; n < metadata_.degree; ++n) {
      const auto neighbor = get_bytes<std::uint32_t>(page, cursor);
      if (slot < node_count && n < degrees[slot] &&
          neighbor != std::numeric_limits<std::uint32_t>::max()) {
        neighbors.push_back(neighbor);
      }
    }
    if (slot < node_count) {
      decoded.nodes[slot].neighbors = std::move(neighbors);
    }
  }

  return decoded;
}

double PackedDiskGraphIndex::cache_score(const CacheEntry& entry) const {
  if (cache_policy_ == "lru") {
    return static_cast<double>(entry.last_access);
  }

  const double frequency = static_cast<double>(entry.frequency);
  const double recency = static_cast<double>(entry.last_access);
  const double density = static_cast<double>(entry.page.nodes.size());
  return frequency * 1000.0 + recency + density * 0.01;
}

void PackedDiskGraphIndex::evict_one_page() {
  if (page_cache_.empty()) {
    return;
  }

  auto victim = page_cache_.begin();
  double victim_score = cache_score(victim->second);
  for (auto it = page_cache_.begin(); it != page_cache_.end(); ++it) {
    const double score = cache_score(it->second);
    if (score < victim_score ||
        (score == victim_score && it->first < victim->first)) {
      victim = it;
      victim_score = score;
    }
  }
  page_cache_.erase(victim);
}

const PackedDiskGraphIndex::DecodedPage& PackedDiskGraphIndex::load_page(
    std::uint32_t page_id, DiskGraphSearchStats& stats) {
  ++stats.page_requests;

  if (cache_policy_ != "none" && cache_capacity_pages_ > 0) {
    auto found = page_cache_.find(page_id);
    if (found != page_cache_.end()) {
      ++stats.page_cache_hits;
      found->second.last_access = ++cache_clock_;
      ++found->second.frequency;
      return found->second.page;
    }
  }

  ++stats.page_cache_misses;
  ++stats.node_reads;
  DecodedPage page = read_page(page_id);

  if (cache_policy_ == "none" || cache_capacity_pages_ == 0) {
    scratch_page_ = std::move(page);
    return scratch_page_;
  }

  if (page_cache_.size() >= cache_capacity_pages_) {
    evict_one_page();
  }

  CacheEntry entry;
  entry.page = std::move(page);
  entry.last_access = ++cache_clock_;
  entry.frequency = 1;
  auto inserted = page_cache_.emplace(page_id, std::move(entry));
  return inserted.first->second.page;
}

DiskGraphSearchResult PackedDiskGraphIndex::search_one(
    const float* query, const DiskGraphSearchConfig& config) {
  if (config.top_k == 0 || config.search_width == 0 ||
      config.entry_count == 0) {
    throw std::runtime_error("Packed graph search top_k, search_width, and entry_count must be positive");
  }

  DiskGraphSearchResult output;
  std::unordered_set<std::uint32_t> visited;
  std::unordered_map<std::uint32_t, DiskNode> local_nodes;
  std::priority_queue<SearchResult, std::vector<SearchResult>, CloserFirst>
      candidates;
  std::priority_queue<SearchResult, std::vector<SearchResult>, WorseResultFirst>
      best;

  auto load_node = [&](std::uint32_t id) -> DiskNode& {
    auto node_found = local_nodes.find(id);
    if (node_found != local_nodes.end()) {
      return node_found->second;
    }

    const std::uint32_t page_id = node_to_page_[id];
    const DecodedPage& page = load_page(page_id, output.stats);
    for (const auto& node : page.nodes) {
      local_nodes.emplace(node.id, node);
    }

    node_found = local_nodes.find(id);
    if (node_found == local_nodes.end()) {
      throw std::runtime_error("Node is missing from packed page");
    }
    return node_found->second;
  };

  auto add_best = [&](const SearchResult& result) {
    const std::size_t effective_k =
        std::min<std::size_t>(config.top_k,
                              static_cast<std::size_t>(metadata_.vector_count));
    if (best.size() < effective_k) {
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
    const DiskNode& node = load_node(entry);
    const float distance = squared_l2(query, node.vector.data(), metadata_.dim);
    const SearchResult result{entry, distance};
    candidates.push(result);
    add_best(result);
  }

  while (!candidates.empty() && output.stats.expanded < config.search_width) {
    const SearchResult current = candidates.top();
    candidates.pop();
    const DiskNode& node = load_node(current.id);
    ++output.stats.expanded;

    for (const auto neighbor_id : node.neighbors) {
      if (neighbor_id >= metadata_.vector_count ||
          !visited.insert(neighbor_id).second) {
        continue;
      }
      const DiskNode& neighbor = load_node(neighbor_id);
      const float distance =
          squared_l2(query, neighbor.vector.data(), metadata_.dim);
      const SearchResult candidate{neighbor_id, distance};
      candidates.push(candidate);
      add_best(candidate);
    }
  }

  output.stats.visited = visited.size();
  output.topk = sorted_results(best);
  return output;
}

}  // namespace agentmem
