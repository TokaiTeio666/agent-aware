#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent_aware/core/types.h"

namespace agent_aware {

struct DatasetPaths {
  std::string base;
  std::string query;
  std::string truth;
  std::string sift_dir;
};

struct DatasetLoadConfig {
  bool synthetic = true;
  DatasetPaths paths;
  std::size_t base_limit = 0;
  std::size_t query_limit = 0;
  SyntheticConfig synthetic_config;
};

struct LoadedDataset {
  VectorSet base;
  VectorSet queries;
  std::vector<std::vector<std::uint32_t>> truth;
  std::string mode;
  std::string truth_source;
  bool truth_from_file = false;
};

VectorSet load_fvecs(const std::string& path, std::size_t limit = 0);

std::vector<std::vector<std::uint32_t>> load_ivecs(const std::string& path,
                                                   std::size_t limit = 0);

SyntheticData generate_synthetic(const SyntheticConfig& config);

DatasetPaths resolve_sift_paths(const std::string& sift_dir);

LoadedDataset load_dataset(const DatasetLoadConfig& config);

}  // namespace agent_aware

