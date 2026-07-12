#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace agent_aware {

struct DiskGraphSearchStats;

class PrefetchAdaptiveController {
 public:
  struct Decision {
    std::size_t top_k = 1;
    std::size_t required_lead = 1;
    bool paused = false;
  };

  Decision decide(const std::string& trigger, bool enabled,
                  std::size_t configured_top_k,
                  DiskGraphSearchStats& stats);
  void record_feedback(const std::string& trigger, bool enabled,
                       const std::string& outcome, double io_latency_us,
                       DiskGraphSearchStats& stats);

 private:
  struct Bucket {
    std::size_t samples = 0;
    std::size_t window_samples = 0;
    std::size_t window_ready = 0;
    std::size_t window_pending = 0;
    std::size_t window_wasted = 0;
    std::size_t top_k = 1;
    std::size_t lead = 1;
    std::size_t pause_remaining = 0;
  };
  std::unordered_map<std::string, Bucket> buckets_;
  std::mutex mutex_;
};

}  // namespace agent_aware
