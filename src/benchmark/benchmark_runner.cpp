#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agentmem/benchmark/benchmark_runner.h"
#include "agentmem/benchmark/cli_args.h"
#include "agentmem/benchmark/metrics.h"
#include "agentmem/core/brute_force.h"
#include "agentmem/data/dataset.h"
#include "agentmem/graph/disk_graph_index.h"
#include "agentmem/storage/lsm_tree.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using agentmem::benchmark::Args;

struct MemoryReport {
  std::size_t raw_vector_bytes = 0;
  std::size_t budget_bytes = 0;
  std::size_t resident_bytes = 0;
  std::size_t bytes_raw_vectors_resident = 0;
  std::size_t bytes_pq_codes = 0;
  std::size_t bytes_pq_codebooks = 0;
  std::size_t bytes_graph_metadata = 0;
  std::size_t bytes_cache = 0;
  std::size_t bytes_path_cache = 0;
  std::size_t bytes_router = 0;
  std::size_t bytes_query_workspace = 0;
  std::size_t bytes_io_buffers = 0;
  std::size_t bytes_delta = 0;
  std::size_t bytes_tombstone = 0;
  std::size_t bytes_temporary_peak = 0;
  double budget_ratio = 0.20;
  double resident_ratio = 0.0;
  bool budget_pass = true;
  bool memory_mode = false;
  bool over_budget_allowed = false;
};

struct RunOutput {  // 保留逐 query 原始样本，最终统一计算分位数和吞吐。
  std::vector<std::vector<agentmem::SearchResult>> results;
  std::vector<double> latencies_ms;
  std::vector<double> ssd_reads;
  std::vector<double> cache_requests;
  std::vector<double> cache_hits;
  std::vector<double> cache_misses;
  std::vector<double> cache_evictions;
  std::vector<double> cache_promotions;
  std::vector<double> path_cache_requests;
  std::vector<double> path_cache_hits;
  std::vector<double> expanded;
  std::vector<double> visited;
  std::vector<double> io_submits;
  std::vector<double> io_completions;
  std::vector<double> io_submit_syscalls;
  std::vector<double> io_prefetches;
  std::vector<double> io_prefetch_hits;
  std::vector<double> io_prefetch_waits;
  std::vector<double> io_pending_pages_peak;
  std::vector<double> prefetch_submitted_pages;
  std::vector<double> prefetch_useful_pages;
  std::vector<double> prefetch_wasted_pages;
  std::vector<double> demand_read_waits;
  std::vector<double> demand_read_wait_us;
  std::vector<double> page_dedup_ratios;
  std::vector<double> page_requests_before_dedup;
  std::vector<double> page_requests_after_dedup;
  std::vector<double> same_page_node_reuse;
  std::vector<double> adc_table_build_us;
  std::vector<double> rerank_reads;
  std::vector<double> pq_filter_reject_count;
  std::vector<double> pq_filter_accept_count;
  std::vector<double> insert_latencies_ms;
  std::vector<double> query_compaction_ms;
  std::vector<double> delta_search_ms;
  std::vector<double> delta_exact_search_ms;
  std::vector<double> delta_recalls;
  std::vector<std::vector<std::uint32_t>> dynamic_truth;
  std::size_t operation_count = 0;
  std::size_t insert_count = 0;
  std::size_t delete_count = 0;
  std::size_t tombstone_count = 0;
  std::size_t wal_replay_records = 0;
  std::size_t wal_replay_inserts = 0;
  std::size_t wal_replay_deletes = 0;
  std::size_t wal_replay_bytes = 0;
  std::size_t wal_replay_delta_size = 0;
  std::size_t compaction_ops = 0;
  std::size_t compaction_vectors = 0;
  std::size_t compaction_skipped_sla = 0;
  std::size_t queries_with_compaction = 0;
  std::size_t compaction_io_bytes = 0;
  std::size_t stream_merge_ops = 0;
  std::size_t stream_merge_vectors = 0;
  std::size_t stream_merge_deleted = 0;
  std::size_t stream_merge_inserted = 0;
  std::size_t delta_active_size = 0;
  std::size_t delta_sealed_size = 0;
  std::size_t wal_records = 0;
  std::size_t wal_bytes = 0;
  double compaction_seconds = 0.0;
  double stream_merge_seconds = 0.0;
  double elapsed_seconds = 0.0;
  double pq_train_seconds = 0.0;
  std::size_t hotpath_train_queries = 0;
  std::size_t hotpath_unique_visited_nodes = 0;
  std::size_t hotpath_top_node_visit_count = 0;
  MemoryReport memory;
};

std::size_t checked_add(std::size_t lhs, std::size_t rhs,
                        const std::string& label) {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    throw std::runtime_error("Memory estimate overflow while adding " + label);
  }
  return lhs + rhs;
}

std::size_t checked_mul(std::size_t lhs, std::size_t rhs,
                        const std::string& label) {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    throw std::runtime_error("Memory estimate overflow while multiplying " +
                             label);
  }
  return lhs * rhs;
}

void add_memory_component(std::size_t& total, std::size_t bytes,
                          const std::string& label) {
  total = checked_add(total, bytes, label);
}

std::size_t ratio_budget_bytes(std::size_t raw_vector_bytes,
                               double memory_budget_ratio) {
  if (raw_vector_bytes == 0) {
    return 0;
  }
  const double budget =
      std::floor(static_cast<double>(raw_vector_bytes) * memory_budget_ratio);
  return static_cast<std::size_t>(std::max(1.0, budget));
}

std::size_t memory_budget_bytes(const Args& args,
                                std::size_t raw_vector_bytes) {
  const std::size_t ratio_budget =
      ratio_budget_bytes(raw_vector_bytes, args.memory_budget_ratio);
  if (args.memory_budget_bytes == 0) {
    return ratio_budget;
  }
  return std::min(args.memory_budget_bytes, ratio_budget);
}

std::vector<std::vector<std::uint32_t>> results_as_truth(
    const std::vector<std::vector<agentmem::SearchResult>>& results) {
  std::vector<std::vector<std::uint32_t>> truth;
  truth.reserve(results.size());
  for (const auto& row : results) {
    std::vector<std::uint32_t> ids;
    ids.reserve(row.size());
    for (const auto& item : row) {
      ids.push_back(item.id);
    }
    truth.push_back(std::move(ids));
  }
  return truth;
}

std::vector<std::size_t> agent_workload_order(const Args& args,
                                              std::size_t query_count) {
  std::vector<std::size_t> order(query_count);
  for (std::size_t i = 0; i < query_count; ++i) {
    order[i] = i;
  }
  if (query_count == 0 || args.agent_workload_mode == "random") {
    return order;
  }

  const std::size_t hot_count = std::min<std::size_t>(32, query_count);
  const std::size_t session_length =
      std::max<std::size_t>(1, args.synthetic_config.session_length);
  for (std::size_t i = 0; i < query_count; ++i) {
    if (args.agent_workload_mode == "hotspot") {
      order[i] = i % hot_count;
    } else if (args.agent_workload_mode == "recent-memory") {
      order[i] = query_count - hot_count + (i % hot_count);
    } else {
      order[i] = (i / session_length) % query_count;
    }
  }
  return order;
}

void apply_agent_workload(const Args& args, agentmem::VectorSet& queries,
                          std::vector<std::vector<std::uint32_t>>& truth,
                          bool truth_loaded) {
  if (args.agent_workload_mode == "random" || queries.empty()) {
    return;
  }
  const auto order = agent_workload_order(args, queries.size());
  agentmem::VectorSet reordered(queries.size(), queries.dim);
  for (std::size_t i = 0; i < order.size(); ++i) {
    const float* source = queries.row(order[i]);
    std::copy(source, source + queries.dim, reordered.mutable_row(i));
  }
  queries = std::move(reordered);

  if (truth_loaded && truth.size() == order.size()) {
    std::vector<std::vector<std::uint32_t>> reordered_truth;
    reordered_truth.reserve(order.size());
    for (const auto source : order) {
      reordered_truth.push_back(truth[source]);
    }
    truth = std::move(reordered_truth);
  }
}

std::vector<std::vector<std::uint32_t>> exact_truth(
    const agentmem::VectorSet& base, const agentmem::VectorSet& queries,
    std::size_t k) {
  // 子集实验中官方 SIFT1M truth 不适用时，用当前 base 重算精确 truth。
  return results_as_truth(agentmem::search_memory_fast_batch(base, queries, k));
}

RunOutput run_memory_search(const agentmem::VectorSet& base,
                            const agentmem::VectorSet& queries,
                            std::size_t k) {
  // search_memory_fast 是全精度内存对照路径，用于正确性上界和性能上限。
  RunOutput output;
  output.results.reserve(queries.size());
  output.latencies_ms.reserve(queries.size());

  const auto batch_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < queries.size(); ++i) {
    const auto query_start = std::chrono::steady_clock::now();
    output.results.push_back(
        agentmem::search_memory_fast(base, queries.row(i), k));
    const auto query_end = std::chrono::steady_clock::now();
    output.latencies_ms.push_back(
        std::chrono::duration<double, std::milli>(query_end - query_start)
            .count());
  }
  const auto batch_end = std::chrono::steady_clock::now();
  output.elapsed_seconds =
      std::chrono::duration<double>(batch_end - batch_start).count();
  return output;
}

struct RoutedEntries {
  std::uint32_t signature = 0;        // Query Path Cache 的查找键。
  std::vector<std::uint32_t> seeds;  // 图遍历优先使用的入口节点。
};

std::vector<std::uint32_t> merge_ids(const std::vector<std::uint32_t>& lhs,
                                     const std::vector<std::uint32_t>& rhs,
                                     std::size_t limit) {
  std::vector<std::uint32_t> merged;
  std::unordered_set<std::uint32_t> seen;
  merged.reserve(std::min(limit, lhs.size() + rhs.size()));
  auto append = [&](const std::vector<std::uint32_t>& ids) {
    for (const auto id : ids) {
      if (merged.size() >= limit) {
        return;
      }
      if (seen.insert(id).second) {
        merged.push_back(id);
      }
    }
  };
  append(lhs);
  append(rhs);
  return merged;
}

std::vector<std::uint32_t> topk_ids(
    const std::vector<agentmem::SearchResult>& results) {
  std::vector<std::uint32_t> ids;
  ids.reserve(results.size());
  for (const auto& result : results) {
    ids.push_back(result.id);
  }
  return ids;
}

struct PathCacheEntry {
  std::uint32_t signature = 0;
  std::uint64_t last_access = 0;
  std::uint64_t frequency = 0;
  std::vector<std::uint32_t> seeds;    // 上一次路由入口。
  std::vector<std::uint32_t> top_ids;  // 上一次 Top-K，可作为更强的热启动入口。
};

// 复用相似 query 的历史图路径入口；容量满时按频率和最近访问时间淘汰。
class QueryPathCache {
 public:
  QueryPathCache(std::string policy, std::size_t capacity)
      : policy_(std::move(policy)), capacity_(capacity) {}

  bool enabled() const {
    return policy_ != "none" && capacity_ > 0;
  }

  std::size_t estimated_capacity_bytes(std::size_t entry_count,
                                       std::size_t top_k) const {
    if (!enabled()) {
      return 0;
    }
    std::size_t bytes = checked_mul(capacity_, sizeof(PathCacheEntry),
                                    "path cache entries");
    const std::size_t ids_per_entry = checked_add(entry_count, top_k,
                                                  "path cache ids/entry");
    const std::size_t id_bytes =
        checked_mul(ids_per_entry, sizeof(std::uint32_t), "path cache ids");
    add_memory_component(bytes, checked_mul(capacity_, id_bytes,
                                            "path cache capacity ids"),
                         "path cache ids");
    return bytes;
  }

  const PathCacheEntry* lookup(std::uint32_t signature) {
    if (!enabled()) {
      return nullptr;
    }
    auto found = entries_.find(signature);
    if (found == entries_.end()) {
      return nullptr;
    }
    found->second.last_access = ++clock_;
    ++found->second.frequency;
    return &found->second;
  }

  void update(std::uint32_t signature, const std::vector<std::uint32_t>& seeds,
              const std::vector<agentmem::SearchResult>& results) {
    if (!enabled()) {
      return;
    }
    auto found = entries_.find(signature);
    if (found == entries_.end()) {
      if (entries_.size() >= capacity_) {
        evict_one();
      }
      PathCacheEntry entry;
      entry.signature = signature;
      entry.last_access = ++clock_;
      entry.frequency = 1;
      entry.seeds = seeds;
      entry.top_ids = topk_ids(results);
      entries_.emplace(signature, std::move(entry));
      return;
    }

    found->second.last_access = ++clock_;
    ++found->second.frequency;
    found->second.seeds = seeds;
    found->second.top_ids = topk_ids(results);
  }

 private:
  void evict_one() {
    auto victim = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->second.frequency < victim->second.frequency ||
          (it->second.frequency == victim->second.frequency &&
           it->second.last_access < victim->second.last_access)) {
        victim = it;
      }
    }
    entries_.erase(victim);
  }

  std::string policy_;
  std::size_t capacity_ = 0;
  std::uint64_t clock_ = 0;
  std::unordered_map<std::uint32_t, PathCacheEntry> entries_;
};

std::uint32_t mix_u32(std::uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

std::uint32_t rotate_left_u32(std::uint32_t value, std::size_t shift) {
  const std::size_t bits = shift & 31u;
  if (bits == 0) {
    return value;
  }
  return static_cast<std::uint32_t>((value << bits) | (value >> (32 - bits)));
}

bool signature_uses_simhash(const std::string& policy) {
  return policy == "simhash" || policy == "simhash-pq";
}

bool signature_uses_pq_prefix(const std::string& policy) {
  return policy == "pq-prefix" || policy == "simhash-pq";
}

// 在内存采样上做轻量路由，并为路径缓存生成 routed/SimHash/PQ 签名。
class ResidentQueryRouter {
 public:
  ResidentQueryRouter(const Args& args, const agentmem::VectorSet& base)
      : dim_(base.dim),
        policy_(args.query_signature_policy),
        simhash_bits_(args.simhash_bits),
        pq_prefix_subspaces_(args.pq_prefix_subspaces),
        pq_prefix_centroids_(args.pq_prefix_centroids),
        pq_prefix_train_iterations_(args.pq_prefix_train_iterations),
        seed_(args.synthetic_config.seed) {
    build_sample_ids(args.routing_sample_count, base);
    copy_sample_vectors(base);
    if (signature_uses_simhash(policy_)) {
      build_simhash_planes();
    }
    if (signature_uses_pq_prefix(policy_)) {
      train_pq_prefix();
    }
  }

  RoutedEntries route(const float* query, std::size_t entry_count) const {
    RoutedEntries routed;
    const auto scored = score_samples(query);  // 只扫描常驻样本，不访问磁盘页。
    if (!scored.empty()) {
      routed.signature = scored.front().id;
    }

    const std::size_t actual = std::min(entry_count, scored.size());
    routed.seeds.reserve(actual);
    for (std::size_t i = 0; i < actual; ++i) {
      routed.seeds.push_back(scored[i].id);
    }

    routed.signature = signature_for(query, routed.signature);
    return routed;
  }

  std::size_t estimated_bytes() const {
    std::size_t bytes = sizeof(*this);
    add_memory_component(
        bytes,
        checked_mul(sample_ids_.capacity(), sizeof(std::uint32_t),
                    "router sample ids"),
        "router sample ids");
    add_memory_component(
        bytes,
        checked_mul(sample_vectors_.capacity(), sizeof(float),
                    "router sampled vector bytes"),
        "router sampled vectors");
    add_memory_component(
        bytes,
        checked_mul(simhash_planes_.capacity(), sizeof(float),
                    "router simhash planes"),
        "router simhash planes");
    add_memory_component(
        bytes,
        checked_mul(pq_subspaces_.capacity(),
                    sizeof(std::size_t) * 2 + sizeof(std::vector<float>),
                    "router pq subspaces"),
        "router pq subspaces");
    for (const auto& subspace : pq_subspaces_) {
      add_memory_component(
          bytes,
          checked_mul(subspace.centroids.capacity(), sizeof(float),
                      "router pq centroids"),
          "router pq centroids");
    }
    return bytes;
  }

 private:
  struct PqSubspace {
    std::size_t start = 0;
    std::size_t length = 0;
    std::vector<float> centroids;
  };

  void build_sample_ids(std::size_t routing_sample_count,
                        const agentmem::VectorSet& base) {
    if (routing_sample_count == 0 || base.empty()) {
      return;
    }
    const std::size_t sample_count =
        std::min(routing_sample_count, base.size());
    sample_ids_.reserve(sample_count);
    for (std::size_t i = 0; i < sample_count; ++i) {
      const std::size_t id =  // 均匀采样保证覆盖整个 base id 空间。
          sample_count == 1 ? 0 : (i * (base.size() - 1)) / (sample_count - 1);
      if (!sample_ids_.empty() && sample_ids_.back() == id) {
        continue;
      }
      sample_ids_.push_back(static_cast<std::uint32_t>(id));
    }
  }

  void copy_sample_vectors(const agentmem::VectorSet& base) {
    if (sample_ids_.empty() || dim_ == 0) {
      return;
    }
    sample_vectors_.reserve(
        checked_mul(sample_ids_.size(), dim_, "router sampled vectors"));
    for (const auto id : sample_ids_) {
      const float* row = base.row(id);
      sample_vectors_.insert(sample_vectors_.end(), row, row + dim_);
    }
  }

  const float* sample_row(std::size_t sample_index) const {
    return sample_vectors_.data() + sample_index * dim_;
  }

  std::vector<agentmem::SearchResult> score_samples(const float* query) const {
    std::vector<agentmem::SearchResult> scored;
    scored.reserve(sample_ids_.size());
    for (std::size_t i = 0; i < sample_ids_.size(); ++i) {
      const auto id = sample_ids_[i];
      scored.push_back(agentmem::SearchResult{
          id, agentmem::squared_l2(query, sample_row(i), dim_)});
    }

    std::sort(scored.begin(), scored.end(),
              [](const agentmem::SearchResult& lhs,
                 const agentmem::SearchResult& rhs) {
                if (lhs.distance == rhs.distance) {
                  return lhs.id < rhs.id;
                }
                return lhs.distance < rhs.distance;
              });
    return scored;
  }

  void build_simhash_planes() {
    if (dim_ == 0) {
      return;
    }
    std::mt19937 rng(seed_ ^ 0x9e3779b9u);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    simhash_planes_.resize(simhash_bits_ * dim_);
    for (float& value : simhash_planes_) {
      value = dist(rng);
    }
  }

  std::uint32_t simhash_signature(const float* query) const {
    if (simhash_planes_.empty()) {
      return 0;
    }
    std::uint32_t signature = 0;
    for (std::size_t bit = 0; bit < simhash_bits_; ++bit) {
      const float* plane = simhash_planes_.data() + bit * dim_;
      float dot = 0.0f;
      for (std::size_t d = 0; d < dim_; ++d) {
        dot += query[d] * plane[d];
      }
      if (dot >= 0.0f) {
        signature |= (1u << bit);
      }
    }
    return signature;
  }

  void train_pq_prefix() {
    if (sample_ids_.empty() || dim_ == 0) {
      return;
    }

    const std::size_t subspaces =
        std::min(pq_prefix_subspaces_, dim_);
    const std::size_t centroids =
        std::min(pq_prefix_centroids_, sample_ids_.size());
    if (subspaces == 0 || centroids < 2) {
      return;
    }

    pq_subspaces_.reserve(subspaces);  // 小型 PQ 仅用于签名，不参与最终距离排序。
    for (std::size_t m = 0; m < subspaces; ++m) {
      PqSubspace subspace;
      subspace.start = (dim_ * m) / subspaces;
      const std::size_t end = (dim_ * (m + 1)) / subspaces;
      subspace.length = end - subspace.start;
      subspace.centroids.assign(centroids * subspace.length, 0.0f);

      for (std::size_t c = 0; c < centroids; ++c) {
        const std::size_t sample_index =
            centroids == 1 ? 0 : (c * (sample_ids_.size() - 1)) / (centroids - 1);
        const float* row = sample_row(sample_index) + subspace.start;
        std::copy(row, row + subspace.length,
                  subspace.centroids.begin() +
                      static_cast<std::ptrdiff_t>(c * subspace.length));
      }

      refine_pq_subspace(subspace, centroids);
      pq_subspaces_.push_back(std::move(subspace));
    }
  }

  void refine_pq_subspace(PqSubspace& subspace, std::size_t centroids) const {
    std::vector<float> sums(centroids * subspace.length, 0.0f);
    std::vector<std::size_t> counts(centroids, 0);

    for (std::size_t iteration = 0; iteration < pq_prefix_train_iterations_;
         ++iteration) {
      std::fill(sums.begin(), sums.end(), 0.0f);
      std::fill(counts.begin(), counts.end(), 0);

      for (std::size_t i = 0; i < sample_ids_.size(); ++i) {
        const float* row = sample_row(i) + subspace.start;
        const std::size_t best = nearest_pq_centroid(row, subspace, centroids);
        ++counts[best];
        float* sum = sums.data() + best * subspace.length;
        for (std::size_t d = 0; d < subspace.length; ++d) {
          sum[d] += row[d];
        }
      }

      for (std::size_t c = 0; c < centroids; ++c) {
        float* centroid = subspace.centroids.data() + c * subspace.length;
        if (counts[c] == 0) {
          const std::size_t sample_index =
              (c + iteration) % sample_ids_.size();
          const float* row = sample_row(sample_index) + subspace.start;
          std::copy(row, row + subspace.length, centroid);
          continue;
        }
        const float scale = 1.0f / static_cast<float>(counts[c]);
        const float* sum = sums.data() + c * subspace.length;
        for (std::size_t d = 0; d < subspace.length; ++d) {
          centroid[d] = sum[d] * scale;
        }
      }
    }
  }

  std::size_t nearest_pq_centroid(const float* vector,
                                  const PqSubspace& subspace,
                                  std::size_t centroids) const {
    std::size_t best = 0;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t c = 0; c < centroids; ++c) {
      const float* centroid = subspace.centroids.data() + c * subspace.length;
      float distance = 0.0f;
      for (std::size_t d = 0; d < subspace.length; ++d) {
        const float diff = vector[d] - centroid[d];
        distance += diff * diff;
      }
      if (distance < best_distance) {
        best_distance = distance;
        best = c;
      }
    }
    return best;
  }

  std::uint32_t pq_prefix_signature(const float* query) const {
    if (pq_subspaces_.empty()) {
      return 0;
    }
    std::uint32_t signature = 2166136261u;
    for (std::size_t m = 0; m < pq_subspaces_.size(); ++m) {
      const auto& subspace = pq_subspaces_[m];
      const std::size_t centroids =
          subspace.length == 0 ? 0 : subspace.centroids.size() / subspace.length;
      if (centroids == 0) {
        continue;
      }
      const std::size_t code = nearest_pq_centroid(
          query + subspace.start, subspace, centroids);
      signature ^= static_cast<std::uint32_t>(code + 31 * (m + 1));
      signature *= 16777619u;
    }
    return signature;
  }

  std::uint32_t signature_for(const float* query,
                              std::uint32_t routed_signature) const {
    if (policy_ == "routed") {
      return routed_signature;
    }
    if (policy_ == "simhash") {
      const std::uint32_t simhash = simhash_signature(query);
      return simhash_planes_.empty() ? routed_signature
                                     : mix_u32(simhash ^ 0x51f15eedu);
    }
    if (policy_ == "pq-prefix") {
      const std::uint32_t pq = pq_prefix_signature(query);
      return pq_subspaces_.empty() ? routed_signature
                                   : mix_u32(pq ^ 0x7057c0deu);
    }

    const std::uint32_t simhash = simhash_signature(query);
    const std::uint32_t pq = pq_prefix_signature(query);
    if (simhash_planes_.empty() && pq_subspaces_.empty()) {
      return routed_signature;
    }
    if (simhash_planes_.empty()) {
      return mix_u32(pq ^ 0x7057c0deu);
    }
    if (pq_subspaces_.empty()) {
      return mix_u32(simhash ^ 0x51f15eedu);
    }
    return mix_u32(simhash ^ rotate_left_u32(pq, simhash_bits_ % 31 + 1) ^
                   0x5a17f00du);
  }

  std::size_t dim_ = 0;
  std::string policy_;
  std::size_t simhash_bits_ = 16;
  std::size_t pq_prefix_subspaces_ = 4;
  std::size_t pq_prefix_centroids_ = 16;
  std::size_t pq_prefix_train_iterations_ = 4;
  std::uint32_t seed_ = 42;
  std::vector<std::uint32_t> sample_ids_;
  std::vector<float> sample_vectors_;
  std::vector<float> simhash_planes_;
  std::vector<PqSubspace> pq_subspaces_;
};

std::size_t estimate_temporary_peak_bytes(const Args& args, std::size_t dim) {
  const std::size_t expansions =
      std::max({args.search_width, args.max_expansions,
                args.search_early_stop_min_expansions, args.min_expansions});
  std::size_t candidates = checked_add(expansions, args.entry_count,
                                       "temporary candidates");
  candidates = checked_add(candidates, args.k, "temporary top-k");
  candidates = checked_add(candidates, args.rerank_topk, "temporary rerank");

  std::size_t bytes = checked_mul(candidates, sizeof(agentmem::SearchResult),
                                  "temporary search results");
  add_memory_component(
      bytes,
      checked_mul(candidates, sizeof(std::uint32_t), "temporary visited ids"),
      "temporary visited ids");
  add_memory_component(bytes, checked_mul(dim, sizeof(float), "query vector"),
                       "query vector");
  if (args.adc_enable && args.pq_enable) {
    add_memory_component(
        bytes,
        checked_mul(checked_mul(args.pq_m, args.pq_ks, "ADC table entries"),
                    sizeof(float), "ADC table bytes"),
        "ADC table");
  }
  return bytes;
}

std::size_t estimate_graph_metadata_bytes(const Args& args,
                                          const agentmem::DiskGraphMetadata& md) {
  std::size_t bytes = sizeof(md);
  if (args.layout == "packed") {
    add_memory_component(
        bytes,
        checked_mul(static_cast<std::size_t>(md.vector_count),
                    sizeof(std::uint32_t), "packed node-to-page directory"),
        "packed node-to-page directory");
  }
  return bytes;
}

std::size_t estimate_decoded_node_bytes(
    const agentmem::DiskGraphMetadata& md) {
  std::size_t bytes = sizeof(std::uint32_t);
  add_memory_component(
      bytes,
      checked_mul(static_cast<std::size_t>(md.dim), sizeof(float),
                  "decoded vector bytes"),
      "decoded vector bytes");
  add_memory_component(
      bytes,
      checked_mul(static_cast<std::size_t>(md.degree), sizeof(std::uint32_t),
                  "decoded neighbor bytes"),
      "decoded neighbor bytes");
  add_memory_component(bytes, sizeof(std::vector<float>),
                       "decoded vector handle");
  add_memory_component(bytes, sizeof(std::vector<std::uint32_t>),
                       "decoded neighbor handle");
  add_memory_component(bytes, 64, "decoded node container overhead");
  return bytes;
}

std::size_t estimate_decoded_page_bytes(
    const agentmem::DiskGraphMetadata& md) {
  std::size_t bytes = sizeof(std::uint32_t);
  add_memory_component(bytes, sizeof(std::vector<std::uint32_t>),
                       "decoded page node vector handle");
  add_memory_component(
      bytes,
      checked_mul(static_cast<std::size_t>(md.nodes_per_page),
                  estimate_decoded_node_bytes(md), "decoded page nodes"),
      "decoded page nodes");
  return bytes;
}

std::size_t effective_cache_capacity_pages(
    const Args& args, const agentmem::DiskGraphMetadata& md) {
  if (args.layout != "packed" || args.cache_policy == "none") {
    return 0;
  }
  std::size_t pages = args.cache_pages;
  if (args.cache_budget_bytes > 0 && md.page_size > 0) {
    const std::size_t budget_pages =
        std::max<std::size_t>(1, args.cache_budget_bytes / md.page_size);
    pages = pages == 0 ? budget_pages : std::min(pages, budget_pages);
  }
  return pages;
}

std::size_t estimate_cache_bytes(const Args& args,
                                 const agentmem::DiskGraphMetadata& md) {
  const std::size_t capacity_pages = effective_cache_capacity_pages(args, md);
  if (capacity_pages == 0) {
    return 0;
  }
  return checked_mul(capacity_pages, estimate_decoded_page_bytes(md),
                     "decoded page cache bytes");
}

std::size_t estimate_graph_workspace_bytes(
    const Args& args, const agentmem::DiskGraphMetadata& md) {
  const std::size_t expansion_budget =
      args.adaptive_early_stop
          ? std::min(args.search_width, args.max_expansions)
          : args.search_width;
  std::size_t candidate_nodes = checked_add(
      args.entry_count,
      checked_mul(expansion_budget, static_cast<std::size_t>(md.degree),
                  "graph frontier nodes"),
      "graph candidate nodes");
  candidate_nodes = std::min<std::size_t>(
      candidate_nodes, static_cast<std::size_t>(md.vector_count));

  std::size_t bytes = 0;
  add_memory_component(
      bytes,
      checked_mul(candidate_nodes, sizeof(std::uint32_t),
                  "visited set ids"),
      "visited set ids");
  add_memory_component(
      bytes,
      checked_mul(candidate_nodes, estimate_decoded_node_bytes(md),
                  "query-local decoded nodes"),
      "query-local decoded nodes");
  add_memory_component(
      bytes,
      checked_mul(candidate_nodes, 48, "query hash table overhead"),
      "query hash table overhead");
  return bytes;
}

std::size_t estimate_io_buffer_bytes(const Args& args,
                                     const agentmem::DiskGraphMetadata& md) {
  if (args.layout != "packed" && args.io_mode == "pread") {
    return 0;
  }

  const std::size_t page_size = static_cast<std::size_t>(md.page_size);
  std::size_t bytes = estimate_decoded_page_bytes(md);
  add_memory_component(bytes, page_size, "graph page read buffer");

  if (args.io_mode == "odirect") {
    add_memory_component(bytes, page_size, "O_DIRECT aligned page buffer");
  } else if (args.io_mode == "io_uring") {
    const std::size_t batch =
        std::max<std::size_t>(1, std::min<std::size_t>(args.io_depth, 4096));
    add_memory_component(
        bytes, checked_mul(batch, page_size, "io_uring pending buffers"),
        "io_uring pending buffers");
    add_memory_component(
        bytes, checked_mul(batch, page_size, "io_uring completion copies"),
        "io_uring completion copies");
    add_memory_component(
        bytes,
        checked_mul(batch, estimate_decoded_page_bytes(md),
                    "io_uring decoded ready pages"),
        "io_uring decoded ready pages");
    add_memory_component(
        bytes,
        checked_add(checked_mul(batch, 128, "io_uring sqe/cqe estimate"),
                    16 * 4096, "io_uring ring mmap estimate"),
        "io_uring ring mmap estimate");
  }

  return bytes;
}

std::size_t estimate_delta_bytes(const Args& args, const RunOutput& output,
                                 std::size_t dim) {
  const std::size_t total_delta =
      checked_add(output.delta_active_size, output.delta_sealed_size,
                  "delta vector count");
  std::size_t bytes = checked_mul(
      total_delta,
      checked_add(sizeof(std::uint32_t),
                  checked_mul(dim, sizeof(float), "delta vector bytes"),
                  "delta record bytes"),
      "delta flat bytes");
  if (args.delta_index_policy == "ivf-flat" && total_delta > 0) {
    add_memory_component(bytes, bytes, "delta IVF mirrored vectors");
    add_memory_component(
        bytes,
        checked_mul(checked_mul(args.delta_ivf_centroids, dim,
                                "delta IVF centroids"),
                    sizeof(float), "delta IVF centroid bytes"),
        "delta IVF centroids");
  }
  return bytes;
}

std::size_t estimate_tombstone_bytes(const RunOutput& output,
                                     std::size_t base_count) {
  if (output.tombstone_count == 0) {
    return 0;
  }
  return checked_add((base_count + 7) / 8, output.tombstone_count,
                     "tombstone bitmap plus sparse deletes");
}

void finalize_memory_report(const Args& args, MemoryReport& report) {
  report.budget_ratio = args.memory_budget_ratio;
  report.budget_bytes = memory_budget_bytes(args, report.raw_vector_bytes);
  report.over_budget_allowed = args.allow_over_budget_for_debug;

  std::size_t resident = 0;
  add_memory_component(resident, report.bytes_raw_vectors_resident,
                       "raw resident vectors");
  add_memory_component(resident, report.bytes_pq_codes, "PQ codes");
  add_memory_component(resident, report.bytes_pq_codebooks, "PQ codebooks");
  add_memory_component(resident, report.bytes_graph_metadata, "graph metadata");
  add_memory_component(resident, report.bytes_cache, "page cache");
  add_memory_component(resident, report.bytes_path_cache, "path cache");
  add_memory_component(resident, report.bytes_router, "router");
  add_memory_component(resident, report.bytes_query_workspace,
                       "query workspace");
  add_memory_component(resident, report.bytes_io_buffers, "I/O buffers");
  add_memory_component(resident, report.bytes_delta, "delta");
  add_memory_component(resident, report.bytes_tombstone, "tombstone");
  add_memory_component(resident, report.bytes_temporary_peak, "temporary peak");
  report.resident_bytes = resident;
  report.resident_ratio =
      report.raw_vector_bytes == 0
          ? 0.0
          : static_cast<double>(report.resident_bytes) /
                static_cast<double>(report.raw_vector_bytes);
  report.budget_pass = report.resident_bytes <= report.budget_bytes;

  if (args.enforce_memory_budget && !report.budget_pass &&
      !args.allow_over_budget_for_debug) {
    throw std::runtime_error(
        "Resident engine memory exceeds --memory-budget-ratio/bytes; "
        "rerun with --allow-over-budget-for-debug to collect debug metrics");
  }
}

MemoryReport build_exact_memory_report(const Args& args,
                                       const agentmem::VectorSet& base) {
  MemoryReport report;
  report.memory_mode = true;
  report.raw_vector_bytes = checked_mul(base.values.size(), sizeof(float),
                                        "raw exact vectors");
  report.bytes_raw_vectors_resident = report.raw_vector_bytes;
  report.bytes_temporary_peak = estimate_temporary_peak_bytes(args, base.dim);
  finalize_memory_report(args, report);
  return report;
}

template <typename Index>
MemoryReport build_graph_memory_report(
    const Args& args, const Index& index,
    const agentmem::PqAdcModel* pq_model, const QueryPathCache& path_cache,
    const ResidentQueryRouter& router, const RunOutput& output) {
  MemoryReport report;
  report.memory_mode = false;
  report.raw_vector_bytes = checked_mul(
      checked_mul(static_cast<std::size_t>(index.metadata().vector_count),
                  static_cast<std::size_t>(index.metadata().dim),
                  "raw graph vector entries"),
      sizeof(float), "raw graph vectors");
  if (pq_model != nullptr) {
    report.bytes_pq_codes = pq_model->code_bytes();
    report.bytes_pq_codebooks =
        checked_add(pq_model->codebook_bytes(), pq_model->offset_bytes(),
                    "PQ codebooks and offsets");
  }
  report.bytes_graph_metadata =
      estimate_graph_metadata_bytes(args, index.metadata());
  report.bytes_cache = estimate_cache_bytes(args, index.metadata());
  report.bytes_path_cache =
      path_cache.estimated_capacity_bytes(args.entry_count, args.k);
  report.bytes_router = router.estimated_bytes();
  report.bytes_query_workspace =
      estimate_graph_workspace_bytes(args, index.metadata());
  report.bytes_io_buffers = estimate_io_buffer_bytes(args, index.metadata());
  report.bytes_delta = estimate_delta_bytes(args, output, index.metadata().dim);
  report.bytes_tombstone = estimate_tombstone_bytes(
      output, static_cast<std::size_t>(index.metadata().vector_count));
  report.bytes_temporary_peak =
      estimate_temporary_peak_bytes(args, index.metadata().dim);
  finalize_memory_report(args, report);
  return report;
}

template <typename Index>
agentmem::DiskGraphSearchResult search_graph_one_with_routing(
    Index& index, const Args& args, const float* query,
    QueryPathCache& path_cache, const ResidentQueryRouter& router,
    const agentmem::PqAdcModel* pq_model, bool& path_hit) {
  agentmem::DiskGraphSearchConfig per_query_config;
  per_query_config.top_k = args.k;
  per_query_config.search_width = args.search_width;
  per_query_config.entry_count = args.entry_count;
  per_query_config.early_stop = args.search_early_stop;
  per_query_config.early_stop_min_expansions =
      args.search_early_stop_min_expansions;
  per_query_config.adaptive_early_stop = args.adaptive_early_stop;
  per_query_config.early_stop_patience = args.early_stop_patience;
  per_query_config.early_stop_eps = args.early_stop_eps;
  per_query_config.min_expansions = args.min_expansions;
  per_query_config.pq_model = pq_model;
  per_query_config.adc_enable = args.adc_enable;
  per_query_config.rerank_topk = args.rerank_topk;
  per_query_config.prefetch_width = args.prefetch_width;
  per_query_config.prefetch_policy = args.prefetch_policy;
  per_query_config.page_dedup = args.page_dedup;
  per_query_config.same_page_reuse = args.same_page_reuse;
  per_query_config.page_coalesce = args.page_coalesce;
  if (args.adaptive_early_stop) {
    per_query_config.search_width =
        std::min(per_query_config.search_width, args.max_expansions);
  }

  const RoutedEntries routed = router.route(query, args.entry_count);
  per_query_config.seed_ids = routed.seeds;
  path_hit = false;
  const PathCacheEntry* cached = path_cache.lookup(routed.signature);
  if (cached != nullptr) {
    path_hit = true;  // 命中后合并历史 Top-K、历史入口和本次路由入口。
    per_query_config.seed_ids =
        merge_ids(cached->top_ids, cached->seeds, args.entry_count);
    per_query_config.seed_ids =
        merge_ids(per_query_config.seed_ids, routed.seeds, args.entry_count);
    per_query_config.search_width =  // 复用路径后减少图扩展预算。
        std::min(args.search_width, args.path_cache_hit_search_width);
  }
  if (args.adaptive_early_stop) {
    per_query_config.search_width =
        std::min(per_query_config.search_width, args.max_expansions);
  }

  auto result = index.search_one(query, per_query_config);
  path_cache.update(routed.signature, routed.seeds, result.topk);
  return result;
}

template <typename Index>
RunOutput execute_graph_queries(Index& index, const Args& args,
                                const agentmem::VectorSet& queries,
                                bool collect_output,
                                QueryPathCache& path_cache,
                                const ResidentQueryRouter& router,
                                const agentmem::PqAdcModel* pq_model) {
  RunOutput output;
  if (collect_output) {  // warmup 阶段仍执行查询，但不污染正式统计。
    output.results.reserve(queries.size());
    output.latencies_ms.reserve(queries.size());
    output.ssd_reads.reserve(queries.size());
    output.cache_requests.reserve(queries.size());
    output.cache_hits.reserve(queries.size());
    output.cache_misses.reserve(queries.size());
    output.cache_evictions.reserve(queries.size());
    output.cache_promotions.reserve(queries.size());
    output.path_cache_requests.reserve(queries.size());
    output.path_cache_hits.reserve(queries.size());
    output.expanded.reserve(queries.size());
    output.visited.reserve(queries.size());
    output.io_submits.reserve(queries.size());
    output.io_completions.reserve(queries.size());
    output.io_submit_syscalls.reserve(queries.size());
    output.io_prefetches.reserve(queries.size());
    output.io_prefetch_hits.reserve(queries.size());
    output.io_prefetch_waits.reserve(queries.size());
    output.io_pending_pages_peak.reserve(queries.size());
    output.prefetch_submitted_pages.reserve(queries.size());
    output.prefetch_useful_pages.reserve(queries.size());
    output.prefetch_wasted_pages.reserve(queries.size());
    output.demand_read_waits.reserve(queries.size());
    output.demand_read_wait_us.reserve(queries.size());
    output.page_dedup_ratios.reserve(queries.size());
    output.page_requests_before_dedup.reserve(queries.size());
    output.page_requests_after_dedup.reserve(queries.size());
    output.same_page_node_reuse.reserve(queries.size());
    output.adc_table_build_us.reserve(queries.size());
    output.rerank_reads.reserve(queries.size());
    output.pq_filter_reject_count.reserve(queries.size());
    output.pq_filter_accept_count.reserve(queries.size());
  }

  const auto batch_start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < queries.size(); ++i) {
    bool path_hit = false;
    const auto query_start = std::chrono::steady_clock::now();
    auto result = search_graph_one_with_routing(index, args, queries.row(i),
                                                path_cache, router, pq_model,
                                                path_hit);
    const auto query_end = std::chrono::steady_clock::now();

    if (collect_output) {
      output.latencies_ms.push_back(
          std::chrono::duration<double, std::milli>(query_end - query_start)
              .count());
      output.ssd_reads.push_back(static_cast<double>(result.stats.node_reads));
      output.cache_requests.push_back(
          static_cast<double>(result.stats.page_requests));
      output.cache_hits.push_back(
          static_cast<double>(result.stats.page_cache_hits));
      output.cache_misses.push_back(
          static_cast<double>(result.stats.page_cache_misses));
      output.cache_evictions.push_back(
          static_cast<double>(result.stats.page_cache_evictions));
      output.cache_promotions.push_back(
          static_cast<double>(result.stats.page_cache_promotions));
      output.path_cache_requests.push_back(path_cache.enabled() ? 1.0 : 0.0);
      output.path_cache_hits.push_back(path_hit ? 1.0 : 0.0);
      output.expanded.push_back(static_cast<double>(result.stats.expanded));
      output.visited.push_back(static_cast<double>(result.stats.visited));
      output.io_submits.push_back(
          static_cast<double>(result.stats.io_submits));
      output.io_completions.push_back(
          static_cast<double>(result.stats.io_completions));
      output.io_submit_syscalls.push_back(
          static_cast<double>(result.stats.io_submit_syscalls));
      output.io_prefetches.push_back(
          static_cast<double>(result.stats.io_prefetches));
      output.io_prefetch_hits.push_back(
          static_cast<double>(result.stats.io_prefetch_hits));
      output.io_prefetch_waits.push_back(
          static_cast<double>(result.stats.io_prefetch_waits));
      output.io_pending_pages_peak.push_back(
          static_cast<double>(result.stats.io_pending_pages_peak));
      output.prefetch_submitted_pages.push_back(
          static_cast<double>(result.stats.prefetch_submitted_pages));
      output.prefetch_useful_pages.push_back(
          static_cast<double>(result.stats.prefetch_useful_pages));
      output.prefetch_wasted_pages.push_back(
          static_cast<double>(result.stats.prefetch_wasted_pages));
      output.demand_read_waits.push_back(
          static_cast<double>(result.stats.demand_read_waits));
      output.demand_read_wait_us.push_back(result.stats.demand_read_wait_us);
      output.page_dedup_ratios.push_back(
          result.stats.page_dedup_requests == 0
              ? 0.0
              : static_cast<double>(result.stats.page_dedup_hits) /
                    static_cast<double>(result.stats.page_dedup_requests));
      output.page_requests_before_dedup.push_back(
          static_cast<double>(result.stats.page_requests_before_dedup));
      output.page_requests_after_dedup.push_back(
          static_cast<double>(result.stats.page_requests_after_dedup));
      output.same_page_node_reuse.push_back(
          static_cast<double>(result.stats.same_page_node_reuse));
      output.adc_table_build_us.push_back(result.stats.adc_table_build_us);
      output.rerank_reads.push_back(
          static_cast<double>(result.stats.rerank_reads));
      output.pq_filter_reject_count.push_back(
          static_cast<double>(result.stats.pq_filter_reject_count));
      output.pq_filter_accept_count.push_back(
          static_cast<double>(result.stats.pq_filter_accept_count));
      output.results.push_back(std::move(result.topk));
    }
  }
  const auto batch_end = std::chrono::steady_clock::now();
  output.elapsed_seconds =
      std::chrono::duration<double>(batch_end - batch_start).count();
  return output;
}

std::vector<std::uint32_t> ids_from_results(
    const std::vector<agentmem::SearchResult>& results) {
  std::vector<std::uint32_t> ids;
  ids.reserve(results.size());
  for (const auto& result : results) {
    ids.push_back(result.id);
  }
  return ids;
}

double recall_one_at_k(const std::vector<agentmem::SearchResult>& approx,
                       const std::vector<agentmem::SearchResult>& truth,
                       std::size_t k) {
  if (k == 0 || truth.empty()) {
    return 1.0;
  }
  const std::size_t truth_count = std::min(k, truth.size());
  std::unordered_set<std::uint32_t> truth_ids;
  truth_ids.reserve(truth_count);
  for (std::size_t i = 0; i < truth_count; ++i) {
    truth_ids.insert(truth[i].id);
  }

  std::size_t hits = 0;
  const std::size_t approx_count = std::min(k, approx.size());
  for (std::size_t i = 0; i < approx_count; ++i) {
    if (truth_ids.find(approx[i].id) != truth_ids.end()) {
      ++hits;
    }
  }
  return static_cast<double>(hits) / static_cast<double>(truth_count);
}

std::vector<float> make_insert_vector(const agentmem::VectorSet& queries,
                                      std::size_t query_index,
                                      std::size_t insert_index,
                                      std::uint32_t seed) {
  std::vector<float> vector(queries.dim, 0.0f);
  const float* source = queries.row(query_index % queries.size());  // 模拟近期 memory 写入。
  for (std::size_t d = 0; d < queries.dim; ++d) {
    const int bucket =
        static_cast<int>((insert_index * 31 + d * 17 + seed) % 23) - 11;
    vector[d] = source[d] + static_cast<float>(bucket) * 0.0025f;
  }
  return vector;
}

double recent_p99_ms(const std::vector<double>& latencies,
                     std::size_t window) {
  if (latencies.empty()) {
    return 0.0;
  }
  const std::size_t begin =  // SLA compaction 只观察近期查询窗口。
      latencies.size() > window ? latencies.size() - window : 0;
  std::vector<double> recent(latencies.begin() +
                                 static_cast<std::ptrdiff_t>(begin),
                             latencies.end());
  return agentmem::summarize_latency(recent).p99_ms;
}

struct CompactionTick {
  std::size_t moved = 0;     // active delta 转入 sealed delta 的向量数。
  std::size_t io_bytes = 0;  // 可观测的模拟或真实文件写入量。
  double ms = 0.0;           // 本次 compaction 对路径造成的时间成本。
};

void write_compaction_io_file(const std::string& path, std::size_t bytes) {
  // V6 文件 I/O 模式：用真实 append + flush/fsync 产生可测量写入压力。
  if (bytes == 0) {
    return;
  }

  std::vector<char> block(std::min<std::size_t>(bytes, 1024 * 1024), 0);
  for (std::size_t i = 0; i < block.size(); ++i) {
    block[i] = static_cast<char>((i * 131) & 0xff);
  }

#ifdef _WIN32
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    throw std::runtime_error("Cannot open compaction I/O file: " + path);
  }
  std::size_t written = 0;
  while (written < bytes) {
    const std::size_t chunk = std::min(block.size(), bytes - written);
    output.write(block.data(), static_cast<std::streamsize>(chunk));
    if (!output) {
      throw std::runtime_error("Failed to write compaction I/O file: " + path);
    }
    written += chunk;
  }
  output.flush();
  if (!output) {
    throw std::runtime_error("Failed to flush compaction I/O file: " + path);
  }
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd < 0) {
    throw std::runtime_error("Cannot open compaction I/O file: " + path);
  }

  std::size_t written = 0;
  while (written < bytes) {
    const std::size_t chunk = std::min(block.size(), bytes - written);
    const ssize_t rc = ::write(fd, block.data(), chunk);
    if (rc < 0) {
      ::close(fd);
      throw std::runtime_error("Failed to write compaction I/O file: " + path);
    }
    written += static_cast<std::size_t>(rc);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("Failed to fsync compaction I/O file: " + path);
  }
  ::close(fd);
#endif
}

CompactionTick run_compaction_tick(agentmem::DeltaFlatIndex& delta,
                                   const Args& args) {
  CompactionTick tick;
  if (delta.active_size() < args.delta_compaction_threshold) {
    return tick;
  }

  const auto start = std::chrono::steady_clock::now();
  tick.moved = delta.compact_active_to_sealed(args.compaction_batch_size);
  tick.io_bytes = tick.moved * args.compaction_io_bytes_per_vector;
  if (tick.moved > 0 && args.compaction_io_mode == "file") {
    write_compaction_io_file(args.compaction_io_path, tick.io_bytes);
  } else if (tick.moved > 0 && args.compaction_work_us > 0) {
    const auto until =
        std::chrono::steady_clock::now() +
        std::chrono::microseconds(args.compaction_work_us);
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  const auto end = std::chrono::steady_clock::now();
  tick.ms = std::chrono::duration<double, std::milli>(end - start).count();
  return tick;
}

void record_compaction(RunOutput& output, const CompactionTick& tick,
                       bool on_query_path) {
  if (tick.moved == 0) {
    return;
  }
  ++output.compaction_ops;
  output.compaction_vectors += tick.moved;
  output.compaction_io_bytes += tick.io_bytes;
  output.compaction_seconds += tick.ms / 1000.0;
  if (on_query_path) {
    ++output.queries_with_compaction;
  }
}

std::vector<agentmem::SearchResult> filter_deleted_results(
    const std::vector<agentmem::SearchResult>& results,
    const std::unordered_set<std::uint32_t>& deleted, std::size_t k) {
  if (deleted.empty()) {
    return results;
  }
  std::vector<agentmem::SearchResult> filtered;
  filtered.reserve(std::min(k, results.size()));
  for (const auto& result : results) {
    if (deleted.find(result.id) == deleted.end()) {
      filtered.push_back(result);
      if (filtered.size() >= k) {
        break;
      }
    }
  }
  return filtered;
}

std::vector<std::uint32_t> deleted_ids_vector(
    const std::unordered_set<std::uint32_t>& deleted) {
  std::vector<std::uint32_t> ids;
  ids.reserve(deleted.size());
  for (const auto id : deleted) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::uint32_t pick_delete_id(std::size_t base_count,
                             const std::unordered_set<std::uint32_t>& deleted,
                             std::size_t& delete_cursor) {
  if (deleted.size() >= base_count) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  for (std::size_t attempts = 0; attempts < base_count; ++attempts) {
    const std::size_t candidate =
        (delete_cursor * 2654435761ull + 17ull) % base_count;
    ++delete_cursor;
    const auto id = static_cast<std::uint32_t>(candidate);
    if (deleted.find(id) == deleted.end()) {
      return id;
    }
  }
  return std::numeric_limits<std::uint32_t>::max();
}

agentmem::VectorSet build_stream_merge_vectors(
    const agentmem::VectorSet& base,
    const std::vector<std::uint32_t>& inserted_ids,
    const std::vector<float>& inserted_values,
    std::unordered_set<std::uint32_t>& deleted) {
  std::uint32_t max_id =
      base.empty() ? 0 : static_cast<std::uint32_t>(base.size() - 1);
  for (const auto id : inserted_ids) {
    max_id = std::max(max_id, id);
  }

  agentmem::VectorSet merged(static_cast<std::size_t>(max_id) + 1, base.dim);
  for (std::size_t i = 0; i < base.size(); ++i) {
    float* target = merged.mutable_row(i);
    const float* source = base.row(i);
    std::copy(source, source + base.dim, target);
  }

  std::unordered_set<std::uint32_t> inserted_seen;
  inserted_seen.reserve(inserted_ids.size());
  for (std::size_t i = 0; i < inserted_ids.size(); ++i) {
    const auto id = inserted_ids[i];
    if (id >= merged.size()) {
      continue;
    }
    float* target = merged.mutable_row(id);
    const float* source = inserted_values.data() + i * base.dim;
    std::copy(source, source + base.dim, target);
    inserted_seen.insert(id);
  }

  for (std::size_t id = base.size(); id < merged.size(); ++id) {
    if (inserted_seen.find(static_cast<std::uint32_t>(id)) ==
        inserted_seen.end()) {
      deleted.insert(static_cast<std::uint32_t>(id));
    }
  }
  return merged;
}

void run_stream_merge(const Args& args, const agentmem::VectorSet& base,
                      const std::vector<std::uint32_t>& inserted_ids,
                      const std::vector<float>& inserted_values,
                      std::unordered_set<std::uint32_t>& deleted,
                      RunOutput& output) {
  const auto merge_start = std::chrono::steady_clock::now();
  // LSM-style 收尾合并：重建一个包含插入和 tombstone 修补的新 LTI 文件。
  const auto merged =
      build_stream_merge_vectors(base, inserted_ids, inserted_values, deleted);

  agentmem::DiskGraphBuildConfig build_config;
  build_config.degree = args.graph_degree;
  build_config.page_size = args.page_size;
  build_config.build_policy = args.graph_build_policy;
  build_config.packing_strategy =
      args.packing_strategy == "hotpath" ? "bfs" : args.packing_strategy;
  build_config.random_seed = args.synthetic_config.seed;
  build_config.approx_projections = args.approx_projections;
  build_config.approx_window = args.approx_window;
  build_config.approx_random_samples = args.approx_random_samples;
  build_config.approx_candidate_limit = args.approx_candidate_limit;
  build_config.lsh_tables = args.lsh_tables;
  build_config.lsh_bits = args.lsh_bits;
  build_config.lsh_probe_radius = args.lsh_probe_radius;
  build_config.lsh_bucket_limit = args.lsh_bucket_limit;
  build_config.robust_prune_alpha = args.robust_prune_alpha;
  build_config.deleted_ids = deleted_ids_vector(deleted);
  build_config.coaccess_sessions = args.coaccess_sessions;
  build_config.coaccess_trace_length = args.coaccess_trace_length;

  const std::string path =
      args.stream_merge_index_path.empty()
          ? args.index_path + ".streammerge.idx"
          : args.stream_merge_index_path;
  if (args.layout == "one-node") {
    agentmem::NaiveDiskGraphBuilder::build(merged, path, build_config);
  } else {
    agentmem::PackedDiskGraphBuilder::build(merged, path, build_config);
  }

  const auto merge_end = std::chrono::steady_clock::now();
  ++output.stream_merge_ops;
  output.stream_merge_vectors = merged.size();
  output.stream_merge_deleted = deleted.size();
  output.stream_merge_inserted = inserted_ids.size();
  output.stream_merge_seconds +=
      std::chrono::duration<double>(merge_end - merge_start).count();
}

template <typename Index>
RunOutput run_mixed_graph_with_index(Index& index, const Args& args,
                                     const agentmem::VectorSet& base,
                                     const agentmem::VectorSet& queries,
                                     QueryPathCache& path_cache,
                                     const ResidentQueryRouter& router,
                                     const agentmem::PqAdcModel* pq_model) {
  if (args.engine != "graph") {
    throw std::runtime_error("Mixed workload currently requires --engine graph");
  }
  if (queries.empty()) {
    throw std::runtime_error("Mixed workload requires non-empty queries");
  }

  RunOutput output;
  output.operation_count =
      args.operation_count == 0 ? queries.size() : args.operation_count;
  output.results.reserve(output.operation_count);
  output.latencies_ms.reserve(output.operation_count);
  output.ssd_reads.reserve(output.operation_count);
  output.cache_requests.reserve(output.operation_count);
  output.cache_hits.reserve(output.operation_count);
  output.cache_misses.reserve(output.operation_count);
  output.cache_evictions.reserve(output.operation_count);
  output.cache_promotions.reserve(output.operation_count);
  output.io_submits.reserve(output.operation_count);
  output.io_completions.reserve(output.operation_count);
  output.io_submit_syscalls.reserve(output.operation_count);
  output.io_prefetches.reserve(output.operation_count);
  output.io_prefetch_hits.reserve(output.operation_count);
  output.io_prefetch_waits.reserve(output.operation_count);
  output.io_pending_pages_peak.reserve(output.operation_count);
  output.prefetch_submitted_pages.reserve(output.operation_count);
  output.prefetch_useful_pages.reserve(output.operation_count);
  output.prefetch_wasted_pages.reserve(output.operation_count);
  output.demand_read_waits.reserve(output.operation_count);
  output.demand_read_wait_us.reserve(output.operation_count);
  output.page_dedup_ratios.reserve(output.operation_count);
  output.page_requests_before_dedup.reserve(output.operation_count);
  output.page_requests_after_dedup.reserve(output.operation_count);
  output.same_page_node_reuse.reserve(output.operation_count);
  output.adc_table_build_us.reserve(output.operation_count);
  output.rerank_reads.reserve(output.operation_count);
  output.pq_filter_reject_count.reserve(output.operation_count);
  output.pq_filter_accept_count.reserve(output.operation_count);
  output.path_cache_requests.reserve(output.operation_count);
  output.path_cache_hits.reserve(output.operation_count);
  output.expanded.reserve(output.operation_count);
  output.visited.reserve(output.operation_count);
  output.insert_latencies_ms.reserve(output.operation_count);
  output.query_compaction_ms.reserve(output.operation_count);
  output.delta_search_ms.reserve(output.operation_count);
  output.delta_exact_search_ms.reserve(output.operation_count);
  output.delta_recalls.reserve(output.operation_count);
  output.dynamic_truth.reserve(output.operation_count);

  agentmem::DeltaFlatIndex delta(base.dim);  // flat 始终保留，作为 Delta ANN 正确性参考。
  std::unique_ptr<agentmem::DeltaIvfFlatIndex> delta_ann;
  if (args.delta_index_policy == "ivf-flat") {
    delta_ann = std::make_unique<agentmem::DeltaIvfFlatIndex>(
        base.dim, args.delta_ivf_centroids, args.delta_ivf_probes,
        args.delta_ivf_train_iterations, args.delta_ivf_rebuild_interval);
  }

  std::size_t query_index = 0;
  std::size_t insert_index = 0;
  std::size_t delete_index = 0;
  std::size_t delete_budget = 0;
  std::vector<std::uint32_t> inserted_ids;
  std::vector<float> inserted_values;
  std::unordered_set<std::uint32_t> deleted;
  agentmem::WalStats initial_wal_stats;
  if (args.wal_replay) {  // 重启恢复：先 replay，再以 append 模式继续写 WAL。
    const auto replay = agentmem::replay_wal_records(
        args.wal_path, base.dim,
        [&](std::uint32_t id, const float* vector) {
          delta.insert(id, vector);
          if (delta_ann) {
            delta_ann->insert(id, vector);
          }
          inserted_ids.push_back(id);
          const std::size_t old_size = inserted_values.size();
          inserted_values.resize(old_size + base.dim);
          std::copy(vector, vector + base.dim,
                    inserted_values.data() + old_size);
        },
        [&](std::uint32_t id) {
          deleted.insert(id);
        });
    output.wal_replay_records = replay.records;
    output.wal_replay_inserts = replay.inserts;
    output.wal_replay_deletes = replay.deletes;
    output.wal_replay_bytes = replay.bytes;
    output.wal_replay_delta_size = delta.total_size();
    initial_wal_stats.records = replay.records;
    initial_wal_stats.bytes = replay.bytes;
    output.delete_count += replay.deletes;
    insert_index =
        replay.has_records && replay.max_id >= base.size()
            ? static_cast<std::size_t>(replay.max_id - base.size() + 1)
            : replay.records;
  }

  agentmem::WalWriter wal(args.wal_path, base.dim, args.wal_replay,
                          initial_wal_stats);
  const agentmem::BruteForceIndex exact_base(base);

  std::size_t write_budget = 0;

  const auto batch_start = std::chrono::steady_clock::now();
  for (std::size_t op = 0; op < output.operation_count; ++op) {
    write_budget += args.write_ratio_percent;  // 百分比累加器让混合负载可复现。
    const bool is_update = write_budget >= 100;
    if (is_update) {
      write_budget -= 100;
      delete_budget += args.delete_ratio_percent;  // delete_ratio 仅作用于更新操作。
      const bool should_delete =
          delete_budget >= 100 && deleted.size() < base.size();
      if (should_delete) {
        delete_budget -= 100;
        const auto delete_start = std::chrono::steady_clock::now();
        const auto id = pick_delete_id(base.size(), deleted, delete_index);
        if (id != std::numeric_limits<std::uint32_t>::max()) {
          wal.append_delete(id);
          wal.flush();  // 先持久化 WAL，再更新内存 tombstone。
          deleted.insert(id);
          ++output.delete_count;
        }
        const auto delete_end = std::chrono::steady_clock::now();
        output.insert_latencies_ms.push_back(
            std::chrono::duration<double, std::milli>(delete_end -
                                                      delete_start)
                .count());
        continue;
      }

      const auto insert_start = std::chrono::steady_clock::now();
      const auto vector = make_insert_vector(
          queries, query_index == 0 ? 0 : query_index - 1, insert_index,
          args.synthetic_config.seed);
      const auto id =
          static_cast<std::uint32_t>(base.size() + insert_index);
      wal.append_insert(id, vector.data());
      wal.flush();  // 插入同样遵循 WAL-first。
      delta.insert(id, vector.data());
      if (delta_ann) {
        delta_ann->insert(id, vector.data());
      }
      inserted_ids.push_back(id);
      inserted_values.insert(inserted_values.end(), vector.begin(),
                             vector.end());
      ++insert_index;
      ++output.insert_count;

      if (args.compaction_policy == "sla") {
        const double p99 = recent_p99_ms(output.latencies_ms, 32);
        if (p99 == 0.0 || p99 < args.sla_p99_ms) {
          const auto tick = run_compaction_tick(delta, args);
          record_compaction(output, tick, false);
        } else if (delta.active_size() >= args.delta_compaction_threshold) {
          ++output.compaction_skipped_sla;
        }
      }

      const auto insert_end = std::chrono::steady_clock::now();
      output.insert_latencies_ms.push_back(
          std::chrono::duration<double, std::milli>(insert_end - insert_start)
              .count());
      continue;
    }

    const float* query = queries.row(query_index % queries.size());
    ++query_index;
    bool path_hit = false;
    double query_compaction_ms = 0.0;

    const auto query_start = std::chrono::steady_clock::now();
    if (args.compaction_policy == "aggressive") {
      const auto tick = run_compaction_tick(delta, args);
      query_compaction_ms = tick.ms;
      record_compaction(output, tick, true);
    } else if (args.compaction_policy == "sla" &&
               delta.active_size() >= args.delta_compaction_threshold) {
      ++output.compaction_skipped_sla;
    }

    auto main_result = search_graph_one_with_routing(  // Main LTI 图索引结果。
        index, args, query, path_cache, router, pq_model, path_hit);
    const auto delta_exact_start = std::chrono::steady_clock::now();
    const auto delta_exact_results = delta.search_one(query, args.k);
    const auto delta_exact_end = std::chrono::steady_clock::now();
    const double delta_exact_ms =
        std::chrono::duration<double, std::milli>(delta_exact_end -
                                                  delta_exact_start)
            .count();

    std::vector<agentmem::SearchResult> delta_results;
    double delta_search_ms = delta_exact_ms;
    if (delta_ann) {
      const auto delta_search_start = std::chrono::steady_clock::now();
      delta_results = delta_ann->search_one(query, args.k);
      const auto delta_search_end = std::chrono::steady_clock::now();
      delta_search_ms =
          std::chrono::duration<double, std::milli>(delta_search_end -
                                                    delta_search_start)
              .count();
    } else {
      delta_results = delta_exact_results;
    }
    const auto merged = filter_deleted_results(  // Main + Delta 合并后过滤 tombstone。
        agentmem::merge_topk(main_result.topk, delta_results,
                             args.k + deleted.size()),
        deleted, args.k);
    const auto truth_results = filter_deleted_results(
        agentmem::merge_topk(exact_base.search_one(query, args.k + deleted.size()),
                             delta_exact_results, args.k + deleted.size()),
        deleted, args.k);
    const auto query_end = std::chrono::steady_clock::now();

    output.latencies_ms.push_back(
        std::chrono::duration<double, std::milli>(query_end - query_start)
            .count());
    output.query_compaction_ms.push_back(query_compaction_ms);
    output.delta_search_ms.push_back(delta_search_ms);
    output.delta_exact_search_ms.push_back(delta_exact_ms);
    output.delta_recalls.push_back(
        recall_one_at_k(delta_results, delta_exact_results, args.k));
    output.ssd_reads.push_back(static_cast<double>(main_result.stats.node_reads));
    output.cache_requests.push_back(
        static_cast<double>(main_result.stats.page_requests));
    output.cache_hits.push_back(
        static_cast<double>(main_result.stats.page_cache_hits));
    output.cache_misses.push_back(
        static_cast<double>(main_result.stats.page_cache_misses));
    output.cache_evictions.push_back(
        static_cast<double>(main_result.stats.page_cache_evictions));
    output.cache_promotions.push_back(
        static_cast<double>(main_result.stats.page_cache_promotions));
    output.path_cache_requests.push_back(path_cache.enabled() ? 1.0 : 0.0);
    output.path_cache_hits.push_back(path_hit ? 1.0 : 0.0);
    output.expanded.push_back(static_cast<double>(main_result.stats.expanded));
    output.visited.push_back(static_cast<double>(main_result.stats.visited));
    output.io_submits.push_back(
        static_cast<double>(main_result.stats.io_submits));
    output.io_completions.push_back(
        static_cast<double>(main_result.stats.io_completions));
    output.io_submit_syscalls.push_back(
        static_cast<double>(main_result.stats.io_submit_syscalls));
    output.io_prefetches.push_back(
        static_cast<double>(main_result.stats.io_prefetches));
    output.io_prefetch_hits.push_back(
        static_cast<double>(main_result.stats.io_prefetch_hits));
    output.io_prefetch_waits.push_back(
        static_cast<double>(main_result.stats.io_prefetch_waits));
    output.io_pending_pages_peak.push_back(
        static_cast<double>(main_result.stats.io_pending_pages_peak));
    output.prefetch_submitted_pages.push_back(
        static_cast<double>(main_result.stats.prefetch_submitted_pages));
    output.prefetch_useful_pages.push_back(
        static_cast<double>(main_result.stats.prefetch_useful_pages));
    output.prefetch_wasted_pages.push_back(
        static_cast<double>(main_result.stats.prefetch_wasted_pages));
    output.demand_read_waits.push_back(
        static_cast<double>(main_result.stats.demand_read_waits));
    output.demand_read_wait_us.push_back(
        main_result.stats.demand_read_wait_us);
    output.page_dedup_ratios.push_back(
        main_result.stats.page_dedup_requests == 0
            ? 0.0
            : static_cast<double>(main_result.stats.page_dedup_hits) /
                  static_cast<double>(main_result.stats.page_dedup_requests));
    output.page_requests_before_dedup.push_back(
        static_cast<double>(main_result.stats.page_requests_before_dedup));
    output.page_requests_after_dedup.push_back(
        static_cast<double>(main_result.stats.page_requests_after_dedup));
    output.same_page_node_reuse.push_back(
        static_cast<double>(main_result.stats.same_page_node_reuse));
    output.adc_table_build_us.push_back(main_result.stats.adc_table_build_us);
    output.rerank_reads.push_back(
        static_cast<double>(main_result.stats.rerank_reads));
    output.pq_filter_reject_count.push_back(
        static_cast<double>(main_result.stats.pq_filter_reject_count));
    output.pq_filter_accept_count.push_back(
        static_cast<double>(main_result.stats.pq_filter_accept_count));
    output.results.push_back(merged);
    output.dynamic_truth.push_back(ids_from_results(truth_results));
  }
  const auto batch_end = std::chrono::steady_clock::now();
  output.elapsed_seconds =
      std::chrono::duration<double>(batch_end - batch_start).count();
  output.delta_active_size = delta.active_size();
  output.delta_sealed_size = delta.sealed_size();
  output.tombstone_count = deleted.size();
  output.wal_records = wal.stats().records;
  output.wal_bytes = wal.stats().bytes;
  if (args.compaction_policy == "stream-merge" &&
      (!inserted_ids.empty() || !deleted.empty())) {
    run_stream_merge(args, base, inserted_ids, inserted_values, deleted,
                     output);
    output.tombstone_count = deleted.size();
  }
  return output;
}

std::uint64_t file_size_bytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return 0;
  }
  const auto end = input.tellg();
  if (end == std::streampos(-1)) {
    return 0;
  }
  return static_cast<std::uint64_t>(end);
}

template <typename Index>
RunOutput run_graph_with_index(Index& index, const Args& args,
                               agentmem::VectorSet& base,
                               const agentmem::VectorSet& queries) {
  if (index.metadata().dim != base.dim) {
    throw std::runtime_error("Graph index dimension does not match loaded base");
  }
  if (index.metadata().vector_count != base.size()) {
    throw std::runtime_error("Graph index vector count does not match loaded base");
  }
  index.configure_io(args.io_mode, args.io_batch_size,
                     args.io_depth);  // 记录 requested/effective I/O。
  const auto& io_status = index.io_status();
  if (!args.allow_io_fallback && args.io_mode != "pread") {
    if (io_status.effective_mode != args.io_mode) {
      throw std::runtime_error(
          "Requested --io-mode " + args.io_mode + " fell back to " +
          io_status.effective_mode +
          "; use --allow-io-fallback only for compatibility runs");
    }
    if (args.io_mode == "odirect" && !io_status.direct_enabled) {
      throw std::runtime_error("Requested O_DIRECT but direct I/O is inactive");
    }
    if (args.io_mode == "io_uring" && !io_status.io_uring_enabled) {
      throw std::runtime_error("Requested io_uring but io_uring is inactive");
    }
  }

  std::cout << "graph_index_path=" << args.index_path << "\n";
  std::cout << "run_type=" << args.run_type << "\n";
  std::cout << "warmup_runs=" << args.warmup_runs << "\n";
  std::cout << "layout=" << args.layout << "\n";
  std::cout << "cache_policy=" << args.cache_policy << "\n";
  std::cout << "page_cache_policy=" << args.cache_policy << "\n";
  std::cout << "cache_pages=" << args.cache_pages << "\n";
  std::cout << "cache_budget_bytes=" << args.cache_budget_bytes << "\n";
  std::cout << "page_cache_capacity_pages="
            << effective_cache_capacity_pages(args, index.metadata()) << "\n";
  std::cout << "cache_protect_hot_pages="
            << (args.cache_protect_hot_pages ? 1 : 0) << "\n";
  std::cout << "cache_hot_degree_threshold="
            << args.cache_hot_degree_threshold << "\n";
  std::cout << "path_cache_policy=" << args.path_cache_policy << "\n";
  std::cout << "path_cache_capacity=" << args.path_cache_capacity << "\n";
  std::cout << "path_cache_hit_search_width="
            << args.path_cache_hit_search_width << "\n";
  std::cout << "query_signature_policy=" << args.query_signature_policy << "\n";
  std::cout << "agent_workload_mode=" << args.agent_workload_mode << "\n";
  std::cout << "simhash_bits=" << args.simhash_bits << "\n";
  std::cout << "pq_prefix_subspaces=" << args.pq_prefix_subspaces << "\n";
  std::cout << "pq_prefix_centroids=" << args.pq_prefix_centroids << "\n";
  std::cout << "pq_prefix_train_iterations="
            << args.pq_prefix_train_iterations << "\n";
  std::cout << "workload_mode=" << args.workload_mode << "\n";
  std::cout << "operation_count=" << args.operation_count << "\n";
  std::cout << "write_ratio_percent=" << args.write_ratio_percent << "\n";
  std::cout << "delete_ratio_percent=" << args.delete_ratio_percent << "\n";
  std::cout << "wal_path=" << args.wal_path << "\n";
  std::cout << "wal_replay=" << (args.wal_replay ? 1 : 0) << "\n";
  std::cout << "delta_index_policy=" << args.delta_index_policy << "\n";
  std::cout << "delta_ivf_centroids=" << args.delta_ivf_centroids << "\n";
  std::cout << "delta_ivf_probes=" << args.delta_ivf_probes << "\n";
  std::cout << "delta_ivf_train_iterations="
            << args.delta_ivf_train_iterations << "\n";
  std::cout << "delta_ivf_rebuild_interval="
            << args.delta_ivf_rebuild_interval << "\n";
  std::cout << "compaction_policy=" << args.compaction_policy << "\n";
  std::cout << "stream_merge_index_path=" << args.stream_merge_index_path
            << "\n";
  std::cout << "delta_compaction_threshold="
            << args.delta_compaction_threshold << "\n";
  std::cout << "compaction_batch_size=" << args.compaction_batch_size << "\n";
  std::cout << "compaction_work_us=" << args.compaction_work_us << "\n";
  std::cout << "compaction_io_mode=" << args.compaction_io_mode << "\n";
  std::cout << "compaction_io_path=" << args.compaction_io_path << "\n";
  std::cout << "compaction_io_bytes_per_vector="
            << args.compaction_io_bytes_per_vector << "\n";
  std::cout << "sla_p99_ms=" << args.sla_p99_ms << "\n";
  std::cout << "packing_strategy="
            << (args.layout == "packed" ? args.packing_strategy : "none")
            << "\n";
  std::cout << "graph_vector_count=" << index.metadata().vector_count << "\n";
  std::cout << "graph_degree=" << index.metadata().degree << "\n";
  std::cout << "graph_build_policy=" << args.graph_build_policy << "\n";
  std::cout << "approx_projections=" << args.approx_projections << "\n";
  std::cout << "approx_window=" << args.approx_window << "\n";
  std::cout << "approx_random_samples=" << args.approx_random_samples << "\n";
  std::cout << "approx_candidate_limit=" << args.approx_candidate_limit << "\n";
  std::cout << "candidate_limit=" << args.approx_candidate_limit << "\n";
  std::cout << "lsh_tables=" << args.lsh_tables << "\n";
  std::cout << "lsh_bits=" << args.lsh_bits << "\n";
  std::cout << "lsh_probe_radius=" << args.lsh_probe_radius << "\n";
  std::cout << "lsh_bucket_limit=" << args.lsh_bucket_limit << "\n";
  std::cout << "robust_prune_alpha=" << args.robust_prune_alpha << "\n";
  std::cout << "reverse_edge_patch=" << (args.reverse_edge_patch ? 1 : 0)
            << "\n";
  std::cout << "prune_passes=" << args.prune_passes << "\n";
  std::cout << "graph_page_size=" << index.metadata().page_size << "\n";
  std::cout << "graph_page_count=" << index.metadata().page_count << "\n";
  std::cout << "graph_nodes_per_page=" << index.metadata().nodes_per_page
            << "\n";
  std::cout << "search_width=" << args.search_width << "\n";
  std::cout << "search_early_stop=" << (args.search_early_stop ? 1 : 0)
            << "\n";
  std::cout << "search_early_stop_min_expansions="
            << args.search_early_stop_min_expansions << "\n";
  std::cout << "adaptive_early_stop=" << (args.adaptive_early_stop ? 1 : 0)
            << "\n";
  std::cout << "early_stop_patience=" << args.early_stop_patience << "\n";
  std::cout << "early_stop_eps=" << args.early_stop_eps << "\n";
  std::cout << "min_expansions=" << args.min_expansions << "\n";
  std::cout << "max_expansions=" << args.max_expansions << "\n";
  std::cout << "entry_count=" << args.entry_count << "\n";
  std::cout << "routing_sample_count=" << args.routing_sample_count << "\n";
  std::cout << "hotpath_train_queries=" << args.hotpath_train_queries << "\n";
  std::cout << "io_mode=" << io_status.requested_mode << "\n";
  std::cout << "io_mode_effective=" << io_status.effective_mode << "\n";
  std::cout << "io_direct_enabled=" << (io_status.direct_enabled ? 1 : 0)
            << "\n";
  std::cout << "io_uring_enabled=" << (io_status.io_uring_enabled ? 1 : 0)
            << "\n";
  std::cout << "io_batch_size=" << io_status.batch_size << "\n";
  std::cout << "io_depth=" << io_status.depth << "\n";
  std::cout << "prefetch_width=" << args.prefetch_width << "\n";
  std::cout << "prefetch_policy=" << args.prefetch_policy << "\n";
  std::cout << "page_dedup=" << (args.page_dedup ? 1 : 0) << "\n";
  std::cout << "same_page_reuse=" << (args.same_page_reuse ? 1 : 0)
            << "\n";
  std::cout << "page_coalesce=" << (args.page_coalesce ? 1 : 0) << "\n";
  if (!io_status.fallback_reason.empty()) {
    std::cout << "io_fallback_reason=" << io_status.fallback_reason << "\n";
  }
  std::cout << "index_size_bytes=" << file_size_bytes(args.index_path) << "\n";

  std::unique_ptr<agentmem::PqAdcModel> pq_model;
  double pq_train_seconds = 0.0;
  if (args.pq_enable) {
    pq_model = std::make_unique<agentmem::PqAdcModel>();
    const auto pq_start = std::chrono::steady_clock::now();
    pq_model->train(base, args.pq_m, args.pq_ks, args.pq_train_limit,
                    args.pq_train_iterations, args.synthetic_config.seed);
    const auto pq_end = std::chrono::steady_clock::now();
    pq_train_seconds =
        std::chrono::duration<double>(pq_end - pq_start).count();
  }
  std::cout << "pq_enable=" << (args.pq_enable ? 1 : 0) << "\n";
  std::cout << "pq_m=" << args.pq_m << "\n";
  std::cout << "pq_ks=" << args.pq_ks << "\n";
  std::cout << "pq_code_bytes_per_vector="
            << (pq_model ? pq_model->subspaces() : 0) << "\n";
  std::cout << "pq_train_seconds=" << pq_train_seconds << "\n";
  std::cout << "pq_code_path=" << args.pq_code_path << "\n";
  std::cout << "pq_code_path_used=0\n";
  std::cout << "adc_enable=" << (args.adc_enable ? 1 : 0) << "\n";
  std::cout << "rerank_topk=" << args.rerank_topk << "\n";

  QueryPathCache path_cache(args.path_cache_policy, args.path_cache_capacity);
  ResidentQueryRouter router(args, base);
  const bool raw_base_released =
      args.release_raw_base_after_prepare && args.workload_mode == "read-only";
  if (raw_base_released) {
    std::vector<float>().swap(base.values);
  }
  std::cout << "raw_base_released_before_search="
            << (raw_base_released ? 1 : 0) << "\n";
  for (std::size_t i = 0; i < args.warmup_runs; ++i) {
    (void)execute_graph_queries(index, args, queries, false, path_cache,
                                router, pq_model.get());
  }
  RunOutput output;
  if (args.workload_mode == "mixed") {
    output = run_mixed_graph_with_index(index, args, base, queries, path_cache,
                                        router, pq_model.get());
  } else {
    output = execute_graph_queries(index, args, queries, true, path_cache,
                                   router, pq_model.get());
  }
  output.pq_train_seconds = pq_train_seconds;
  output.memory =
      build_graph_memory_report(args, index, pq_model.get(), path_cache,
                                router, output);
  return output;
}

RunOutput run_graph(const Args& args, agentmem::VectorSet& base,
                    const agentmem::VectorSet& queries) {
  if (args.build_index) {
    agentmem::DiskGraphBuildConfig build_config;
    agentmem::DiskGraphBuildStats build_stats;
    build_config.degree = args.graph_degree;
    build_config.page_size = args.page_size;
    build_config.build_policy = args.graph_build_policy;
    build_config.packing_strategy = args.packing_strategy;
    build_config.random_seed = args.synthetic_config.seed;
    build_config.approx_projections = args.approx_projections;
    build_config.approx_window = args.approx_window;
    build_config.approx_random_samples = args.approx_random_samples;
    build_config.approx_candidate_limit = args.approx_candidate_limit;
    build_config.reverse_edge_patch = args.reverse_edge_patch;
    build_config.prune_passes = args.prune_passes;
    build_config.lsh_tables = args.lsh_tables;
    build_config.lsh_bits = args.lsh_bits;
    build_config.lsh_probe_radius = args.lsh_probe_radius;
    build_config.lsh_bucket_limit = args.lsh_bucket_limit;
    build_config.robust_prune_alpha = args.robust_prune_alpha;
    build_config.coaccess_sessions = args.coaccess_sessions;
    build_config.coaccess_trace_length = args.coaccess_trace_length;
    build_config.hotpath_queries = &queries;
    build_config.hotpath_train_queries = args.hotpath_train_queries;
    build_config.hotpath_search_width = args.hotpath_search_width;
    build_config.hotpath_entry_count = args.hotpath_entry_count;
    build_config.stats = &build_stats;
    const auto build_start = std::chrono::steady_clock::now();
    if (args.layout == "one-node") {  // V1 与 V2 共享建图逻辑，仅落盘布局不同。
      agentmem::NaiveDiskGraphBuilder::build(base, args.index_path,
                                             build_config);
    } else {
      agentmem::PackedDiskGraphBuilder::build(base, args.index_path,
                                              build_config);
    }
    const auto build_end = std::chrono::steady_clock::now();
    const double build_seconds =
        std::chrono::duration<double>(build_end - build_start).count();
    std::cout << "graph_build_seconds=" << build_seconds << "\n";
    std::cout << "index_build_seconds=" << build_seconds << "\n";
    std::cout << "hotpath_train_queries=" << build_stats.hotpath_train_queries
              << "\n";
    std::cout << "hotpath_unique_visited_nodes="
              << build_stats.hotpath_unique_visited_nodes << "\n";
    std::cout << "hotpath_top_node_visit_count="
              << build_stats.hotpath_top_node_visit_count << "\n";
    std::cout << "reverse_edges_added="
              << build_stats.reverse_edges_added << "\n";
    std::cout << "reverse_prune_count="
              << build_stats.reverse_prune_count << "\n";
    std::cout << "avg_degree=" << build_stats.avg_degree << "\n";
    std::cout << "max_degree=" << build_stats.max_degree << "\n";
    std::cout << "graph_candidate_avg="
              << build_stats.graph_candidate_avg << "\n";
    std::cout << "graph_candidate_p95="
              << build_stats.graph_candidate_p95 << "\n";
    std::cout << "graph_candidate_p99="
              << build_stats.graph_candidate_p99 << "\n";
  }

  if (args.layout == "one-node") {
    agentmem::NaiveDiskGraphIndex index(args.index_path);
    return run_graph_with_index(index, args, base, queries);
  }

  agentmem::PackedDiskGraphIndex index(args.index_path);
  index.configure_cache(args.cache_policy,
                        effective_cache_capacity_pages(args, index.metadata()),
                        args.cache_protect_hot_pages,
                        args.cache_hot_degree_threshold);
  return run_graph_with_index(index, args, base, queries);
}

void print_optional_stats(const std::string& prefix,
                          const std::vector<double>& values) {
  if (values.empty()) {
    return;
  }
  const auto stats = agentmem::summarize_latency(values);
  std::cout << prefix << "_avg=" << stats.avg_ms << "\n";
  std::cout << prefix << "_p95=" << stats.p95_ms << "\n";
  std::cout << prefix << "_p99=" << stats.p99_ms << "\n";
}

double sum_values(const std::vector<double>& values) {
  double sum = 0.0;
  for (const double value : values) {
    sum += value;
  }
  return sum;
}

void print_memory_report(const Args& args, const MemoryReport& report) {
  std::cout << "memory_budget_ratio=" << report.budget_ratio << "\n";
  std::cout << "memory_budget_bytes=" << report.budget_bytes << "\n";
  std::cout << "memory_budget_bytes_user=" << args.memory_budget_bytes << "\n";
  std::cout << "memory_budget_enforced="
            << (args.enforce_memory_budget ? 1 : 0) << "\n";
  std::cout << "memory_over_budget_allowed="
            << (report.over_budget_allowed ? 1 : 0) << "\n";
  std::cout << "memory_resident_bytes=" << report.resident_bytes << "\n";
  std::cout << "memory_resident_ratio=" << report.resident_ratio << "\n";
  std::cout << "memory_budget_pass=" << (report.budget_pass ? 1 : 0) << "\n";
  std::cout << "memory_mode=" << (report.memory_mode ? 1 : 0) << "\n";
  std::cout << "memory_accounting_scope=engine_resident\n";
  std::cout << "memory_bytes_raw_vectors=" << report.raw_vector_bytes << "\n";
  std::cout << "memory_bytes_raw_vectors_resident="
            << report.bytes_raw_vectors_resident << "\n";
  std::cout << "memory_bytes_pq_codes=" << report.bytes_pq_codes << "\n";
  std::cout << "memory_bytes_pq_codebooks="
            << report.bytes_pq_codebooks << "\n";
  std::cout << "memory_bytes_graph_metadata="
            << report.bytes_graph_metadata << "\n";
  std::cout << "memory_bytes_cache=" << report.bytes_cache << "\n";
  std::cout << "page_cache_bytes=" << report.bytes_cache << "\n";
  std::cout << "memory_bytes_path_cache=" << report.bytes_path_cache << "\n";
  std::cout << "memory_bytes_router=" << report.bytes_router << "\n";
  std::cout << "memory_bytes_query_workspace="
            << report.bytes_query_workspace << "\n";
  std::cout << "memory_bytes_io_buffers=" << report.bytes_io_buffers << "\n";
  std::cout << "memory_bytes_delta=" << report.bytes_delta << "\n";
  std::cout << "memory_bytes_tombstone=" << report.bytes_tombstone << "\n";
  std::cout << "memory_bytes_temporary_peak="
            << report.bytes_temporary_peak << "\n";
  std::cout << "memory_compression_ratio="
            << (report.bytes_pq_codes == 0
                    ? 0.0
                    : static_cast<double>(report.raw_vector_bytes) /
                          static_cast<double>(report.bytes_pq_codes))
            << "\n";
}

struct TruthValidation {
  bool has_out_of_range_ids = false;
  std::size_t checked_ids = 0;
  std::size_t out_of_range_ids = 0;
  std::uint32_t max_checked_id = 0;
};

TruthValidation validate_truth_ids(
    const std::vector<std::vector<std::uint32_t>>& truth, std::size_t k,
    std::size_t base_count) {
  TruthValidation validation;
  for (const auto& row : truth) {
    const std::size_t truth_k = std::min(k, row.size());
    for (std::size_t i = 0; i < truth_k; ++i) {
      const std::uint32_t id = row[i];
      ++validation.checked_ids;
      validation.max_checked_id = std::max(validation.max_checked_id, id);
      if (static_cast<std::size_t>(id) >= base_count) {  // 官方 truth 可能针对完整 SIFT1M。
        validation.has_out_of_range_ids = true;
        ++validation.out_of_range_ids;
      }
    }
  }
  return validation;
}

}  // namespace

int agentmem::benchmark::run_cli(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);

    agentmem::VectorSet base;
    agentmem::VectorSet queries;
    std::vector<std::vector<std::uint32_t>> truth;
    bool truth_from_file = false;

    agentmem::DatasetLoadConfig load_config;
    load_config.synthetic = args.synthetic;
    load_config.paths.base = args.base_path;
    load_config.paths.query = args.query_path;
    load_config.paths.truth = args.truth_path;
    load_config.paths.sift_dir = args.sift_dir;
    load_config.base_limit = args.base_limit;
    load_config.query_limit = args.query_limit;
    load_config.synthetic_config = args.synthetic_config;
    auto loaded = agentmem::load_dataset(load_config);
    base = std::move(loaded.base);
    queries = std::move(loaded.queries);
    truth = std::move(loaded.truth);
    truth_from_file = loaded.truth_from_file;

    if (args.synthetic) {
      std::cout << "mode=synthetic\n";
      std::cout << "synthetic_workload=" << args.synthetic_config.workload << "\n";
      std::cout << "session_length=" << args.synthetic_config.session_length << "\n";
    } else {
      std::cout << "mode=fvecs\n";
      std::cout << "base_path=" << args.base_path << "\n";
      std::cout << "query_path=" << args.query_path << "\n";
      if (!args.truth_path.empty()) {
        std::cout << "truth_path=" << args.truth_path << "\n";
      }
    }

    apply_agent_workload(args, queries, truth, truth_from_file);

    if (base.dim != queries.dim) {
      throw std::runtime_error("Base and query dimensions do not match");
    }

    TruthValidation truth_validation;
    bool truth_validation_ready = false;
    if (truth_from_file) {
      if (truth.size() != queries.size()) {
        throw std::runtime_error(
            "Ground-truth query count does not match loaded queries");
      }
      truth_validation = validate_truth_ids(truth, args.k, base.size());
      truth_validation_ready = true;
    }

    Args run_args = args;
    run_args.release_raw_base_after_prepare =
        args.engine == "graph" && args.workload_mode == "read-only" &&
        truth_validation_ready && !truth_validation.has_out_of_range_ids;

    std::cout << "engine=" << args.engine << "\n";
    std::cout << "search_path="
              << (is_memory_engine(args.engine) ? "search_memory_fast"
                                                : "diskann_search_enhanced")
              << "\n";
    std::cout << "agent_workload_mode=" << args.agent_workload_mode << "\n";
    std::cout << "base_count=" << base.size() << "\n";
    std::cout << "query_count=" << queries.size() << "\n";
    std::cout << "dim=" << base.dim << "\n";
    std::cout << "k=" << args.k << "\n";

    RunOutput output;
    if (is_memory_engine(args.engine)) {
      output = run_memory_search(base, queries, args.k);
      output.memory = build_exact_memory_report(args, base);
    } else {
      output = run_graph(run_args, base, queries);
    }

    if (!output.dynamic_truth.empty()) {  // 混合负载必须使用包含 Delta 的动态 truth。
      truth = output.dynamic_truth;
      std::cout << "truth=dynamic_exact_bruteforce\n";
      std::cout << "truth_source=dynamic_exact_bruteforce\n";
    } else if (truth_from_file) {
      std::cout << "truth_file_rows=" << truth.size() << "\n";
      std::cout << "truth_file_checked_ids=" << truth_validation.checked_ids << "\n";
      std::cout << "truth_file_max_checked_id="
                << truth_validation.max_checked_id << "\n";
      std::cout << "truth_file_out_of_range_ids="
                << truth_validation.out_of_range_ids << "\n";
      std::cout << "truth_file_usable="
                << (truth_validation.has_out_of_range_ids ? 0 : 1) << "\n";
      if (truth_validation.has_out_of_range_ids) {
        std::cerr
            << "warning: ground-truth ids exceed loaded base_count; "
            << "recomputing exact truth for the current base subset.\n";
        if (is_memory_engine(args.engine)) {
          truth = results_as_truth(output.results);
          std::cout << "truth=file_out_of_range_exact_self\n";
          std::cout << "truth_source=subset_recompute_exact_self\n";
        } else {
          truth = exact_truth(base, queries, args.k);
          std::cout << "truth=file_out_of_range_exact_bruteforce\n";
          std::cout << "truth_source=subset_recompute_exact_bruteforce\n";
        }
      } else {
        std::cout << "truth=file\n";
        std::cout << "truth_source=official_file\n";
      }
    } else if (truth.empty()) {
      if (is_memory_engine(args.engine)) {
        truth = results_as_truth(output.results);
        std::cout << "truth=exact_self\n";
        std::cout << "truth_source=generated_exact_self\n";
      } else {
        truth = exact_truth(base, queries, args.k);
        std::cout << "truth=exact_bruteforce\n";
        std::cout << "truth_source=generated_exact_bruteforce\n";
      }
    } else {
      std::cout << "truth=file\n";
      std::cout << "truth_source=official_file\n";
    }

    const auto latency = agentmem::summarize_latency(output.latencies_ms);
    const double recall = agentmem::recall_at_k(output.results, truth, args.k);
    const double one_minus_recall = 1.0 - recall;
    const std::size_t measured_queries = output.results.size();

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "recall_at_" << args.k << "=" << recall << "\n";
    std::cout << "one_minus_recall_at_" << args.k << "=" << one_minus_recall
              << "\n";
    std::cout << "qps="
              << agentmem::queries_per_second(measured_queries,
                                              output.elapsed_seconds)
              << "\n";
    if (output.operation_count > 0) {
      std::cout << "ops_per_second="
                << agentmem::queries_per_second(output.operation_count,
                                                output.elapsed_seconds)
                << "\n";
      std::cout << "measured_queries=" << measured_queries << "\n";
      std::cout << "insert_count=" << output.insert_count << "\n";
      std::cout << "delete_count=" << output.delete_count << "\n";
      std::cout << "tombstone_count=" << output.tombstone_count << "\n";
    }
    std::cout << "avg_latency_ms=" << latency.avg_ms << "\n";
    std::cout << "p50_latency_ms=" << latency.p50_ms << "\n";
    std::cout << "p95_latency_ms=" << latency.p95_ms << "\n";
    std::cout << "p99_latency_ms=" << latency.p99_ms << "\n";
    print_memory_report(args, output.memory);
    print_optional_stats("ssd_reads_per_query", output.ssd_reads);
    print_optional_stats("page_cache_requests_per_query",
                         output.cache_requests);
    print_optional_stats("page_cache_hits_per_query", output.cache_hits);
    print_optional_stats("page_cache_misses_per_query", output.cache_misses);
    print_optional_stats("page_cache_evictions_per_query",
                         output.cache_evictions);
    print_optional_stats("page_cache_promotions_per_query",
                         output.cache_promotions);
    print_optional_stats("path_cache_requests_per_query",
                         output.path_cache_requests);
    print_optional_stats("path_cache_hits_per_query", output.path_cache_hits);
    print_optional_stats("graph_expanded_per_query", output.expanded);
    print_optional_stats("graph_visited_per_query", output.visited);
    print_optional_stats("io_submit_count_per_query", output.io_submits);
    print_optional_stats("io_complete_count_per_query", output.io_completions);
    print_optional_stats("io_submit_syscall_count_per_query",
                         output.io_submit_syscalls);
    print_optional_stats("io_prefetch_count_per_query", output.io_prefetches);
    print_optional_stats("io_prefetch_hit_count_per_query",
                         output.io_prefetch_hits);
    print_optional_stats("io_prefetch_wait_count_per_query",
                         output.io_prefetch_waits);
    print_optional_stats("io_pending_pages_peak_per_query",
                         output.io_pending_pages_peak);
    print_optional_stats("prefetch_submitted_pages_per_query",
                         output.prefetch_submitted_pages);
    print_optional_stats("prefetch_useful_pages_per_query",
                         output.prefetch_useful_pages);
    print_optional_stats("prefetch_wasted_pages_per_query",
                         output.prefetch_wasted_pages);
    print_optional_stats("demand_read_wait_count_per_query",
                         output.demand_read_waits);
    print_optional_stats("demand_read_wait_us_per_query",
                         output.demand_read_wait_us);
    print_optional_stats("page_requests_before_dedup_per_query",
                         output.page_requests_before_dedup);
    print_optional_stats("page_requests_after_dedup_per_query",
                         output.page_requests_after_dedup);
    print_optional_stats("same_page_node_reuse_per_query",
                         output.same_page_node_reuse);
    const double total_io_submits = sum_values(output.io_submits);
    const double total_io_submit_syscalls =
        sum_values(output.io_submit_syscalls);
    if (total_io_submit_syscalls > 0.0) {
      std::cout << "io_batch_size_avg="
                << (total_io_submits / total_io_submit_syscalls) << "\n";
    }
    const double total_prefetch_submitted =
        sum_values(output.prefetch_submitted_pages);
    std::cout << "prefetch_hit_rate="
              << (total_prefetch_submitted > 0.0
                      ? sum_values(output.prefetch_useful_pages) /
                            total_prefetch_submitted
                      : 0.0)
              << "\n";
    if (!output.page_dedup_ratios.empty()) {
      std::cout << "page_dedup_ratio="
                << (sum_values(output.page_dedup_ratios) /
                    static_cast<double>(output.page_dedup_ratios.size()))
                << "\n";
    }
    print_optional_stats("adc_table_build_us", output.adc_table_build_us);
    print_optional_stats("rerank_reads_per_query", output.rerank_reads);
    print_optional_stats("pq_filter_reject_count_per_query",
                         output.pq_filter_reject_count);
    print_optional_stats("pq_filter_accept_count_per_query",
                         output.pq_filter_accept_count);
    std::cout << "page_cache_evictions="
              << sum_values(output.cache_evictions) << "\n";
    std::cout << "page_cache_promotions="
              << sum_values(output.cache_promotions) << "\n";
    print_optional_stats("insert_latency_ms", output.insert_latencies_ms);
    print_optional_stats("query_compaction_ms", output.query_compaction_ms);
    print_optional_stats("delta_search_ms", output.delta_search_ms);
    print_optional_stats("delta_exact_search_ms", output.delta_exact_search_ms);
    if (!output.delta_recalls.empty()) {
      std::cout << "delta_recall_at_" << args.k << "="
                << (sum_values(output.delta_recalls) /
                    static_cast<double>(output.delta_recalls.size()))
                << "\n";
      std::cout << "delta_recall_at_" << args.k << "_min="
                << *std::min_element(output.delta_recalls.begin(),
                                     output.delta_recalls.end())
                << "\n";
    }
    if (!output.ssd_reads.empty()) {
      const auto read_stats = agentmem::summarize_latency(output.ssd_reads);
      std::cout << "io_amplification_reads_per_result="
                << (read_stats.avg_ms / static_cast<double>(args.k)) << "\n";
    }
    const double total_cache_requests = sum_values(output.cache_requests);
    std::cout << "page_cache_hit_rate="
              << (total_cache_requests > 0.0
                      ? sum_values(output.cache_hits) / total_cache_requests
                      : 0.0)
              << "\n";
    const double total_path_requests = sum_values(output.path_cache_requests);
    std::cout << "path_cache_hit_rate="
              << (total_path_requests > 0.0
                      ? sum_values(output.path_cache_hits) / total_path_requests
                      : 0.0)
              << "\n";
    if (output.operation_count > 0) {
      std::cout << "wal_replay_records=" << output.wal_replay_records << "\n";
      std::cout << "wal_replay_inserts=" << output.wal_replay_inserts << "\n";
      std::cout << "wal_replay_deletes=" << output.wal_replay_deletes << "\n";
      std::cout << "wal_replay_bytes=" << output.wal_replay_bytes << "\n";
      std::cout << "wal_replay_delta_size="
                << output.wal_replay_delta_size << "\n";
      std::cout << "delta_active_size=" << output.delta_active_size << "\n";
      std::cout << "delta_sealed_size=" << output.delta_sealed_size << "\n";
      std::cout << "wal_records=" << output.wal_records << "\n";
      std::cout << "wal_bytes=" << output.wal_bytes << "\n";
      std::cout << "compaction_ops=" << output.compaction_ops << "\n";
      std::cout << "compaction_vectors=" << output.compaction_vectors << "\n";
      std::cout << "compaction_io_bytes="
                << output.compaction_io_bytes << "\n";
      std::cout << "compaction_seconds=" << output.compaction_seconds << "\n";
      std::cout << "compaction_skipped_sla="
                << output.compaction_skipped_sla << "\n";
      std::cout << "queries_with_compaction="
                << output.queries_with_compaction << "\n";
      std::cout << "stream_merge_ops=" << output.stream_merge_ops << "\n";
      std::cout << "stream_merge_vectors=" << output.stream_merge_vectors
                << "\n";
      std::cout << "stream_merge_inserted=" << output.stream_merge_inserted
                << "\n";
      std::cout << "stream_merge_deleted=" << output.stream_merge_deleted
                << "\n";
      std::cout << "stream_merge_seconds=" << output.stream_merge_seconds
                << "\n";
      std::cout << "compaction_work_ms_per_query="
                << (measured_queries == 0
                        ? 0.0
                        : (output.compaction_seconds * 1000.0) /
                              static_cast<double>(measured_queries))
                << "\n";
      std::cout << "compaction_interference_ms_per_query="
                << (measured_queries == 0
                        ? 0.0
                        : sum_values(output.query_compaction_ms) /
                              static_cast<double>(measured_queries))
                << "\n";
    }
    std::cout << "elapsed_seconds=" << output.elapsed_seconds << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
