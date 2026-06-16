#pragma once

#include <cstddef>

#include "agentmem/core/brute_force.h"

namespace agentmem {

inline float l2_distance_sq_simd(const float* lhs, const float* rhs,
                                 std::size_t dim) {
  // squared_l2 owns the AVX/SSE2 implementation and scalar tail handling.
  return static_cast<float>(squared_l2(lhs, rhs, dim));
}

}  // namespace agentmem
