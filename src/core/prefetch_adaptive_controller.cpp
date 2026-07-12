#include "core/prefetch_adaptive_controller.h"

#include <algorithm>

#include "graph/disk_graph_index.h"

namespace agent_aware {

PrefetchAdaptiveController::Decision PrefetchAdaptiveController::decide(
    const std::string& trigger, bool enabled, std::size_t configured_top_k,
    DiskGraphSearchStats& stats) {
  std::lock_guard<std::mutex> lock(mutex_);
  Decision decision;
  decision.top_k = configured_top_k;
  if (!enabled || configured_top_k == 0) {
    stats.prefetch_adaptive_final_top_k = decision.top_k;
    return decision;
  }
  auto& bucket = buckets_[trigger];
  if (bucket.pause_remaining > 0) {
    --bucket.pause_remaining;
    decision.top_k = 0;
    decision.paused = true;
  } else if (bucket.samples < 32) {
    decision.top_k = std::min<std::size_t>(1, configured_top_k);
  } else {
    decision.top_k = std::min(bucket.top_k, configured_top_k);
    decision.required_lead = bucket.lead;
  }
  stats.prefetch_adaptive_final_top_k = decision.top_k;
  stats.prefetch_adaptive_final_lead = decision.required_lead;
  return decision;
}

void PrefetchAdaptiveController::record_feedback(
    const std::string& trigger, bool enabled, const std::string& outcome,
    double io_latency_us, DiskGraphSearchStats& stats) {
  std::lock_guard<std::mutex> lock(mutex_);
  (void)io_latency_us;
  if (!enabled) return;
  auto& bucket = buckets_[trigger];
  ++bucket.samples;
  ++bucket.window_samples;
  bucket.window_ready += outcome == "ready_hit" ? 1 : 0;
  bucket.window_pending += outcome == "pending_hit" ? 1 : 0;
  bucket.window_wasted += outcome == "wasted" ? 1 : 0;
  if (bucket.window_samples < 32) return;

  const double samples = static_cast<double>(bucket.window_samples);
  const std::size_t useful_count = bucket.window_ready + bucket.window_pending;
  const double useful = static_cast<double>(useful_count) / samples;
  const double waste = static_cast<double>(bucket.window_wasted) / samples;
  const double pending_share = useful_count == 0
      ? 0.0
      : static_cast<double>(bucket.window_pending) / useful_count;
  const double ready = static_cast<double>(bucket.window_ready) / samples;
  if (useful < 0.60 || waste > 0.25) {
    bucket.pause_remaining = 8;
    bucket.top_k = 1;
    bucket.lead = 1;
    ++stats.prefetch_adaptive_pause_count;
  } else if (pending_share > 0.50 && useful >= 0.70) {
    bucket.top_k = 1;
    bucket.lead = 2;
  } else if (ready >= 0.50 && waste <= 0.15) {
    bucket.top_k = 2;
    bucket.lead = 1;
  } else {
    bucket.top_k = 1;
    bucket.lead = 1;
  }
  bucket.window_samples = bucket.window_ready = bucket.window_pending =
      bucket.window_wasted = 0;
}

}  // namespace agent_aware
