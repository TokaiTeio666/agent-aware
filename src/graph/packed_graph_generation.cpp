#include "graph/packed_graph_generation.h"

#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <regex>
#include <queue>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "core/simd_distance.h"
#include "graph/vamana_builder.h"

namespace agent_aware {
namespace {

constexpr std::uint32_t kMagic = 0x34474d50;  // PMG4
constexpr std::uint32_t kVersion = 2;

template <typename T>
void write_value(std::ostream& output, const T& value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
bool read_value(std::istream& input, T& value) {
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  return static_cast<bool>(input);
}

void sync_file(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("Cannot sync generation file");
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) throw std::runtime_error("Cannot sync generation file");
}

void sync_dir(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd >= 0) {
    (void)::fsync(fd);
    ::close(fd);
  }
}

std::filesystem::path live_dir_for(const std::filesystem::path& base) {
  return std::filesystem::path(base.string() + ".live");
}

std::filesystem::path manifest_for(const std::filesystem::path& live) {
  return live / "generation.manifest";
}

std::filesystem::path segment_for(const std::filesystem::path& live,
                                  std::uint64_t generation) {
  return live / ("segment_" + std::to_string(generation) + ".bin");
}

std::uint32_t checksum_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::uint32_t hash = 2166136261u;
  char buffer[64 * 1024];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const auto count = input.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      hash ^= static_cast<unsigned char>(buffer[i]);
      hash *= 16777619u;
    }
  }
  return hash;
}

void cleanup_unreferenced(const std::filesystem::path& live,
                          const std::filesystem::path& referenced) {
  for (const auto& item : std::filesystem::directory_iterator(live)) {
    if (!item.is_regular_file()) continue;
    const auto name = item.path().filename().string();
    if (item.path() == referenced || name == "generation.manifest") continue;
    if (name.rfind("segment_", 0) == 0 ||
        name.find(".tmp") != std::string::npos) {
      std::error_code error;
      std::filesystem::remove(item.path(), error);
    }
  }
}

}  // namespace

std::shared_ptr<const PackedGraphGeneration>
PackedGraphGeneration::open_or_create(
    const std::filesystem::path& base_index_path,
    std::shared_ptr<const PQEncoder> pq_model, bool cleanup_obsolete) {
  auto generation = std::make_shared<PackedGraphGeneration>();
  generation->live_dir_ = live_dir_for(base_index_path);
  generation->pq_model_ = std::move(pq_model);
  std::filesystem::create_directories(generation->live_dir_);
  const auto manifest = manifest_for(generation->live_dir_);
  if (!std::filesystem::exists(manifest)) return generation;
  std::ifstream input(manifest);
  std::string segment_name;
  std::uint64_t segment_bytes = 0;
  std::uint32_t segment_checksum = 0;
  if (!(input >> generation->generation_id_ >>
        generation->incorporated_sequence_ >> segment_name >> segment_bytes >>
        segment_checksum)) {
    throw std::runtime_error("Corrupted packed generation manifest");
  }
  if (segment_name != "-") {
    generation->segment_path_ = generation->live_dir_ / segment_name;
    if (!std::filesystem::is_regular_file(generation->segment_path_) ||
        std::filesystem::file_size(generation->segment_path_) != segment_bytes ||
        checksum_file(generation->segment_path_) != segment_checksum) {
      throw std::runtime_error("Packed generation checksum mismatch");
    }
    PackedGraphGeneration::load_segment(*generation);
  }
  if (cleanup_obsolete) {
    cleanup_unreferenced(generation->live_dir_, generation->segment_path_);
  }
  return generation;
}

std::shared_ptr<const PackedGraphGeneration> PackedGraphGeneration::publish(
    const std::filesystem::path& base_index_path,
    const PackedGraphGeneration& previous,
    const std::vector<DynamicRecord>& delta, std::uint64_t cutoff_sequence,
    std::shared_ptr<const PQEncoder> pq_model,
    const std::atomic<bool>* cancelled) {
  const auto check_cancelled = [&] {
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
      throw std::runtime_error("Packed generation merge cancelled");
    }
  };
  check_cancelled();
  std::unordered_map<NodeId, DynamicRecord> latest;
  for (auto& record : previous.materialize_all()) {
    latest[record.node_id] = std::move(record);
  }
  for (const auto& record : delta) {
    if (record.sequence_id <= previous.incorporated_sequence_ ||
        record.sequence_id > cutoff_sequence) continue;
    auto found = latest.find(record.node_id);
    if (found == latest.end() ||
        found->second.sequence_id < record.sequence_id) {
      latest[record.node_id] = record;
    }
  }

  const auto live = live_dir_for(base_index_path);
  std::filesystem::create_directories(live);
  const std::uint64_t generation_id = previous.generation_id_ + 1;
  const auto final_segment = segment_for(live, generation_id);
  const auto temp_segment = std::filesystem::path(final_segment.string() + ".tmp");
  std::vector<NodeId> ids;
  ids.reserve(latest.size());
  for (const auto& item : latest) ids.push_back(item.first);
  std::sort(ids.begin(), ids.end());
  VectorSet overlay(ids.size(), pq_model ? pq_model->dim() : 0);
  std::vector<std::uint32_t> deleted_local_ids;
  std::unordered_map<NodeId, std::size_t> local_by_id;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    local_by_id[ids[i]] = i;
    const auto& record = latest.at(ids[i]);
    if (record.deleted) {
      deleted_local_ids.push_back(static_cast<std::uint32_t>(i));
    } else {
      std::copy(record.vector.begin(), record.vector.end(), overlay.mutable_row(i));
    }
  }
  std::vector<std::vector<NodeId>> neighbors(ids.size());
  if (!ids.empty() && deleted_local_ids.size() < ids.size()) {
    check_cancelled();
    VamanaBuildConfig graph_config;
    graph_config.max_degree = std::min<std::size_t>(16, ids.size());
    graph_config.search_width = std::min<std::size_t>(64, ids.size());
    graph_config.alpha = 1.2;
    graph_config.seed = static_cast<std::uint32_t>(generation_id);
    graph_config.add_reverse_edges = true;
    graph_config.deleted_ids = deleted_local_ids;
    const auto graph = VamanaBuilder(graph_config).build(overlay);
    for (std::size_t i = 0; i < graph.adjacency.size(); ++i) {
      for (const auto local_neighbor : graph.adjacency[i]) {
        neighbors[i].push_back(ids[local_neighbor]);
      }
    }
  }

  {
    std::ofstream output(temp_segment, std::ios::binary | std::ios::trunc);
    write_value(output, kMagic);
    write_value(output, kVersion);
    write_value(output, generation_id);
    write_value(output, cutoff_sequence);
    write_value(output, static_cast<std::uint64_t>(latest.size()));
    for (const auto id : ids) {
      check_cancelled();
      const auto& record = latest.at(id);
      const auto local_id = local_by_id.at(id);
      const auto code = !record.deleted && pq_model &&
                                record.vector.size() == pq_model->dim()
                            ? pq_model->encode(record.vector.data())
                            : std::vector<std::uint8_t>{};
      if (!record.deleted &&
          (!pq_model || record.vector.size() != pq_model->dim())) {
        throw std::runtime_error("Generation record dimension mismatch");
      }
      write_value(output, record.node_id);
      write_value(output, record.sequence_id);
      write_value(output, record.dim);
      const std::uint8_t deleted = record.deleted ? 1 : 0;
      write_value(output, deleted);
      const std::uint32_t code_bytes = static_cast<std::uint32_t>(code.size());
      const std::uint32_t neighbor_count =
          static_cast<std::uint32_t>(neighbors[local_id].size());
      const std::uint32_t vector_bytes = static_cast<std::uint32_t>(
          record.vector.size() * sizeof(float));
      write_value(output, code_bytes);
      write_value(output, neighbor_count);
      write_value(output, vector_bytes);
      if (!code.empty()) output.write(reinterpret_cast<const char*>(code.data()), code.size());
      if (neighbor_count) {
        output.write(reinterpret_cast<const char*>(neighbors[local_id].data()),
                     neighbor_count * sizeof(NodeId));
      }
      if (!record.vector.empty()) {
        output.write(reinterpret_cast<const char*>(record.vector.data()), vector_bytes);
      }
    }
    output.flush();
    if (!output) throw std::runtime_error("Failed to write generation segment");
  }
  sync_file(temp_segment);
  check_cancelled();
  std::filesystem::rename(temp_segment, final_segment);
  const auto segment_bytes = std::filesystem::file_size(final_segment);
  const auto segment_checksum = checksum_file(final_segment);

  const auto manifest = manifest_for(live);
  const auto temp_manifest = std::filesystem::path(manifest.string() + ".tmp");
  {
    check_cancelled();
    std::ofstream output(temp_manifest, std::ios::trunc);
    output << generation_id << ' ' << cutoff_sequence << ' '
           << final_segment.filename().string() << ' ' << segment_bytes << ' '
           << segment_checksum << '\n';
    output.flush();
    if (!output) throw std::runtime_error("Failed to write generation manifest");
  }
  sync_file(temp_manifest);
  check_cancelled();
  std::error_code error;
  std::filesystem::rename(temp_manifest, manifest, error);
  if (error) {
    std::filesystem::remove(manifest, error);
    error.clear();
    std::filesystem::rename(temp_manifest, manifest, error);
  }
  if (error) throw std::runtime_error("Failed to publish generation manifest");
  sync_dir(live);
  return open_or_create(base_index_path, std::move(pq_model), false);
}

void PackedGraphGeneration::load_segment(PackedGraphGeneration& generation) {
  std::ifstream input(generation.segment_path_, std::ios::binary);
  std::uint32_t magic = 0, version = 0;
  std::uint64_t generation_id = 0, cutoff = 0, count = 0;
  if (!read_value(input, magic) || !read_value(input, version) ||
      !read_value(input, generation_id) || !read_value(input, cutoff) ||
      !read_value(input, count) || magic != kMagic || version != kVersion ||
      generation_id != generation.generation_id_ ||
      cutoff != generation.incorporated_sequence_) {
    throw std::runtime_error("Corrupted packed generation segment");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    PackedGenerationEntry entry;
    std::uint8_t deleted = 0;
    std::uint32_t code_bytes = 0, neighbor_count = 0, vector_bytes = 0;
    if (!read_value(input, entry.node_id) ||
        !read_value(input, entry.sequence_id) ||
        !read_value(input, entry.dim) || !read_value(input, deleted) ||
        !read_value(input, code_bytes) || !read_value(input, neighbor_count) ||
        !read_value(input, vector_bytes)) {
      throw std::runtime_error("Truncated packed generation entry");
    }
    entry.deleted = deleted != 0;
    entry.pq_code.resize(code_bytes);
    if (code_bytes) input.read(reinterpret_cast<char*>(entry.pq_code.data()), code_bytes);
    entry.neighbor_offset = static_cast<std::uint64_t>(input.tellg());
    entry.neighbor_count = neighbor_count;
    input.seekg(static_cast<std::streamoff>(neighbor_count * sizeof(NodeId)),
                std::ios::cur);
    entry.offset = static_cast<std::uint64_t>(input.tellg());
    input.seekg(vector_bytes, std::ios::cur);
    if (!input) throw std::runtime_error("Truncated packed generation vector");
    generation.entries_[entry.node_id] = std::move(entry);
  }
}

std::vector<NodeId> PackedGraphGeneration::read_neighbors(
    const PackedGenerationEntry& entry) const {
  std::vector<NodeId> output(entry.neighbor_count);
  if (output.empty()) return output;
  std::ifstream input(segment_path_, std::ios::binary);
  input.seekg(static_cast<std::streamoff>(entry.neighbor_offset));
  input.read(reinterpret_cast<char*>(output.data()),
             output.size() * sizeof(NodeId));
  if (!input) return {};
  return output;
}

bool PackedGraphGeneration::read_entry(const PackedGenerationEntry& entry,
                                       DynamicRecord& out) const {
  DynamicRecord record;
  record.node_id = entry.node_id;
  record.sequence_id = entry.sequence_id;
  record.dim = entry.dim;
  record.deleted = entry.deleted;
  if (!entry.deleted) {
    std::ifstream input(segment_path_, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(entry.offset));
    record.vector.resize(entry.dim);
    input.read(reinterpret_cast<char*>(record.vector.data()),
               record.vector.size() * sizeof(float));
    if (!input) return false;
  }
  out = std::move(record);
  return true;
}

bool PackedGraphGeneration::resolve(NodeId node_id, DynamicRecord& out) const {
  const auto found = entries_.find(node_id);
  return found != entries_.end() && read_entry(found->second, out);
}

NodeId PackedGraphGeneration::max_node_id() const {
  NodeId value = 0;
  for (const auto& item : entries_) value = std::max(value, item.first);
  return value;
}

void PackedGraphGeneration::cleanup_obsolete_segments() const {
  cleanup_unreferenced(live_dir_, segment_path_);
}

std::vector<DynamicRecord> PackedGraphGeneration::search(
    const float* query, std::uint32_t dim, std::size_t topk,
    std::size_t candidate_top_l) const {
  struct Candidate { const PackedGenerationEntry* entry; float distance; };
  std::vector<Candidate> candidates;
  const auto table = pq_model_ ? pq_model_->build_adc_table(query)
                               : std::vector<float>{};
  const auto less = [](const Candidate& a, const Candidate& b) {
    return a.distance == b.distance ? a.entry->node_id < b.entry->node_id
                                    : a.distance < b.distance;
  };
  struct FurtherFirst {
    bool operator()(const Candidate& a, const Candidate& b) const {
      return a.distance > b.distance;
    }
  };
  std::priority_queue<Candidate, std::vector<Candidate>, FurtherFirst> frontier;
  std::unordered_set<NodeId> visited;
  auto push = [&](const PackedGenerationEntry& entry) {
    if (entry.deleted || entry.dim != dim || !pq_model_ ||
        entry.pq_code.size() != pq_model_->subspaces() ||
        !visited.insert(entry.node_id).second) return;
    frontier.push(Candidate{
        &entry, pq_model_->adc_distance_code(entry.pq_code.data(), table)});
  };
  for (const auto& item : entries_) {
    if (!item.second.deleted) {
      push(item.second);
      break;
    }
  }
  const std::size_t expansion_limit =
      std::min(entries_.size(), std::max<std::size_t>(64, candidate_top_l * 2));
  while (!frontier.empty() && candidates.size() < expansion_limit) {
    const Candidate current = frontier.top();
    frontier.pop();
    candidates.push_back(current);
    for (const auto neighbor : read_neighbors(*current.entry)) {
      const auto found = entries_.find(neighbor);
      if (found != entries_.end()) push(found->second);
    }
  }
  // Disconnected components are rare but possible after deletes; seed them
  // only when graph traversal did not provide enough rerank candidates.
  if (candidates.size() < candidate_top_l) {
    for (const auto& item : entries_) push(item.second);
    while (!frontier.empty() && candidates.size() < expansion_limit) {
      candidates.push_back(frontier.top());
      frontier.pop();
    }
  }
  const std::size_t limit = std::min(candidates.size(), candidate_top_l);
  if (limit < candidates.size()) {
    std::nth_element(candidates.begin(), candidates.begin() + limit,
                     candidates.end(), less);
    candidates.resize(limit);
  }
  std::vector<std::pair<float, DynamicRecord>> exact;
  for (const auto& candidate : candidates) {
    DynamicRecord record;
    if (read_entry(*candidate.entry, record)) {
      exact.emplace_back(squared_l2(query, record.vector.data(), dim),
                         std::move(record));
    }
  }
  std::sort(exact.begin(), exact.end(), [](const auto& a, const auto& b) {
    return a.first == b.first ? a.second.node_id < b.second.node_id
                              : a.first < b.first;
  });
  if (exact.size() > topk) exact.resize(topk);
  std::vector<DynamicRecord> output;
  for (auto& item : exact) output.push_back(std::move(item.second));
  return output;
}

std::vector<DynamicRecord> PackedGraphGeneration::materialize_all() const {
  std::vector<DynamicRecord> output;
  output.reserve(entries_.size());
  for (const auto& item : entries_) {
    DynamicRecord record;
    if (read_entry(item.second, record)) output.push_back(std::move(record));
  }
  return output;
}

}  // namespace agent_aware
