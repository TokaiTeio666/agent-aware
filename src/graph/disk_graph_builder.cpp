#include "graph/disk_graph_builder.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "graph/disk_page_codec.h"
#include "graph/vamana_builder.h"

namespace agent_aware {
namespace {

constexpr std::uint64_t kRecordsOffset = 4096;
constexpr std::uint64_t kSerializedPqHeaderBytes = 48;

VamanaGraph build_vamana_graph(
    const VectorSet& base, const DiskGraphBuildConfig& config) {
  VamanaBuildConfig options;
  options.max_degree = config.degree;
  options.search_width = std::max<std::size_t>(config.degree, 256);
  options.alpha = 1.2;
  options.seed = config.random_seed;
  options.add_reverse_edges = true;
  // 远点锚点（beacon）与撒点起点都关掉。它们当初是为了治"图断裂"加的，
  // 但实测 1M 的断裂并不来自连通性（从节点 0 可达 99.71%），而它们注入的
  // 均匀切片点（0, 31250, 62500, …）会被 robust_prune 当作有效近邻保留下来，
  // 把真实近邻挤出每个节点的出边名额（实测有个节点 0/32 条出边是近邻）。
  options.beacon_count = 0;
  options.start_count = 0;
  options.deleted_ids = config.deleted_ids;
  return VamanaBuilder(options).build(base);
}

// 入口点选中心性最高的节点，而不是几何 medoid。Vamana 的贪心搜索只能沿
// 出边前进，入口若是某个局部区域的点，搜索一开始就被锁在那里。medoid 在
// 大规模上尤其糟糕：它到全库都"不近不远"，出边改局部化以后就退化成普通
// 小簇的成员（实测 12 万点上从 medoid 出发只能到达 0.1%，从 PageRank 前
// 几名的点出发能到达 97%）。
//
// 这里用无向化、固定迭代次数的 PageRank（1M×32×20 轮在构建时间面前可忽略）。
std::uint32_t select_entry_node(
    const std::vector<std::vector<std::uint32_t>>& graph) {
  const std::size_t count = graph.size();
  if (count == 0) {
    return 0;
  }
  constexpr int kIterations = 20;
  constexpr double kDamping = 0.85;
  const double teleport = (1.0 - kDamping) / static_cast<double>(count);
  std::vector<double> rank(count, 1.0 / static_cast<double>(count));
  std::vector<double> next(count, 0.0);
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    std::fill(next.begin(), next.end(), teleport);
    for (std::size_t from = 0; from < count; ++from) {
      const auto& neighbors = graph[from];
      if (neighbors.empty()) {
        continue;
      }
      const double share =
          kDamping * rank[from] / static_cast<double>(neighbors.size());
      for (const auto to : neighbors) {
        if (to < count) {
          next[to] += share;
        }
      }
    }
    rank.swap(next);
  }
  // PageRank 不直接决定入口：在成规模的图上，一个抱团的小簇（成员彼此互指、
  // 出边全在簇内）也能拿到很高的评分，而它的成员只能到达本簇一百来个点。
  // 构建图里这类孤岛实测有上千个。因此先标记最大连通分量，再只在其中挑
  // PageRank 最高的节点 —— 入口必须在主分量的骨架里，而不是在任何孤岛上。
  std::vector<char> in_main_component(count, 0);
  {
    std::vector<std::size_t> component(count, 0);
    std::vector<std::size_t> component_sizes;
    std::vector<std::uint32_t> stack;
    for (std::size_t seed = 0; seed < count; ++seed) {
      if (component[seed] != 0) {
        continue;
      }
      const std::size_t label = component_sizes.size() + 1;
      component_sizes.push_back(0);
      stack.push_back(static_cast<std::uint32_t>(seed));
      component[seed] = label;
      while (!stack.empty()) {
        const auto current = stack.back();
        stack.pop_back();
        ++component_sizes.back();
        for (const auto neighbor : graph[current]) {
          if (neighbor < count && component[neighbor] == 0) {
            component[neighbor] = label;
            stack.push_back(neighbor);
          }
        }
      }
    }
    const std::size_t main_label =
        static_cast<std::size_t>(
            std::max_element(component_sizes.begin(), component_sizes.end()) -
            component_sizes.begin()) +
        1;
    for (std::size_t i = 0; i < count; ++i) {
      in_main_component[i] = component[i] == main_label ? 1 : 0;
    }
  }

  // 在主分量里按 PageRank 取前 k 名作为候选，逐个实测"从它出发能到达多少
  // 节点"（有向可达，才是贪心搜索真正的活动范围），取覆盖最大的那个。
  // PageRank 本身与有向可达规模并不一致：实测主分量内评分最高的点只能到达
  // 122 个节点，而分布中位数以上位置的点能到达 116,442 个。
  std::size_t candidate_count = 0;
  for (std::size_t i = 0; i < count; ++i) {
    candidate_count += in_main_component[i] ? 1 : 0;
  }
  std::vector<std::uint32_t> candidates(count);
  std::iota(candidates.begin(), candidates.end(), 0u);
  const std::size_t probe_count =
      std::min<std::size_t>({std::size_t{32}, candidate_count, count});
  if (probe_count == 0) {
    return 0;
  }
  std::partial_sort(
      candidates.begin(), candidates.begin() + probe_count, candidates.end(),
      [&](std::uint32_t lhs, std::uint32_t rhs) {
        if (in_main_component[lhs] != in_main_component[rhs]) {
          return in_main_component[lhs] != 0;
        }
        return rank[lhs] == rank[rhs] ? lhs < rhs : rank[lhs] > rank[rhs];
      });
  // PageRank 前 k 名可能全部落在同一个局部稠密簇里（实测 12 万点上前 32 名
  // 都只能到达 122 个节点，而主分量有 11.9 万个节点）。补一批均匀采样的候选，
  // 给"实测可达规模"一个不受 PageRank 影响的兜底。
  constexpr std::size_t kUniformProbes = 64;
  candidates.resize(probe_count);
  const std::size_t stride = std::max<std::size_t>(1, count / kUniformProbes);
  for (std::size_t i = 0; i < count && candidates.size() < probe_count + kUniformProbes;
       i += stride) {
    if (in_main_component[i] != 0) {
      candidates.push_back(static_cast<std::uint32_t>(i));
    }
  }

  std::uint32_t best = 0;
  std::size_t best_reachable = 0;
  std::vector<char> seen(count, 0);
  std::vector<std::uint32_t> queue;
  for (const auto start : candidates) {
    if (!in_main_component[start]) {
      continue;
    }
    std::fill(seen.begin(), seen.end(), 0);
    queue.clear();
    seen[start] = 1;
    queue.push_back(start);
    std::size_t reachable = 0;
    for (std::size_t head = 0; head < queue.size(); ++head) {
      ++reachable;
      for (const auto neighbor : graph[queue[head]]) {
        if (neighbor < count && !seen[neighbor]) {
          seen[neighbor] = 1;
          queue.push_back(neighbor);
        }
      }
    }
    if (reachable > best_reachable) {
      best_reachable = reachable;
      best = start;
    }
  }
  return best;
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

DiskGraphBuildResult PackedDiskGraphBuilder::build(
    const VectorSet& base, const std::string& path,
    const DiskGraphBuildConfig& config) {
  validate_build(base, config);
  const bool has_pq = config.pq_model != nullptr && config.pq_model->enabled();
  if (has_pq && config.pq_model->vector_count() != base.size()) {
    throw std::runtime_error("PQ model does not cover packed graph base vectors");
  }

  const std::size_t nodes_per_page = packed_nodes_per_page(
      base.dim, config.degree, config.page_size);
  if (nodes_per_page == 0) {
    // 单节点约需 dim*4 + degree*4 + 8 字节，再加整页的 id/degree SoA 区与页头。
    // 报错信息带上可执行的最小 page_size，避免使用者反复试 4096。
    std::ostringstream message;
    message << "Packed page cannot hold even one node (dim=" << base.dim
            << ", degree=" << config.degree
            << ", page_size=" << config.page_size
            << "); use page_size >= "
            << (8 + base.dim * sizeof(float) + config.degree * sizeof(std::uint32_t))
            << " or a smaller --graph-degree";
    throw std::runtime_error(message.str());
  }

  const auto built_graph = build_vamana_graph(base, config);
  const auto& graph = built_graph.adjacency;
  const auto order = bfs_order(graph);
  const std::uint64_t page_count =
      (static_cast<std::uint64_t>(base.size()) + nodes_per_page - 1) /
      nodes_per_page;
  if (page_count > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("Too many packed pages for V3 directory");
  }

  const std::uint64_t directory_offset = kRecordsOffset;
  const std::uint64_t directory_bytes =
      static_cast<std::uint64_t>(base.size()) * sizeof(std::uint32_t);
  const std::uint64_t records_offset =
      align_up(directory_offset + directory_bytes, config.page_size);
  const std::uint64_t records_bytes = page_count * config.page_size;
  const std::uint64_t pq_offset = records_offset + records_bytes;
  const std::uint64_t pq_offsets_bytes =
      has_pq ? (config.pq_model->offsets().size() * sizeof(std::uint64_t)) : 0;
  const std::uint64_t pq_codebook_bytes =
      has_pq ? config.pq_model->codebook_bytes() : 0;
  const std::uint64_t pq_codes_bytes =
      has_pq ? config.pq_model->code_bytes() : 0;
  const std::uint64_t pq_bytes =
      has_pq ? kSerializedPqHeaderBytes + pq_offsets_bytes +
                   pq_codebook_bytes + pq_codes_bytes
             : 0;
  const std::uint64_t pq_offsets_offset =
      has_pq ? pq_offset + kSerializedPqHeaderBytes : 0;
  const std::uint64_t pq_codebook_offset =
      has_pq ? pq_offsets_offset + pq_offsets_bytes : 0;
  const std::uint64_t pq_codes_offset =
      has_pq ? pq_codebook_offset + pq_codebook_bytes : 0;

  std::vector<std::uint32_t> node_to_page(base.size(), 0);
  for (std::size_t position = 0; position < order.size(); ++position) {
    node_to_page[order[position]] =
        static_cast<std::uint32_t>(position / nodes_per_page);
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create packed graph index: " + path);
  }

  output.write(kPackedGraphMagic.data(), kPackedGraphMagic.size());
  write_value(output, kPackedGraphVersion);
  write_value(output, static_cast<std::uint64_t>(base.size()));
  write_value(output, static_cast<std::uint32_t>(base.dim));
  write_value(output, static_cast<std::uint32_t>(config.degree));
  write_value(output, static_cast<std::uint32_t>(config.page_size));
  write_value(output, records_offset);
  write_value(output, directory_offset);
  write_value(output, page_count);
  write_value(output, static_cast<std::uint32_t>(nodes_per_page));
  write_value(output, static_cast<std::uint32_t>(has_pq ? config.pq_model->subspaces() : 0));
  write_value(output, static_cast<std::uint32_t>(has_pq ? config.pq_model->centroids() : 0));
  write_value(output, static_cast<std::uint32_t>(has_pq ? config.pq_model->dim() : 0));
  write_value(output, has_pq ? pq_offset : std::uint64_t{0});
  write_value(output, pq_bytes);
  write_value(output, pq_offsets_offset);
  write_value(output, pq_offsets_bytes);
  write_value(output, pq_codebook_offset);
  write_value(output, pq_codebook_bytes);
  write_value(output, pq_codes_offset);
  write_value(output, pq_codes_bytes);

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
        nodes_per_page, config.page_size, config.degree);
    output.write(page.data(), static_cast<std::streamsize>(page.size()));
    if (!output) {
      throw std::runtime_error("Failed while writing packed graph page");
    }
  }
  if (has_pq) {
    config.pq_model->write(output);
  }
  if (!output) {
    throw std::runtime_error("Failed while finalizing packed graph index");
  }
  return DiskGraphBuildResult{select_entry_node(graph)};
}

}  // namespace agent_aware
