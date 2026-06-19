#include "agent_aware/data/dataset.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <stdexcept>
#include <utility>

namespace agent_aware {
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

DatasetPaths resolve_sift_paths(const std::string& sift_dir) {
  if (sift_dir.empty()) {
    throw std::runtime_error("SIFT directory path must not be empty");
  }

  DatasetPaths paths;
  paths.sift_dir = sift_dir;
  const bool has_slash =
      sift_dir.back() == '/' || sift_dir.back() == '\\';
  const std::string prefix = has_slash ? sift_dir : sift_dir + "/";
  paths.base = prefix + "sift_base.fvecs";
  paths.query = prefix + "sift_query.fvecs";
  paths.truth = prefix + "sift_groundtruth.ivecs";
  return paths;
}

LoadedDataset load_dataset(const DatasetLoadConfig& config) {
  LoadedDataset loaded;

  if (config.synthetic) {
    auto data = generate_synthetic(config.synthetic_config);
    loaded.base = std::move(data.base);
    loaded.queries = std::move(data.queries);
    loaded.mode = "synthetic";
    loaded.truth_source = "none";
    return loaded;
  }

  DatasetPaths paths = config.paths;
  if (!paths.sift_dir.empty()) {
    const DatasetPaths sift_paths = resolve_sift_paths(paths.sift_dir);
    if (paths.base.empty()) {
      paths.base = sift_paths.base;
    }
    if (paths.query.empty()) {
      paths.query = sift_paths.query;
    }
    if (paths.truth.empty()) {
      paths.truth = sift_paths.truth;
    }
  }

  if (paths.base.empty() || paths.query.empty()) {
    throw std::runtime_error(
        "Dataset loading requires base and query fvecs paths");
  }

  loaded.base = load_fvecs(paths.base, config.base_limit);
  loaded.queries = load_fvecs(paths.query, config.query_limit);
  if (loaded.base.empty()) {
    throw std::runtime_error("Loaded base dataset is empty: " + paths.base);
  }
  if (loaded.queries.empty()) {
    throw std::runtime_error("Loaded query dataset is empty: " + paths.query);
  }
  if (loaded.base.dim != loaded.queries.dim) {
    throw std::runtime_error("Base and query dimensions do not match");
  }

  loaded.mode = paths.sift_dir.empty() ? "fvecs" : "sift";
  loaded.truth_source = "none";
  if (!paths.truth.empty()) {
    loaded.truth = load_ivecs(paths.truth, loaded.queries.size());
    if (loaded.truth.size() != loaded.queries.size()) {
      throw std::runtime_error(
          "Ground-truth query count does not match loaded queries");
    }
    for (std::size_t query_id = 0; query_id < loaded.truth.size();
         ++query_id) {
      for (const auto id : loaded.truth[query_id]) {
        if (id >= loaded.base.size()) {
          throw std::runtime_error(
              "Ground-truth id exceeds loaded base count; provide a truth file "
              "for the selected --base-limit or load the full base set");
        }
      }
    }
    loaded.truth_from_file = true;
    loaded.truth_source = paths.truth;
  }

  return loaded;
}

}  // namespace agent_aware

