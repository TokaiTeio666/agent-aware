#include "agentmem/storage/disk_record.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

#if defined(_MSC_VER)
#include <malloc.h>
#else
#include <cstdlib>
#endif

namespace agentmem {
namespace {

std::uint32_t fnv1a_bytes(const void* data, std::size_t bytes,
                          std::uint32_t seed = 2166136261u) {
  const auto* cursor = static_cast<const std::uint8_t*>(data);
  std::uint32_t hash = seed;
  for (std::size_t i = 0; i < bytes; ++i) {
    hash ^= cursor[i];
    hash *= 16777619u;
  }
  return hash;
}

std::uint32_t payload_checksum(const float* vector, std::uint32_t dim,
                               const std::uint32_t* neighbors,
                               std::uint32_t degree) {
  std::uint32_t hash = 2166136261u;
  if (dim > 0) {
    hash = fnv1a_bytes(vector, static_cast<std::size_t>(dim) * sizeof(float),
                       hash);
  }
  if (degree > 0) {
    hash = fnv1a_bytes(
        neighbors, static_cast<std::size_t>(degree) * sizeof(std::uint32_t),
        hash);
  }
  return hash;
}

void require_exact_page(std::size_t buffer_bytes) {
  if (buffer_bytes != kDiskIndexPageSize) {
    throw std::runtime_error("Disk record buffer must be exactly 4096 bytes");
  }
}

}  // namespace

void AlignedBufferDeleter::operator()(std::uint8_t* ptr) const noexcept {
#if defined(_MSC_VER)
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

AlignedBuffer allocate_aligned_buffer(std::size_t bytes) {
  if (bytes == 0 || bytes % kDiskIndexPageSize != 0) {
    throw std::runtime_error("Aligned buffer size must be a positive 4KB multiple");
  }

  void* raw = nullptr;
#if defined(_MSC_VER)
  raw = _aligned_malloc(bytes, kDiskIndexPageSize);
  if (raw == nullptr) {
    throw std::runtime_error("Failed to allocate aligned disk buffer");
  }
#else
  if (posix_memalign(&raw, kDiskIndexPageSize, bytes) != 0) {
    throw std::runtime_error("Failed to allocate aligned disk buffer");
  }
#endif
  std::memset(raw, 0, bytes);
  return AlignedBuffer(static_cast<std::uint8_t*>(raw));
}

std::size_t disk_record_bytes(std::uint32_t dim, std::uint32_t degree) {
  const std::uint64_t vector_bytes =
      static_cast<std::uint64_t>(dim) * sizeof(float);
  const std::uint64_t neighbor_bytes =
      static_cast<std::uint64_t>(degree) * sizeof(std::uint32_t);
  const std::uint64_t total =
      sizeof(DiskNodeRecordHeader) + vector_bytes + neighbor_bytes;
  if (total > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("Disk record size overflows size_t");
  }
  return static_cast<std::size_t>(total);
}

void validate_disk_record_shape(std::uint32_t dim, std::uint32_t degree) {
  if (dim == 0) {
    throw std::runtime_error("Disk record dimension must be positive");
  }
  if (dim > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("Disk record dimension exceeds header limit");
  }
  if (degree > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("Disk record degree exceeds header limit");
  }
  if (disk_record_bytes(dim, degree) > kDiskIndexPageSize) {
    throw std::runtime_error("Disk record payload exceeds 4096 bytes");
  }
}

IndexFileHeader make_index_file_header(std::uint32_t dim,
                                       std::uint32_t max_degree,
                                       std::uint64_t num_nodes,
                                       std::uint64_t entry_node) {
  validate_disk_record_shape(dim, max_degree);
  if (num_nodes > 0 && entry_node >= num_nodes) {
    throw std::runtime_error("Disk index entry node is out of range");
  }

  IndexFileHeader header;
  header.magic = kDiskIndexFileMagic;
  header.version = kDiskIndexVersion;
  header.dim = dim;
  header.max_degree = max_degree;
  header.num_nodes = num_nodes;
  header.record_size = kDiskIndexPageSize;
  header.header_size = kDiskIndexPageSize;
  header.entry_node = entry_node;
  std::fill(std::begin(header.reserved), std::end(header.reserved), 0);
  return header;
}

void validate_index_file_header(const IndexFileHeader& header) {
  if (header.magic != kDiskIndexFileMagic) {
    throw std::runtime_error("Invalid disk index magic");
  }
  if (header.version != kDiskIndexVersion) {
    throw std::runtime_error("Unsupported disk index version");
  }
  if (header.record_size != kDiskIndexPageSize ||
      header.header_size != kDiskIndexPageSize) {
    throw std::runtime_error("Disk index uses an unsupported page size");
  }
  validate_disk_record_shape(header.dim, header.max_degree);
  if (header.num_nodes > 0 && header.entry_node >= header.num_nodes) {
    throw std::runtime_error("Disk index entry node is out of range");
  }
}

std::uint64_t disk_record_offset(std::uint32_t node_id,
                                 const IndexFileHeader& header) {
  validate_index_file_header(header);
  if (node_id >= header.num_nodes) {
    throw std::out_of_range("Disk index node id is out of range");
  }
  const std::uint64_t max_node_offset =
      std::numeric_limits<std::uint64_t>::max() - header.header_size;
  if (header.record_size != 0 && node_id > max_node_offset / header.record_size) {
    throw std::runtime_error("Disk index record offset overflows uint64_t");
  }
  return header.header_size + static_cast<std::uint64_t>(node_id) *
                                  header.record_size;
}

void encode_node_record(std::uint32_t node_id, const float* vector,
                        std::uint32_t dim,
                        const std::vector<std::uint32_t>& neighbors,
                        void* buffer, std::size_t buffer_bytes) {
  require_exact_page(buffer_bytes);
  if (buffer == nullptr) {
    throw std::runtime_error("Disk record buffer is null");
  }
  if (vector == nullptr) {
    throw std::runtime_error("Disk record vector is null");
  }
  validate_disk_record_shape(dim, static_cast<std::uint32_t>(neighbors.size()));

  auto* bytes = static_cast<std::uint8_t*>(buffer);
  std::memset(bytes, 0, buffer_bytes);

  DiskNodeRecordHeader header;
  header.magic = kDiskNodeRecordMagic;
  header.node_id = node_id;
  header.degree = static_cast<std::uint16_t>(neighbors.size());
  header.dim = static_cast<std::uint16_t>(dim);
  header.flags = 0;
  header.vector_offset = sizeof(DiskNodeRecordHeader);
  header.neighbors_offset =
      header.vector_offset + static_cast<std::uint32_t>(dim * sizeof(float));
  header.payload_bytes =
      static_cast<std::uint32_t>(disk_record_bytes(dim, header.degree));
  header.checksum =
      payload_checksum(vector, dim, neighbors.data(), header.degree);

  std::memcpy(bytes, &header, sizeof(header));
  std::memcpy(bytes + header.vector_offset, vector,
              static_cast<std::size_t>(dim) * sizeof(float));
  if (!neighbors.empty()) {
    std::memcpy(bytes + header.neighbors_offset, neighbors.data(),
                neighbors.size() * sizeof(std::uint32_t));
  }
}

NodeRecord decode_node_record(const void* buffer, std::size_t buffer_bytes) {
  require_exact_page(buffer_bytes);
  if (buffer == nullptr) {
    throw std::runtime_error("Disk record buffer is null");
  }

  const auto* bytes = static_cast<const std::uint8_t*>(buffer);
  DiskNodeRecordHeader header;
  std::memcpy(&header, bytes, sizeof(header));

  if (header.magic != kDiskNodeRecordMagic) {
    throw std::runtime_error("Invalid disk node record magic");
  }
  validate_disk_record_shape(header.dim, header.degree);

  const std::uint32_t expected_vector_offset = sizeof(DiskNodeRecordHeader);
  const std::uint32_t expected_neighbors_offset =
      expected_vector_offset +
      static_cast<std::uint32_t>(header.dim * sizeof(float));
  const std::uint32_t expected_payload_bytes =
      static_cast<std::uint32_t>(disk_record_bytes(header.dim, header.degree));
  if (header.vector_offset != expected_vector_offset ||
      header.neighbors_offset != expected_neighbors_offset ||
      header.payload_bytes != expected_payload_bytes) {
    throw std::runtime_error("Disk node record layout is inconsistent");
  }

  const auto* vector_ptr =
      reinterpret_cast<const float*>(bytes + header.vector_offset);
  const auto* neighbor_ptr =
      reinterpret_cast<const std::uint32_t*>(bytes + header.neighbors_offset);
  const std::uint32_t checksum =
      payload_checksum(vector_ptr, header.dim, neighbor_ptr, header.degree);
  if (checksum != header.checksum) {
    throw std::runtime_error("Disk node record checksum mismatch");
  }

  NodeRecord record;
  record.node_id = header.node_id;
  record.degree = header.degree;
  record.dim = header.dim;
  record.vector.resize(record.dim);
  record.neighbors.resize(record.degree);
  std::memcpy(record.vector.data(), bytes + header.vector_offset,
              record.vector.size() * sizeof(float));
  if (!record.neighbors.empty()) {
    std::memcpy(record.neighbors.data(), bytes + header.neighbors_offset,
                record.neighbors.size() * sizeof(std::uint32_t));
  }
  return record;
}

}  // namespace agentmem
