#include "dynamic/wal.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>

#include "core/platform_io.h"

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

// 校验覆盖解码所需的全部头部字段与载荷：tombstone 位、node id、sequence
// 或 dim 翻转都会被识别为坏记录，而不是被当作有效写入重放。
std::uint32_t checksum_record(const WalRecordHeader& header,
                              const std::vector<float>& vector,
                              const std::vector<NodeId>& neighbors) {
  std::uint32_t hash = 2166136261u;
  auto fold = [&hash](const void* data, std::size_t size) {
    const std::uint32_t part = checksum_bytes(
        static_cast<const unsigned char*>(data), size);
    hash ^= part + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
  };
  fold(&header.op_type, sizeof(header.op_type));
  fold(&header.sequence_id, sizeof(header.sequence_id));
  fold(&header.node_id, sizeof(header.node_id));
  fold(&header.dim, sizeof(header.dim));
  fold(&header.vector_bytes, sizeof(header.vector_bytes));
  fold(&header.neighbor_count, sizeof(header.neighbor_count));
  fold(&header.deleted, sizeof(header.deleted));
  if (!vector.empty()) {
    fold(vector.data(), vector.size() * sizeof(float));
  }
  if (!neighbors.empty()) {
    fold(neighbors.data(), neighbors.size() * sizeof(NodeId));
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

bool encode_record(const DynamicRecord& record, std::vector<char>& output) {
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
  header.payload_checksum =
      checksum_record(header, record.vector, record.neighbors);

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
  if (!record.neighbors.empty()) {
    const auto* begin = reinterpret_cast<const char*>(record.neighbors.data());
    output.insert(output.end(), begin,
                  begin + record.neighbors.size() * sizeof(NodeId));
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

}  // namespace

WalWriter::WalWriter(const std::filesystem::path& path) : path_(path) {
  file_ = std::fopen(path_.string().c_str(), "ab");
}

WalWriter::~WalWriter() { (void)close(); }

bool WalWriter::append(const DynamicRecord& record) {
  return append_batch(std::vector<DynamicRecord>{record});
}

bool WalWriter::append_batch(const std::vector<DynamicRecord>& records) {
  if (closed_ || file_ == nullptr) {
    return false;
  }
  std::vector<char> encoded;
  for (const auto& record : records) {
    if (!encode_record(record, encoded)) {
      return false;
    }
  }
  return encoded.empty() ||
         std::fwrite(encoded.data(), 1, encoded.size(), file_) ==
             encoded.size();
}

bool WalWriter::sync() {
  if (closed_ || file_ == nullptr) {
    return false;
  }
  return std::fflush(file_) == 0 && fsync_file(file_);
}

bool WalWriter::close() {
  if (closed_) {
    return true;
  }
  bool ok = file_ != nullptr && sync();
  if (file_ != nullptr && std::fclose(file_) != 0) {
    ok = false;
  }
  file_ = nullptr;
  closed_ = true;
  return ok;
}

bool WalWriter::valid() const { return !closed_ && file_ != nullptr; }

WalReader::WalReader(const std::filesystem::path& path) : path_(path) {}

WalReplayReport WalReader::replay_with_report() {
  WalReplayReport report;
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    report.missing = true;
    return report;
  }

  // 逐条严格解码；一旦遇到坏记录就停止，并把原因与截断位置暴露给调用方，
  // 而不是静默丢弃其后的已提交记录。
  std::uint64_t offset = 0;
  while (input.peek() != std::char_traits<char>::eof()) {
    const std::uint64_t record_start = offset;
    WalRecordHeader header;
    if (!read_header(input, header)) {
      report.truncated_bytes = record_start;
      report.reason = "incomplete_record_header";
      break;
    }
    if (!header_shape_is_valid(header)) {
      report.truncated_bytes = record_start;
      report.reason = "invalid_record_header";
      break;
    }
    if (!node_id_fits(static_cast<NodeId>(header.node_id), header.node_id)) {
      report.truncated_bytes = record_start;
      report.reason = "node_id_out_of_range";
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
        report.truncated_bytes = record_start;
        report.reason = "incomplete_vector_payload";
        break;
      }
    }
    if (header.neighbor_count > 0) {
      input.read(reinterpret_cast<char*>(record.neighbors.data()),
                 record.neighbors.size() * sizeof(NodeId));
      if (!input) {
        report.truncated_bytes = record_start;
        report.reason = "incomplete_neighbor_payload";
        break;
      }
    }
    if (checksum_record(header, record.vector, record.neighbors) !=
        header.payload_checksum) {
      report.truncated_bytes = record_start;
      report.reason = "payload_checksum_mismatch";
      break;
    }
    // 记录偏移，供截断定位使用。
    offset = static_cast<std::uint64_t>(input.tellg());
    report.records.push_back(std::move(record));
  }
  report.replayed_records = report.records.size();
  report.last_sequence =
      report.records.empty() ? 0 : report.records.back().sequence_id;
  return report;
}

std::vector<DynamicRecord> WalReader::replay() {
  return replay_with_report().records;
}

}  // namespace agent_aware::dynamic
