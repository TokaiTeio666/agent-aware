#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/types.h"

namespace agent_aware {

struct CandidatePoolMergeStats {
  std::size_t admitted = 0;
  std::size_t replacements = 0;
  std::size_t rejected = 0;
};

inline bool candidate_is_better(const SearchResult& lhs,
                                const SearchResult& rhs) {
  return lhs.distance < rhs.distance ||
         (lhs.distance == rhs.distance && lhs.id < rhs.id);
}

class TopLCandidatePool {
 public:
  explicit TopLCandidatePool(std::size_t capacity) : capacity_(capacity) {
    pool_.reserve(capacity_);
    scratch_.reserve(capacity_);
  }

  CandidatePoolMergeStats merge(std::vector<SearchResult>& staging) {
    std::sort(staging.begin(), staging.end(), candidate_is_better);
    const std::size_t old_size = pool_.size();
    scratch_.clear();

    std::size_t pool_index = 0;
    std::size_t staging_index = 0;
    std::size_t staged_selected = 0;
    while (scratch_.size() < capacity_ &&
           (pool_index < pool_.size() || staging_index < staging.size())) {
      const bool take_staging =
          staging_index < staging.size() &&
          (pool_index == pool_.size() ||
           candidate_is_better(staging[staging_index], pool_[pool_index]));
      if (take_staging) {
        scratch_.push_back(staging[staging_index++]);
        ++staged_selected;
      } else {
        scratch_.push_back(pool_[pool_index++]);
      }
    }

    CandidatePoolMergeStats stats;
    const std::size_t free_slots =
        capacity_ > old_size ? capacity_ - old_size : 0;
    stats.admitted = std::min(staged_selected, free_slots);
    stats.replacements = staged_selected - stats.admitted;
    stats.rejected = staging.size() - staged_selected;
    pool_.swap(scratch_);
    staging.clear();
    return stats;
  }

  const std::vector<SearchResult>& values() const { return pool_; }
  std::size_t size() const { return pool_.size(); }
  std::size_t capacity() const { return capacity_; }
  bool empty() const { return pool_.empty(); }

 private:
  std::size_t capacity_ = 0;
  std::vector<SearchResult> pool_;
  std::vector<SearchResult> scratch_;
};

}  // namespace agent_aware
