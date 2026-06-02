#include "agentmem/dataset.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <stdexcept>

namespace agentmem {
namespace {

std::int32_t read_i32(std::ifstream& input, const std::string& path) {
  std::int32_t value = 0;  // fvecs/ivecs 每条记录都以 int32 维度开头。
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!input) {
    throw std::runtime_error("Failed to read int32 from " + path);
  }
  return value;
}

void validate_positive_dim(std::int32_t dim, const std::string& path) {
  if (dim <= 0 || dim > 1'000'000) {
    throw std::runtime_error("Invalid vector dimension in " + path);
  }
}

}  // namespace

VectorSet load_fvecs(const std::string& path, std::size_t limit) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open fvecs file: " + path);
  }

  VectorSet output;
  std::size_t count = 0;

  while (input.peek() != EOF && (limit == 0 || count < limit)) {
    const std::int32_t dim_i32 = read_i32(input, path);
    validate_positive_dim(dim_i32, path);
    const std::size_t dim = static_cast<std::size_t>(dim_i32);

    if (output.dim == 0) {
      output.dim = dim;
    } else if (output.dim != dim) {
      throw std::runtime_error("Inconsistent vector dimension in " + path);
    }

    const std::size_t old_size = output.values.size();  // VectorSet 按行连续存储。
    output.values.resize(old_size + dim);
    input.read(reinterpret_cast<char*>(output.values.data() + old_size),
               static_cast<std::streamsize>(dim * sizeof(float)));
    if (!input) {
      throw std::runtime_error("Truncated fvecs payload in " + path);
    }
    ++count;
  }

  return output;
}

std::vector<std::vector<std::uint32_t>> load_ivecs(const std::string& path,
                                                   std::size_t limit) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open ivecs file: " + path);
  }

  std::vector<std::vector<std::uint32_t>> output;

  while (input.peek() != EOF && (limit == 0 || output.size() < limit)) {
    const std::int32_t dim_i32 = read_i32(input, path);
    validate_positive_dim(dim_i32, path);
    const std::size_t dim = static_cast<std::size_t>(dim_i32);

    std::vector<std::uint32_t> row(dim);  // ivecs 的 payload 是邻居或 truth id。
    input.read(reinterpret_cast<char*>(row.data()),
               static_cast<std::streamsize>(dim * sizeof(std::uint32_t)));
    if (!input) {
      throw std::runtime_error("Truncated ivecs payload in " + path);
    }
    output.push_back(std::move(row));
  }

  return output;
}

SyntheticData generate_synthetic(const SyntheticConfig& config) {
  if (config.base_count == 0 || config.query_count == 0 || config.dim == 0 ||
      config.clusters == 0) {
    throw std::runtime_error("Synthetic counts, dimension, and clusters must be positive");
  }

  std::mt19937 rng(config.seed);
  std::normal_distribution<float> center_dist(0.0f, 8.0f);   // 拉开簇中心。
  std::normal_distribution<float> point_noise(0.0f, 0.35f);  // base 簇内扰动。
  std::normal_distribution<float> query_noise(0.0f, 0.45f);  // query 稍难于 base。
  std::normal_distribution<float> session_drift(0.0f, 0.12f);  // 会话内轻微漂移。
  std::uniform_int_distribution<std::size_t> cluster_pick(0, config.clusters - 1);

  VectorSet centers(config.clusters, config.dim);
  for (float& value : centers.values) {
    value = center_dist(rng);
  }

  VectorSet base(config.base_count, config.dim);
  for (std::size_t i = 0; i < base.size(); ++i) {
    const std::size_t cluster = i % config.clusters;  // 均匀填充每个簇。
    const float* center = centers.row(cluster);
    float* row = base.mutable_row(i);
    for (std::size_t d = 0; d < config.dim; ++d) {
      row[d] = center[d] + point_noise(rng);
    }
  }

  VectorSet queries(config.query_count, config.dim);
  for (std::size_t i = 0; i < queries.size(); ++i) {
    std::size_t cluster = cluster_pick(rng);
    if (config.workload == "agent") {
      const std::size_t session_length = std::max<std::size_t>(1, config.session_length);
      const std::size_t session = i / session_length;  // 连续 query 复用同一簇。
      cluster = session % config.clusters;
    } else if (config.workload != "random") {
      throw std::runtime_error("Synthetic workload must be random or agent");
    }

    const float* center = centers.row(cluster);
    float* row = queries.mutable_row(i);
    for (std::size_t d = 0; d < config.dim; ++d) {
      float value = center[d] + query_noise(rng);
      if (config.workload == "agent") {
        value += session_drift(rng);
      }
      row[d] = value;
    }
  }

  return SyntheticData{std::move(base), std::move(queries)};
}

}  // namespace agentmem

