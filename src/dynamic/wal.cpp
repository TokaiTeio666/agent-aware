#include "dynamic/wal.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <regex>
#include <unistd.h>

namespace agent_aware::dynamic {
namespace {

constexpr std::uint32_t kWalRecordMagic = 0x41574c50;
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
void append_value(std::vector<char>& output, const T& value) {
  const auto* bytes = reinterpret_cast<const char*>(&value);
  output.insert(output.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool read_value(std::ifstream& input, T& value) {
  input.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(input);
}

bool encode_record(const MaterializedRecord& record, std::vector<char>& output) {
  if (record.dim != record.vector.size() ||
      record.vector.size() > std::numeric_limits<std::uint32_t>::max() /
                                 sizeof(float)) {
    return false;
  }

  WalRecordHeader header;
  header.sequence_id = record.sequence_id;
  header.node_id = record.node_id;
  header.dim = record.dim;
  header.vector_bytes =
      static_cast<std::uint32_t>(record.vector.size() * sizeof(float));
  header.neighbor_count = 0;
  header.deleted = record.deleted ? 1 : 0;
  header.payload_checksum = checksum_payload(record.vector, {});

  append_value(output, header.magic);
  append_value(output, header.version);
  append_value(output, header.op_type);
  append_value(output, header.sequence_id);
  append_value(output, header.node_id);
  append_value(output, header.dim);
  append_value(output, header.vector_bytes);
  append_value(output, header.neighbor_count);
  append_value(output, header.deleted);
  append_value(output, header.payload_checksum);
  if (!record.vector.empty()) {
    const auto* begin = reinterpret_cast<const char*>(record.vector.data());
    output.insert(output.end(), begin, begin + header.vector_bytes);
  }
  return true;
}

bool write_all(int fd, const char* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const auto result = ::write(fd, data + written, size - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    written += static_cast<std::size_t>(result);
  }
  return true;
}

bool read_header(std::ifstream& input, WalRecordHeader& header) {
  return read_value(input, header.magic) &&
         read_value(input, header.version) &&
         read_value(input, header.op_type) &&
         read_value(input, header.sequence_id) &&
         read_value(input, header.node_id) && read_value(input, header.dim) &&
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

std::uint64_t segment_id_from_path(const std::filesystem::path& path) {
  std::smatch match;
  const std::regex pattern("wal_([0-9]+)\\.log");
  const auto name = path.filename().string();
  return std::regex_match(name, match, pattern)
             ? static_cast<std::uint64_t>(std::stoull(match[1]))
             : 0;
}

}  // namespace

WalWriter::WalWriter(const std::filesystem::path& path,
                     std::uint64_t segment_id)
    : path_(path), segment_id_(segment_id) {
  fd_ = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
}

WalWriter::~WalWriter() { (void)close(); }

bool WalWriter::append(const MaterializedRecord& record) {
  return append_batch(std::vector<MaterializedRecord>{record});
}

bool WalWriter::append_batch(const std::vector<MaterializedRecord>& records) {
  std::vector<DynamicLocator> ignored;
  return append_batch(records, ignored);
}

bool WalWriter::append_batch(const std::vector<MaterializedRecord>& records,
                             std::vector<DynamicLocator>& locators) {
  if (closed_ || fd_ < 0) {
    return false;
  }
  std::vector<char> encoded;
  std::vector<std::pair<std::size_t, std::uint32_t>> spans;
  spans.reserve(records.size());
  for (const auto& record : records) {
    const std::size_t begin = encoded.size();
    if (!encode_record(record, encoded)) {
      return false;
    }
    spans.emplace_back(begin,
                       static_cast<std::uint32_t>(encoded.size() - begin));
  }
  const off_t start = ::lseek(fd_, 0, SEEK_END);
  if (start < 0 || (!encoded.empty() &&
                    !write_all(fd_, encoded.data(), encoded.size()))) {
    return false;
  }
  locators.clear();
  locators.reserve(spans.size());
  for (const auto& span : spans) {
    locators.push_back(DynamicLocator{
        DynamicStorageKind::kWal, segment_id_,
        static_cast<std::uint64_t>(start) + span.first, span.second, 0});
  }
  return true;
}

bool WalWriter::sync() {
  if (closed_ || fd_ < 0) {
    return false;
  }
  while (::fdatasync(fd_) != 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

bool WalWriter::close() {
  if (closed_) {
    return true;
  }
  bool ok = fd_ >= 0 && sync();
  if (fd_ >= 0 && ::close(fd_) != 0) {
    ok = false;
  }
  fd_ = -1;
  closed_ = true;
  return ok;
}

bool WalWriter::valid() const { return !closed_ && fd_ >= 0; }

WalReader::WalReader(const std::filesystem::path& path) : path_(path) {}

std::vector<MaterializedRecord> WalReader::replay() {
  std::vector<MaterializedRecord> records;
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    return records;
  }

  while (input.peek() != std::char_traits<char>::eof()) {
    const auto record_offset = static_cast<std::uint64_t>(input.tellg());
    WalRecordHeader header;
    if (!read_header(input, header) || !header_shape_is_valid(header)) {
      break;
    }
    if (!node_id_fits(static_cast<NodeId>(header.node_id), header.node_id)) {
      break;
    }

    MaterializedRecord record;
    record.sequence_id = header.sequence_id;
    record.node_id = static_cast<NodeId>(header.node_id);
    record.dim = header.dim;
    record.deleted = header.deleted != 0;
    record.vector.resize(header.dim);
    std::vector<NodeId> legacy_neighbors(header.neighbor_count);

    if (header.vector_bytes > 0) {
      input.read(reinterpret_cast<char*>(record.vector.data()),
                 header.vector_bytes);
      if (!input) {
        break;
      }
    }
    if (header.neighbor_count > 0) {
      input.read(reinterpret_cast<char*>(legacy_neighbors.data()),
                 legacy_neighbors.size() * sizeof(NodeId));
      if (!input) {
        break;
      }
    }
    if (checksum_payload(record.vector, legacy_neighbors) !=
        header.payload_checksum) {
      break;
    }
    const auto record_end = static_cast<std::uint64_t>(input.tellg());
    record.locator = DynamicLocator{
        DynamicStorageKind::kWal, segment_id_from_path(path_), record_offset,
        static_cast<std::uint32_t>(record_end - record_offset), 0};
    records.push_back(std::move(record));
  }
  return records;
}

bool WalReader::read_at(const DynamicLocator& locator,
                        MaterializedRecord& out) const {
  if (locator.kind != DynamicStorageKind::kWal) return false;
  std::ifstream input(path_, std::ios::binary);
  if (!input) return false;
  input.seekg(static_cast<std::streamoff>(locator.offset));
  if (!input) return false;
  WalRecordHeader header;
  if (!read_header(input, header) || !header_shape_is_valid(header) ||
      !node_id_fits(static_cast<NodeId>(header.node_id), header.node_id)) {
    return false;
  }
  MaterializedRecord record;
  record.sequence_id = header.sequence_id;
  record.node_id = static_cast<NodeId>(header.node_id);
  record.dim = header.dim;
  record.deleted = header.deleted != 0;
  record.vector.resize(header.dim);
  std::vector<NodeId> legacy_neighbors(header.neighbor_count);
  if (header.vector_bytes > 0) {
    input.read(reinterpret_cast<char*>(record.vector.data()),
               header.vector_bytes);
  }
  if (header.neighbor_count > 0) {
    input.read(reinterpret_cast<char*>(legacy_neighbors.data()),
               legacy_neighbors.size() * sizeof(NodeId));
  }
  if (!input || checksum_payload(record.vector, legacy_neighbors) !=
                    header.payload_checksum) {
    return false;
  }
  record.locator = locator;
  out = std::move(record);
  return true;
}

}  // namespace agent_aware::dynamic
