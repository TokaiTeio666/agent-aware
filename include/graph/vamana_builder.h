#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/types.h"

namespace agent_aware {

struct VamanaBuildConfig {
  std::size_t max_degree = 32;
  std::size_t search_width = 200;
  double alpha = 1.2;
  std::uint32_t seed = 42;
  bool add_reverse_edges = true;
  // 每个节点的远点采样（beacon）数量与构造搜索的撒点起点数量。
  // 默认关闭：作为 robust_prune 的候选时，远点会挤掉真实近邻的出边名额，
  // 实测在 1M 上让节点的出边几乎不含近邻（见 disk_graph_builder.cpp）。
  // 非 0 值仅用于对照实验；两份构建路径都会关闭它。
  std::size_t beacon_count = 0;
  std::size_t start_count = 0;
  std::vector<std::uint32_t> deleted_ids;
};

struct VamanaBuildStats {
  std::uint32_t medoid = 0;
  std::size_t reverse_edges_added = 0;
  std::size_t reverse_prune_count = 0;
  std::size_t prune_count = 0;
  double avg_degree = 0.0;
  std::size_t max_degree = 0;
  double candidate_avg = 0.0;
  double candidate_p95 = 0.0;
  double candidate_p99 = 0.0;
  std::vector<std::size_t> candidate_counts;
};

struct VamanaGraph {
  std::uint32_t medoid = 0;
  std::vector<std::vector<std::uint32_t>> adjacency;
  VamanaBuildStats stats;
};

class VamanaBuilder {
 public:
  explicit VamanaBuilder(VamanaBuildConfig config = {});

  VamanaGraph build(const VectorSet& base) const;

  std::uint32_t find_medoid(const VectorSet& base) const;

  std::vector<std::uint32_t> search_for_construction(
      const VectorSet& base,
      const std::vector<std::vector<std::uint32_t>>& graph,
      const float* query, std::uint32_t entry_id,
      std::uint32_t excluded_id = UINT32_MAX) const;

  std::vector<std::uint32_t> robust_prune(
      const VectorSet& base, std::uint32_t node_id,
      const std::vector<std::uint32_t>& candidates) const;

  void insert(const VectorSet& base,
              std::vector<std::vector<std::uint32_t>>& graph,
              std::uint32_t node_id, std::uint32_t entry_id,
              VamanaBuildStats* stats = nullptr) const;

 private:
  std::vector<std::uint32_t> start_candidates(
      const VectorSet& base, std::uint32_t excluded_id,
      const std::vector<char>& deleted) const;
  std::vector<std::uint32_t> beacon_candidates(
      const VectorSet& base, std::uint32_t node_id,
      const std::vector<char>& deleted) const;
  std::uint32_t select_entry_node(
      const std::vector<std::vector<std::uint32_t>>& graph,
      const std::vector<char>& deleted) const;
  void refine_node(const VectorSet& base,
                   std::vector<std::vector<std::uint32_t>>& graph,
                   std::uint32_t node_id,
                   const std::vector<char>& deleted) const;
  std::vector<char> deleted_mask(std::size_t count) const;
  std::vector<std::vector<std::uint32_t>> initial_graph(
      const VectorSet& base, const std::vector<char>& deleted) const;
  std::vector<std::uint32_t> build_order(std::size_t count) const;
  void add_reverse_edge(const VectorSet& base,
                        std::vector<std::vector<std::uint32_t>>& graph,
                        std::uint32_t source, std::uint32_t target,
                        VamanaBuildStats* stats) const;
  void finalize_stats(const std::vector<std::vector<std::uint32_t>>& graph,
                      VamanaBuildStats& stats) const;

  VamanaBuildConfig config_;
  // 当前生效的剪枝 alpha：第一遍从 1.0 线性升到 config_.alpha，第二遍用最终值。
  mutable double active_alpha_ = 1.0;
  // build() 开头算一次 medoid，之后每个节点复用。find_medoid 是 O(n·dim)，
  // 若在 insert/beacon 里逐节点重算，大 base 的构建时间会变成 O(n^2·dim)。
  mutable std::uint32_t medoid_cache_ = 0;
};

}  // namespace agent_aware
