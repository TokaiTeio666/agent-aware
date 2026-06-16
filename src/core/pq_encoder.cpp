#include "agentmem/core/pq_encoder.h"

#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>

namespace agentmem {

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
  float distance = 0.0f;
  const std::size_t offset = static_cast<std::size_t>(id) * subspaces_;
  for (std::size_t m = 0; m < subspaces_; ++m) {
    distance += table[m * centroids_ + codes_[offset + m]];
  }
  return distance;
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

}  // namespace agentmem
