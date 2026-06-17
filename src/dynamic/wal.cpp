#include "agentmem/dynamic/wal.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace agentmem::dynamic {
namespace {

constexpr std::uint32_t kWalRecordMagic = 0x41574c50;  // "PLWA", little endian.
constexpr std::uint16_t kWalRecordVersion = 1;
constexpr std::uint16_t kWalInsertOp = 1;
constexpr std::size_t kMaxPayloadBytes = 512ULL * 1024ULL * 1024ULL;

struct WalRecordHeader {
  std::uint32_t magic = kWalRecordMagic;
  std::uint16_t version = kWalRecordVersion;
  std::uint16_t op_type = kWalInsertOp;
  std::uint64_t sequence_id = 0;
  std::uint64_t node_id = 0;
  std::uint32_t dim = 0;
  std::uint32_t vector_bytes = 0;
  std::uint32_t neighbor_count = 0;
  std::uint8_t deleted = 0;
  std::uint32_t payload_checksum = 0;
};

std::uint32_t checksum_bytes(const unsigned char* data, std::size_t size) {
  std::uint32_t hash = 2166136261u;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

std::uint32_t checksum_payload(const std::vector<float>& vector,
                               const std::vector<NodeId>& neighbors) {
  std::uint32_t hash = 2166136261u;
  if (!vector.empty()) {
    hash = checksum_bytes(reinterpret_cast<const unsigned char*>(vector.data()),
                          vector.size() * sizeof(float));
  }
  if (!neighbors.empty()) {
    const auto neighbor_hash = checksum_bytes(
        reinterpret_cast<const unsigned char*>(neighbors.data()),
        neighbors.size() * sizeof(NodeId));
    hash ^= neighbor_hash + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
  }
  return hash;
}

template <typename T>
bool write_value(std::ofstream& output, const T& value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(T));
  return static_cast<bool>(output);
}

template <typename T>
bool read_value(std::ifstream& input, T& value) {
  input.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(input);
}

bool write_header(std::ofstream& output, const WalRecordHeader& header) {
  return write_value(output, header.magic) &&
         write_value(output, header.version) &&
         write_value(output, header.op_type) &&
         write_value(output, header.sequence_id) &&
         write_value(output, header.node_id) &&
         write_value(output, header.dim) &&
         write_value(output, header.vector_bytes) &&
         write_value(output, header.neighbor_count) &&
         write_value(output, header.deleted) &&
         write_value(output, header.payload_checksum);
}

bool read_header(std::ifstream& input, WalRecordHeader& header) {
  return read_value(input, header.magic) &&
         read_value(input, header.version) &&
         read_value(input, header.op_type) &&
         read_value(input, header.sequence_id) &&
         read_value(input, header.node_id) &&
         read_value(input, header.dim) &&
         read_value(input, header.vector_bytes) &&
         read_value(input, header.neighbor_count) &&
         read_value(input, header.deleted) &&
         read_value(input, header.payload_checksum);
}

bool header_shape_is_valid(const WalRecordHeader& header) {
  if (header.magic != kWalRecordMagic ||
      header.version != kWalRecordVersion || header.op_type != kWalInsertOp ||
      header.deleted > 1) {
    return false;
  }
  if (header.dim > std::numeric_limits<std::uint32_t>::max() / sizeof(float)) {
    return false;
  }
  if (header.vector_bytes != header.dim * sizeof(float)) {
    return false;
  }
  const auto payload_bytes =
      static_cast<std::uint64_t>(header.vector_bytes) +
      static_cast<std::uint64_t>(header.neighbor_count) * sizeof(NodeId);
  return payload_bytes <= kMaxPayloadBytes;
}

bool node_id_fits(NodeId node_id, std::uint64_t value) {
  return value <= std::numeric_limits<NodeId>::max() &&
         node_id == static_cast<NodeId>(value);
}

}  // namespace

WalWriter::WalWriter(const std::filesystem::path& path) : path_(path) {
  output_.open(path_, std::ios::binary | std::ios::app);
}

WalWriter::~WalWriter() {
  (void)close();
}

bool WalWriter::append(const DynamicRecord& record) {
  if (closed_ || !output_) {
    return false;
  }
  if (record.dim != record.vector.size() ||
      record.vector.size() > std::numeric_limits<std::uint32_t>::max() /
                                 sizeof(float) ||
      record.neighbors.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  WalRecordHeader header;
  header.sequence_id = record.sequence_id;
  header.node_id = record.node_id;
  header.dim = record.dim;
  header.vector_bytes =
      static_cast<std::uint32_t>(record.vector.size() * sizeof(float));
  header.neighbor_count = static_cast<std::uint32_t>(record.neighbors.size());
  header.deleted = record.deleted ? 1 : 0;
  header.payload_checksum = checksum_payload(record.vector, record.neighbors);

  if (!write_header(output_, header)) {
    return false;
  }
  if (!record.vector.empty()) {
    output_.write(reinterpret_cast<const char*>(record.vector.data()),
                  record.vector.size() * sizeof(float));
    if (!output_) {
      return false;
    }
  }
  if (!record.neighbors.empty()) {
    output_.write(reinterpret_cast<const char*>(record.neighbors.data()),
                  record.neighbors.size() * sizeof(NodeId));
    if (!output_) {
      return false;
    }
  }
  return true;
}

bool WalWriter::sync() {
  if (closed_ || !output_) {
    return false;
  }
  output_.flush();
  return static_cast<bool>(output_);
}

bool WalWriter::close() {
  if (closed_) {
    return true;
  }
  const bool ok = sync();
  output_.close();
  closed_ = true;
  return ok;
}

WalReader::WalReader(const std::filesystem::path& path) : path_(path) {}

std::vector<DynamicRecord> WalReader::replay() {
  std::vector<DynamicRecord> records;
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    return records;
  }

  while (input.peek() != std::char_traits<char>::eof()) {
    WalRecordHeader header;
    if (!read_header(input, header) || !header_shape_is_valid(header)) {
      break;
    }
    if (!node_id_fits(static_cast<NodeId>(header.node_id), header.node_id)) {
      break;
    }

    DynamicRecord record;
    record.sequence_id = header.sequence_id;
    record.node_id = static_cast<NodeId>(header.node_id);
    record.dim = header.dim;
    record.deleted = header.deleted != 0;
    record.vector.resize(header.dim);
    record.neighbors.resize(header.neighbor_count);

    if (header.vector_bytes > 0) {
      input.read(reinterpret_cast<char*>(record.vector.data()),
                 header.vector_bytes);
      if (!input) {
        break;
      }
    }
    if (header.neighbor_count > 0) {
      input.read(reinterpret_cast<char*>(record.neighbors.data()),
                 record.neighbors.size() * sizeof(NodeId));
      if (!input) {
        break;
      }
    }

    if (checksum_payload(record.vector, record.neighbors) !=
        header.payload_checksum) {
      break;
    }
    records.push_back(std::move(record));
  }

  return records;
}

}  // namespace agentmem::dynamic
