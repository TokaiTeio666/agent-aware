#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace agent_aware {

struct VectorSet {
  std::size_t dim = 0;
  std::vector<float> values;

  VectorSet() = default;

  VectorSet(std::size_t count, std::size_t dimension)
      : dim(dimension), values(count * dimension, 0.0f) {}

  std::size_t size() const {
    return dim == 0 ? 0 : values.size() / dim;
  }

  bool empty() const {
    return size() == 0;
  }

  const float* row(std::size_t index) const {
    if (index >= size()) {
      throw std::out_of_range("VectorSet row index out of range");
    }
    return values.data() + index * dim;
  }

  float* mutable_row(std::size_t index) {
    if (index >= size()) {
      throw std::out_of_range("VectorSet row index out of range");
    }
    return values.data() + index * dim;
  }
};

struct SearchResult {
  std::uint32_t id = 0;
  float distance = 0.0f;
};

struct SyntheticConfig {
  std::size_t base_count = 10000;
  std::size_t query_count = 1000;
  std::size_t dim = 128;
  std::size_t clusters = 64;
  std::string workload = "random";
  std::size_t session_length = 8;
  std::uint32_t seed = 42;
};

struct SyntheticData {
  VectorSet base;
  VectorSet queries;
};

}  // namespace agent_aware
