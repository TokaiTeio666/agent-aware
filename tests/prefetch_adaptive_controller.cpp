#include <stdexcept>
#include <vector>

#include "core/prefetch_adaptive_controller.h"
#include "graph/disk_graph_index.h"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() {
  using namespace agent_aware;
  DiskGraphSearchStats stats;
  PrefetchAdaptiveController controller;
  auto cold = controller.decide("post_expand", true, 2, stats);
  require(cold.top_k == 1, "cold start must submit at most one page");
  auto fixed = controller.decide("post_expand", false, 2, stats);
  require(fixed.top_k == 2, "disabled adaptation must preserve fixed cap");

  for (int i = 0; i < 32; ++i) {
    controller.record_feedback("post_expand", true, "ready_hit", 100.0, stats);
  }
  auto expanded = controller.decide("post_expand", true, 2, stats);
  require(expanded.top_k == 2, "healthy ready rate must expand to two pages");

  PrefetchAdaptiveController waste_controller;
  for (int i = 0; i < 32; ++i) {
    waste_controller.record_feedback("post_expand", true, "wasted", 100.0,
                                     stats);
  }
  auto paused = waste_controller.decide("post_expand", true, 2, stats);
  require(paused.paused && paused.top_k == 0,
          "high waste must pause prefetch windows");

  const auto pages = rank_topology_pages({9, 4, 9, 3, 4, 2}, 4);
  require(pages.size() == 3, "candidate window or page dedup is incorrect");
  require(pages[0].page_id == 9 && pages[0].min_pq_rank == 0 &&
              pages[0].candidate_count == 2,
          "best PQ rank must dominate page ordering");
  require(pages[1].page_id == 4 && pages[2].page_id == 3,
          "page ordering must be stable after aggregation");
  return 0;
}
