#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace agentmem {

inline constexpr std::size_t kDiskIndexPageSize = 4096;
inline constexpr std::uint32_t kDiskIndexFileMagic = 0x58444941u;    // AIDX
inline constexpr std::uint32_t kDiskNodeRecordMagic = 0x52444941u;  // AIDR
inline constexpr std::uint32_t kDiskIndexVersion = 1;

struct IndexFileHeader {
  std::uint32_t magic = kDiskIndexFileMagic;
  std::uint32_t version = kDiskIndexVersion;
  std::uint32_t dim = 0;
  std::uint32_t max_degree = 0;
  std::uint64_t num_nodes = 0;
  std::uint64_t record_size = kDiskIndexPageSize;
  std::uint64_t header_size = kDiskIndexPageSize;
  std::uint64_t entry_node = 0;
  std::uint8_t reserved[kDiskIndexPageSize - 48] = {};
};

static_assert(sizeof(IndexFileHeader) == kDiskIndexPageSize,
              "IndexFileHeader must occupy one 4KB page");

struct DiskNodeRecordHeader {
  std::uint32_t magic = kDiskNodeRecordMagic;
  std::uint32_t node_id = 0;
  std::uint16_t degree = 0;
  std::uint16_t dim = 0;
  std::uint32_t flags = 0;
  std::uint32_t checksum = 0;
  std::uint32_t vector_offset = 0;
  std::uint32_t neighbors_offset = 0;
  std::uint32_t payload_bytes = 0;
};

static_assert(sizeof(DiskNodeRecordHeader) == 32,
              "DiskNodeRecordHeader must stay compact");

struct NodeRecord {
  std::uint32_t node_id = 0;
  std::uint16_t degree = 0;
  std::uint16_t dim = 0;
  std::vector<float> vector;
  std::vector<std::uint32_t> neighbors;
};

struct AlignedBufferDeleter {
  void operator()(std::uint8_t* ptr) const noexcept;
};

using AlignedBuffer = std::unique_ptr<std::uint8_t, AlignedBufferDeleter>;

AlignedBuffer allocate_aligned_buffer(std::size_t bytes = kDiskIndexPageSize);

std::size_t disk_record_bytes(std::uint32_t dim, std::uint32_t degree);

void validate_disk_record_shape(std::uint32_t dim, std::uint32_t degree);

IndexFileHeader make_index_file_header(std::uint32_t dim,
                                       std::uint32_t max_degree,
                                       std::uint64_t num_nodes,
                                       std::uint64_t entry_node);

void validate_index_file_header(const IndexFileHeader& header);

std::uint64_t disk_record_offset(std::uint32_t node_id,
                                 const IndexFileHeader& header);

void encode_node_record(std::uint32_t node_id, const float* vector,
                        std::uint32_t dim,
                        const std::vector<std::uint32_t>& neighbors,
                        void* buffer, std::size_t buffer_bytes);

NodeRecord decode_node_record(const void* buffer, std::size_t buffer_bytes);

}  // namespace agentmem
