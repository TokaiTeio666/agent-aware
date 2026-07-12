#include <cassert>
#include <vector>

#include "core/top_l_candidate_pool.h"

using agent_aware::SearchResult;
using agent_aware::TopLCandidatePool;

int main() {
  TopLCandidatePool pool(4);
  std::vector<SearchResult> initial{{4, 40.0f}, {2, 20.0f},
                                    {3, 30.0f}, {1, 10.0f}};
  const auto inserted = pool.merge(initial);
  assert(inserted.admitted == 4);
  assert(inserted.replacements == 0);
  assert(inserted.rejected == 0);
  assert(pool.values()[0].id == 1);
  assert(pool.values()[3].id == 4);

  std::vector<SearchResult> replacement{{5, 15.0f}, {6, 50.0f}};
  const auto replaced = pool.merge(replacement);
  assert(replaced.admitted == 0);
  assert(replaced.replacements == 1);
  assert(replaced.rejected == 1);
  assert(pool.values()[1].id == 5);
  assert(pool.values()[3].id == 3);

  std::vector<SearchResult> tie{{0, 30.0f}, {7, 30.0f}};
  const auto tied = pool.merge(tie);
  assert(tied.replacements == 1);
  assert(tied.rejected == 1);
  assert(pool.values()[3].id == 0);

  TopLCandidatePool reservoir(8);
  std::vector<SearchResult> candidates;
  for (std::uint32_t id = 0; id < 8; ++id) {
    candidates.push_back(SearchResult{id, static_cast<float>(id)});
  }
  reservoir.merge(candidates);
  const std::size_t logical_l = 4;
  assert(reservoir.values()[logical_l].id == 4);
  assert(reservoir.values().size() == 8);
  return 0;
}
