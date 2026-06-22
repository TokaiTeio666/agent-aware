#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "agent_aware/core/types.h"

namespace agent_aware {

struct PQTrainingConfig {
  std::size_t subspaces = 8;
  std::size_t centroids = 256;
  std::size_t train_limit = 100000;
  std::size_t iterations = 4;
  std::uint32_t seed = 42;
};

struct PQTrainingStats {
  std::size_t vector_count = 0;
  std::size_t training_vectors = 0;
  std::size_t dim = 0;
  std::size_t subspaces = 0;
  std::size_t centroids = 0;
  std::size_t code_bytes = 0;
  std::size_t codebook_bytes = 0;
};

class PQEncoder {
 public:
  PQTrainingStats train(const VectorSet& base,
                        const PQTrainingConfig& config);

  void train(const VectorSet& base, std::size_t subspaces,
             std::size_t centroids, std::size_t train_limit,
             std::size_t iterations, std::uint32_t seed);

  bool enabled() const {
    return !codes_.empty();
  }

  std::size_t vector_count() const {
    return subspaces_ == 0 ? 0 : codes_.size() / subspaces_;
  }

  std::size_t dim() const {
    return dim_;
  }

  std::size_t subspaces() const {
    return subspaces_;
  }

  std::size_t centroids() const {
    return centroids_;
  }

  std::size_t code_bytes() const {
    return codes_.size();
  }

  std::size_t codebook_bytes() const {
    return codebooks_.size() * sizeof(float);
  }

  std::size_t offset_bytes() const {
    return offsets_.size() * sizeof(std::size_t);
  }

  const std::vector<std::uint8_t>& codes() const {
    return codes_;
  }

  const std::vector<float>& codebooks() const {
    return codebooks_;
  }

  const std::vector<std::size_t>& offsets() const {
    return offsets_;
  }

  std::vector<std::uint8_t> encode(const float* vector) const;
  std::vector<float> decode_code(const std::uint8_t* code) const;
  std::vector<float> decode(std::uint32_t id) const;

  std::vector<float> build_adc_table(const float* query) const;
  float adc_distance(std::uint32_t id, const std::vector<float>& table) const;

 private:
  std::size_t nearest_centroid(const float* vector, std::size_t subspace) const;
  void validate_trained() const;

  std::size_t subspaces_ = 0;
  std::size_t centroids_ = 0;
  std::size_t dim_ = 0;
  std::vector<std::size_t> offsets_;
  std::vector<float> codebooks_;
  std::vector<std::uint8_t> codes_;
};

}  // namespace agent_aware
