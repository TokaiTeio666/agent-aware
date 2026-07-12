#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "core/types.h"
#include "graph/disk_graph_index.h"

namespace agent_aware {

inline constexpr std::array<char, 8> kPackedGraphMagic = {
    'A', 'M', 'F', 'P', 'V', '3', '\0', '\0'};
inline constexpr std::uint32_t kPackedGraphVersion = 3;

template <typename T>
void write_value(std::ofstream& output, const T& value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(T));
  if (!output) {
    throw std::runtime_error("Failed to write graph index");
  }
}

template <typename T>
T read_value(std::ifstream& input) {
  T value{};
  input.read(reinterpret_cast<char*>(&value), sizeof(T));
  if (!input) {
    throw std::runtime_error("Failed to read graph index header");
  }
  return value;
}

template <typename T>
void put_bytes(std::vector<char>& page, std::size_t& offset, const T& value) {
  if (offset + sizeof(T) > page.size()) {
    throw std::runtime_error("Graph node page overflow");
  }
  std::memcpy(page.data() + offset, &value, sizeof(T));
  offset += sizeof(T);
}

template <typename T>
T get_bytes(const std::vector<char>& page, std::size_t& offset) {
  if (offset + sizeof(T) > page.size()) {
    throw std::runtime_error("Graph node page is truncated");
  }
  T value{};
  std::memcpy(&value, page.data() + offset, sizeof(T));
  offset += sizeof(T);
  return value;
}

std::size_t graph_record_bytes(std::size_t dim, std::size_t degree);
std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment);
std::size_t packed_nodes_per_page(std::size_t dim, std::size_t degree,
                                  std::size_t page_size);

class DiskPageCodec {
 public:
  static std::vector<char> encode_packed_page(
      std::uint32_t page_id, const VectorSet& base,
      const std::vector<std::vector<std::uint32_t>>& graph,
      const std::vector<std::uint32_t>& order, std::size_t nodes_per_page,
      std::size_t page_size, std::size_t degree);

  static PackedDiskGraphIndex::DecodedPage decode_packed_page(
      const DiskGraphMetadata& metadata, std::uint32_t page_id,
      std::vector<char> page);

  static PackedDiskGraphIndex::DiskNode node_at(
      const DiskGraphMetadata& metadata,
      const PackedDiskGraphIndex::DecodedPage& page, std::uint32_t slot);

  static PackedDiskGraphIndex::DiskNode find_node(
      const DiskGraphMetadata& metadata,
      const PackedDiskGraphIndex::DecodedPage& page, std::uint32_t node_id);

  static const float* vector_data(
      const DiskGraphMetadata& metadata,
      const PackedDiskGraphIndex::DecodedPage& page,
      const PackedDiskGraphIndex::DiskNode& node);
};

}  // namespace agent_aware
