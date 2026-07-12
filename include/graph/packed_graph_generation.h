#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/pq_encoder.h"
#include "dynamic/dynamic_types.h"

namespace agent_aware {

struct PackedGenerationEntry {
  NodeId node_id = 0;
  std::uint64_t sequence_id = 0;
  std::uint64_t offset = 0;
  std::uint64_t neighbor_offset = 0;
  std::uint32_t neighbor_count = 0;
  std::uint32_t dim = 0;
  bool deleted = false;
  std::vector<std::uint8_t> pq_code;
};

class PackedGraphGeneration {
 public:
  static std::shared_ptr<const PackedGraphGeneration> open_or_create(
      const std::filesystem::path& base_index_path,
      std::shared_ptr<const PQEncoder> pq_model,
      bool cleanup_obsolete = true);
  static std::shared_ptr<const PackedGraphGeneration> publish(
      const std::filesystem::path& base_index_path,
      const PackedGraphGeneration& previous,
      const std::vector<MaterializedRecord>& delta, std::uint64_t cutoff_sequence,
      std::shared_ptr<const PQEncoder> pq_model,
      const std::atomic<bool>* cancelled = nullptr);

  std::uint64_t id() const { return generation_id_; }
  std::uint64_t incorporated_sequence() const { return incorporated_sequence_; }
  std::size_t segment_count() const { return segment_path_.empty() ? 0 : 1; }
  std::size_t size() const { return entries_.size(); }
  NodeId max_node_id() const;
  void cleanup_obsolete_segments() const;

  bool resolve(NodeId node_id, MaterializedRecord& out) const;
  std::vector<MaterializedRecord> search(const float* query, std::uint32_t dim,
                                    std::size_t topk,
                                    std::size_t candidate_top_l) const;
  std::vector<MaterializedRecord> materialize_all() const;

 private:
  static void load_segment(PackedGraphGeneration& generation);
  bool read_entry(const PackedGenerationEntry& entry,
                  MaterializedRecord& out) const;
  std::vector<NodeId> read_neighbors(const PackedGenerationEntry& entry) const;

  std::uint64_t generation_id_ = 0;
  std::uint64_t incorporated_sequence_ = 0;
  std::filesystem::path live_dir_;
  std::filesystem::path segment_path_;
  std::shared_ptr<const PQEncoder> pq_model_;
  std::unordered_map<NodeId, PackedGenerationEntry> entries_;
};

}  // namespace agent_aware
