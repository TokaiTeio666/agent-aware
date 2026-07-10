#include "agent_aware/graph/disk_graph_builder.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "agent_aware/graph/disk_page_codec.h"
#include "agent_aware/graph/vamana_builder.h"

namespace agent_aware {
namespace {

constexpr char kPackedMagic[8] = {'A', 'M', 'F', 'P', 'V', '2', '\0', '\0'};
constexpr std::uint32_t kPackedVersion = 2;
constexpr std::uint64_t kRecordsOffset = 4096;

std::vector<std::vector<std::uint32_t>> build_vamana_graph(
    const VectorSet& base, const DiskGraphBuildConfig& config) {
  VamanaBuildConfig options;
  options.max_degree = config.degree;
  options.search_width = std::max<std::size_t>(config.degree, 256);
  options.alpha = 1.2;
  options.seed = config.random_seed;
  options.add_reverse_edges = true;
  options.deleted_ids = config.deleted_ids;
  return VamanaBuilder(options).build(base).adjacency;
}

std::vector<std::uint32_t> bfs_order(
    const std::vector<std::vector<std::uint32_t>>& graph) {
  std::vector<std::uint32_t> order;
  std::vector<char> seen(graph.size(), 0);
  std::deque<std::uint32_t> queue;
  order.reserve(graph.size());

  for (std::size_t start = 0; start < graph.size(); ++start) {
    if (seen[start]) {
      continue;
    }
    seen[start] = 1;
    queue.push_back(static_cast<std::uint32_t>(start));
    while (!queue.empty()) {
      const std::uint32_t current = queue.front();
      queue.pop_front();
      order.push_back(current);
      for (const std::uint32_t neighbor : graph[current]) {
        if (neighbor < seen.size() && !seen[neighbor]) {
          seen[neighbor] = 1;
          queue.push_back(neighbor);
        }
      }
    }
  }
  return order;
}

void validate_build(const VectorSet& base,
                    const DiskGraphBuildConfig& config) {
  if (base.empty()) {
    throw std::runtime_error("Cannot build packed graph index for empty base vectors");
  }
  if (base.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("Packed graph index supports at most uint32_t ids");
  }
  if (config.degree == 0) {
    throw std::runtime_error("Graph degree must be positive");
  }
  if (config.page_size < kRecordsOffset) {
    throw std::runtime_error("Page size must be at least 4096 bytes");
  }
}

}  // namespace

void PackedDiskGraphBuilder::build(const VectorSet& base,
                                   const std::string& path,
                                   const DiskGraphBuildConfig& config) {
  validate_build(base, config);
  const std::size_t pq_code_bytes =
      config.pq_model != nullptr && config.pq_model->enabled()
          ? config.pq_model->subspaces()
          : 0;
  if (pq_code_bytes > 0 && config.pq_model->vector_count() < base.size()) {
    throw std::runtime_error("PQ model does not cover packed graph base vectors");
  }

  const std::size_t nodes_per_page = packed_nodes_per_page(
      base.dim, config.degree, config.page_size, pq_code_bytes);
  if (nodes_per_page == 0) {
    throw std::runtime_error("Packed page cannot hold even one node");
  }

  const auto graph = build_vamana_graph(base, config);
  const auto order = bfs_order(graph);
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
    node_to_page[order[position]] =
        static_cast<std::uint32_t>(position / nodes_per_page);
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
  write_value(output, static_cast<std::uint32_t>(pq_code_bytes));

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
    const auto page = DiskPageCodec::encode_packed_page(
        static_cast<std::uint32_t>(page_id), base, graph, order,
        nodes_per_page, config.page_size, config.degree, config.pq_model);
    output.write(page.data(), static_cast<std::streamsize>(page.size()));
    if (!output) {
      throw std::runtime_error("Failed while writing packed graph page");
    }
  }
}

}  // namespace agent_aware
