#include "core/pq_encoder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>

namespace agent_aware {
namespace {
constexpr std::uint32_t kPqFormatVersion = 1;
constexpr char kPqMagic[8] = {'A', 'M', 'P', 'Q', 'V', '1', '\0', '\0'};

template <typename T>
void write_pq_value(std::ostream& output, const T& value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(value));
  if (!output) throw std::runtime_error("Failed to write PQ section");
}

template <typename T>
T read_pq_value(std::istream& input) {
  T value{};
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!input) throw std::runtime_error("Truncated PQ section");
  return value;
}
}  // namespace

// Product Quantization 只用于快速候选排序；最终对距离敏感的结果仍可用原始向量 rerank。
PQTrainingStats PQEncoder::train(const VectorSet& base,
                                 const PQTrainingConfig& config) {
  if (base.empty() || base.dim == 0 || config.subspaces == 0 ||
      config.centroids < 2 || config.centroids > 256 ||
      config.train_limit == 0 || config.iterations == 0) {
    throw std::runtime_error("Invalid PQ training configuration");
  }

  subspaces_ = std::min(config.subspaces, base.dim);
  centroids_ = config.centroids;
  dim_ = base.dim;
  offsets_.resize(subspaces_ + 1);
  for (std::size_t m = 0; m <= subspaces_; ++m) {
    // 子空间按连续维度切分；dim 不能整除子空间数时，宽度最多相差一维。
    offsets_[m] = (m * dim_) / subspaces_;
  }

  const std::size_t sample_count =
      std::min(base.size(), std::max(centroids_, config.train_limit));
  std::vector<std::uint32_t> sample_ids(sample_count);
  for (std::size_t i = 0; i < sample_count; ++i) {
    sample_ids[i] = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(i) * base.size()) / sample_count);
  }

  codebooks_.assign(subspaces_ * centroids_ * dim_, 0.0f);
  std::mt19937 rng(config.seed ^ 0x0adc0deu);

  for (std::size_t m = 0; m < subspaces_; ++m) {
    const std::size_t begin = offsets_[m];
    const std::size_t end = offsets_[m + 1];
    const std::size_t width = end - begin;
    for (std::size_t c = 0; c < centroids_; ++c) {
      // 确定性的均匀采样初始化让 benchmark 索引可复现，无需额外保存训练元数据。
      const std::size_t sample = sample_ids[(c * sample_count) / centroids_];
      float* centroid = codebooks_.data() + (m * centroids_ + c) * dim_;
      std::copy(base.row(sample) + begin, base.row(sample) + end,
                centroid + begin);
    }

    std::vector<float> sums(centroids_ * width, 0.0f);
    std::vector<std::size_t> counts(centroids_, 0);
    for (std::size_t iteration = 0; iteration < config.iterations;
         ++iteration) {
      std::fill(sums.begin(), sums.end(), 0.0f);
      std::fill(counts.begin(), counts.end(), 0);

      for (std::size_t i = 0; i < sample_count; ++i) {
        const float* row = base.row(sample_ids[i]);
        const std::size_t best = nearest_centroid(row, m);
        ++counts[best];
        for (std::size_t d = begin; d < end; ++d) {
          sums[best * width + (d - begin)] += row[d];
        }
      }

      for (std::size_t c = 0; c < centroids_; ++c) {
        float* centroid = codebooks_.data() + (m * centroids_ + c) * dim_;
        if (counts[c] == 0) {
          const std::size_t replacement =
              sample_ids[std::uniform_int_distribution<std::size_t>(
                  0, sample_count - 1)(rng)];
          std::copy(base.row(replacement) + begin,
                    base.row(replacement) + end, centroid + begin);
          continue;
        }
        for (std::size_t d = begin; d < end; ++d) {
          centroid[d] =
              sums[c * width + (d - begin)] / static_cast<float>(counts[c]);
        }
      }
    }
  }

  codes_.resize(base.size() * subspaces_);
  for (std::size_t i = 0; i < base.size(); ++i) {
    const auto code = encode(base.row(i));
    std::copy(code.begin(), code.end(),
              codes_.begin() + static_cast<std::ptrdiff_t>(i * subspaces_));
  }

  return PQTrainingStats{base.size(), sample_count, dim_, subspaces_,
                         centroids_, code_bytes(), codebook_bytes()};
}

void PQEncoder::train(const VectorSet& base, std::size_t subspaces,
                      std::size_t centroids, std::size_t train_limit,
                      std::size_t iterations, std::uint32_t seed) {
  PQTrainingConfig config;
  config.subspaces = subspaces;
  config.centroids = centroids;
  config.train_limit = train_limit;
  config.iterations = iterations;
  config.seed = seed;
  (void)train(base, config);
}

std::vector<std::uint8_t> PQEncoder::encode(const float* vector) const {
  validate_trained();
  std::vector<std::uint8_t> code(subspaces_, 0);
  for (std::size_t m = 0; m < subspaces_; ++m) {
    code[m] = static_cast<std::uint8_t>(nearest_centroid(vector, m));
  }
  return code;
}

std::vector<float> PQEncoder::decode_code(const std::uint8_t* code) const {
  validate_trained();
  std::vector<float> decoded(dim_, 0.0f);
  for (std::size_t m = 0; m < subspaces_; ++m) {
    const std::size_t centroid_id = code[m];
    if (centroid_id >= centroids_) {
      throw std::runtime_error("Invalid PQ code centroid id");
    }
    const std::size_t begin = offsets_[m];
    const std::size_t end = offsets_[m + 1];
    const float* centroid =
        codebooks_.data() + (m * centroids_ + centroid_id) * dim_;
    std::copy(centroid + begin, centroid + end, decoded.begin() + begin);
  }
  return decoded;
}

std::vector<float> PQEncoder::decode(std::uint32_t id) const {
  validate_trained();
  if (static_cast<std::size_t>(id) >= vector_count()) {
    throw std::runtime_error("PQ decode id out of range");
  }
  return decode_code(codes_.data() + static_cast<std::size_t>(id) * subspaces_);
}

std::vector<float> PQEncoder::build_adc_table(const float* query) const {
  if (!enabled()) {
    return {};
  }
  // ADC 预计算 query 到各 centroid 的距离，扩展图时用子空间查表替代完整向量 L2。
  std::vector<float> table(subspaces_ * centroids_, 0.0f);
  for (std::size_t m = 0; m < subspaces_; ++m) {
    const std::size_t begin = offsets_[m];
    const std::size_t end = offsets_[m + 1];
    for (std::size_t c = 0; c < centroids_; ++c) {
      const float* centroid =
          codebooks_.data() + (m * centroids_ + c) * dim_;
      float distance = 0.0f;
      for (std::size_t d = begin; d < end; ++d) {
        const float diff = query[d] - centroid[d];
        distance += diff * diff;
      }
      table[m * centroids_ + c] = distance;
    }
  }
  return table;
}

float PQEncoder::adc_distance(std::uint32_t id,
                              const std::vector<float>& table) const {
  if (!enabled() || static_cast<std::size_t>(id) >= vector_count() ||
      table.size() != subspaces_ * centroids_) {
    throw std::runtime_error("Invalid PQ ADC lookup");
  }
  const std::size_t offset = static_cast<std::size_t>(id) * subspaces_;
  return adc_distance_code(codes_.data() + offset, table);
}

float PQEncoder::adc_distance_code(
    const std::uint8_t* code, const std::vector<float>& table) const {
  if (!enabled() || code == nullptr ||
      table.size() != subspaces_ * centroids_) {
    throw std::runtime_error("Invalid PQ ADC code lookup");
  }
  float distance = 0.0f;
  for (std::size_t m = 0; m < subspaces_; ++m) {
    if (code[m] >= centroids_) {
      throw std::runtime_error("Invalid PQ code centroid id");
    }
    distance += table[m * centroids_ + code[m]];
  }
  return distance;
}

void PQEncoder::write(std::ostream& output) const {
  validate_trained();
  output.write(kPqMagic, sizeof(kPqMagic));
  write_pq_value(output, kPqFormatVersion);
  write_pq_value(output, static_cast<std::uint32_t>(dim_));
  write_pq_value(output, static_cast<std::uint32_t>(subspaces_));
  write_pq_value(output, static_cast<std::uint32_t>(centroids_));
  write_pq_value(output, static_cast<std::uint64_t>(offsets_.size()));
  write_pq_value(output, static_cast<std::uint64_t>(codebooks_.size()));
  write_pq_value(output, static_cast<std::uint64_t>(codes_.size()));
  for (const auto value : offsets_) {
    write_pq_value(output, static_cast<std::uint64_t>(value));
  }
  output.write(reinterpret_cast<const char*>(codebooks_.data()),
               static_cast<std::streamsize>(codebooks_.size() * sizeof(float)));
  output.write(reinterpret_cast<const char*>(codes_.data()),
               static_cast<std::streamsize>(codes_.size()));
  if (!output) throw std::runtime_error("Failed to write PQ section payload");
}

PQEncoder PQEncoder::read(std::istream& input) {
  char magic[sizeof(kPqMagic)]{};
  input.read(magic, sizeof(magic));
  if (!input || std::memcmp(magic, kPqMagic, sizeof(magic)) != 0) {
    throw std::runtime_error("Invalid PQ section magic");
  }
  if (read_pq_value<std::uint32_t>(input) != kPqFormatVersion) {
    throw std::runtime_error("Unsupported PQ section version");
  }
  PQEncoder model;
  model.dim_ = read_pq_value<std::uint32_t>(input);
  model.subspaces_ = read_pq_value<std::uint32_t>(input);
  model.centroids_ = read_pq_value<std::uint32_t>(input);
  const auto offset_count = read_pq_value<std::uint64_t>(input);
  const auto codebook_count = read_pq_value<std::uint64_t>(input);
  const auto code_count = read_pq_value<std::uint64_t>(input);
  if (model.dim_ == 0 || model.subspaces_ == 0 || model.centroids_ < 2 ||
      model.centroids_ > 256 || offset_count != model.subspaces_ + 1 ||
      offset_count > 1'000'000 || codebook_count > 10'000'000'000ULL ||
      code_count > 10'000'000'000ULL) {
    throw std::runtime_error("Invalid PQ section metadata");
  }
  model.offsets_.resize(static_cast<std::size_t>(offset_count));
  for (auto& value : model.offsets_) {
    value = static_cast<std::size_t>(read_pq_value<std::uint64_t>(input));
  }
  if (model.offsets_.front() != 0 || model.offsets_.back() != model.dim_) {
    throw std::runtime_error("PQ offsets do not cover the model dimension");
  }
  for (std::size_t i = 1; i < model.offsets_.size(); ++i) {
    if (model.offsets_[i] < model.offsets_[i - 1]) {
      throw std::runtime_error("PQ offsets are not monotonic");
    }
  }
  model.codebooks_.resize(static_cast<std::size_t>(codebook_count));
  input.read(reinterpret_cast<char*>(model.codebooks_.data()),
             static_cast<std::streamsize>(model.codebooks_.size() * sizeof(float)));
  model.codes_.resize(static_cast<std::size_t>(code_count));
  input.read(reinterpret_cast<char*>(model.codes_.data()),
             static_cast<std::streamsize>(model.codes_.size()));
  if (!input || model.codebooks_.size() !=
                    model.subspaces_ * model.centroids_ * model.dim_ ||
      model.codes_.size() % model.subspaces_ != 0) {
    throw std::runtime_error("Invalid PQ section payload sizes");
  }
  model.validate_trained();
  return model;
}

void PQEncoder::write_file(const std::string& path) const {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("Cannot create PQ model: " + path);
  write(output);
}

PQEncoder PQEncoder::read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open PQ model: " + path);
  return read(input);
}

std::size_t PQEncoder::nearest_centroid(const float* vector,
                                        std::size_t subspace) const {
  const std::size_t begin = offsets_[subspace];
  const std::size_t end = offsets_[subspace + 1];
  std::size_t best = 0;
  float best_distance = std::numeric_limits<float>::max();
  for (std::size_t c = 0; c < centroids_; ++c) {
    const float* centroid =
        codebooks_.data() + (subspace * centroids_ + c) * dim_;
    float distance = 0.0f;
    for (std::size_t d = begin; d < end; ++d) {
      const float diff = vector[d] - centroid[d];
      distance += diff * diff;
    }
    if (distance < best_distance) {
      best_distance = distance;
      best = c;
    }
  }
  return best;
}

void PQEncoder::validate_trained() const {
  if (subspaces_ == 0 || centroids_ == 0 || dim_ == 0 ||
      offsets_.size() != subspaces_ + 1 || codebooks_.empty()) {
    throw std::runtime_error("PQ encoder has not been trained");
  }
}

}  // namespace agent_aware
