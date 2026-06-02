#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "agentmem/types.h"

namespace agentmem {

struct WalStats {
  std::size_t records = 0;
  std::size_t bytes = 0;
};

struct WalReplayStats {
  std::size_t records = 0;
  std::size_t inserts = 0;
  std::size_t deletes = 0;
  std::size_t bytes = 0;
  std::uint32_t dim = 0;
  std::uint32_t max_id = 0;
  bool has_records = false;
};

class WalWriter {
 public:
  WalWriter(const std::string& path, std::size_t dim);
  WalWriter(const std::string& path, std::size_t dim, bool append,
            const WalStats& initial_stats);

  void append_insert(std::uint32_t id, const float* vector);
  void append_delete(std::uint32_t id);
  void flush();

  const WalStats& stats() const {
    return stats_;
  }

 private:
  std::string path_;
  std::size_t dim_ = 0;
  std::ofstream output_;
  WalStats stats_;
};

WalReplayStats replay_wal_inserts(
    const std::string& path, std::size_t expected_dim,
    const std::function<void(std::uint32_t, const float*)>& on_insert);

WalReplayStats replay_wal_records(
    const std::string& path, std::size_t expected_dim,
    const std::function<void(std::uint32_t, const float*)>& on_insert,
    const std::function<void(std::uint32_t)>& on_delete);

class DeltaFlatIndex {
 public:
  explicit DeltaFlatIndex(std::size_t dim);

  void insert(std::uint32_t id, const float* vector);
  std::size_t compact_active_to_sealed(std::size_t max_vectors);

  std::vector<SearchResult> search_one(const float* query,
                                       std::size_t k) const;

  std::size_t active_size() const {
    return active_ids_.size();
  }

  std::size_t sealed_size() const {
    return sealed_ids_.size();
  }

  std::size_t total_size() const {
    return active_size() + sealed_size();
  }

 private:
  std::vector<SearchResult> search_block(const std::vector<std::uint32_t>& ids,
                                         const std::vector<float>& values,
                                         const float* query,
                                         std::size_t k) const;

  std::size_t dim_ = 0;
  std::vector<std::uint32_t> active_ids_;
  std::vector<float> active_values_;
  std::vector<std::uint32_t> sealed_ids_;
  std::vector<float> sealed_values_;
};

class DeltaIvfFlatIndex {
 public:
  DeltaIvfFlatIndex(std::size_t dim, std::size_t centroid_count,
                    std::size_t probe_count, std::size_t train_iterations,
                    std::size_t rebuild_interval);

  void insert(std::uint32_t id, const float* vector);
  std::vector<SearchResult> search_one(const float* query,
                                       std::size_t k) const;

  std::size_t size() const {
    return size_;
  }

  std::size_t centroid_count() const {
    return centroid_count_;
  }

  std::size_t probe_count() const {
    return probe_count_;
  }

 private:
  std::size_t nearest_centroid(const float* vector) const;
  std::vector<std::size_t> closest_centroids(const float* query) const;
  void rebuild();
  void initialize_centroids(std::size_t effective_centroids);
  std::size_t nearest_centroid_in_range(const float* vector,
                                        std::size_t centroid_count) const;
  const float* stored_vector(std::size_t index) const;

  std::size_t dim_ = 0;
  std::size_t centroid_count_ = 0;
  std::size_t probe_count_ = 0;
  std::size_t train_iterations_ = 0;
  std::size_t rebuild_interval_ = 0;
  std::size_t initialized_centroids_ = 0;
  std::size_t size_ = 0;
  std::size_t dirty_since_rebuild_ = 0;
  std::vector<float> centroids_;
  std::vector<std::uint32_t> all_ids_;
  std::vector<float> all_values_;
  std::vector<std::vector<std::uint32_t>> bucket_ids_;
  std::vector<std::vector<float>> bucket_values_;
};

std::vector<SearchResult> merge_topk(const std::vector<SearchResult>& lhs,
                                     const std::vector<SearchResult>& rhs,
                                     std::size_t k);

}  // namespace agentmem
