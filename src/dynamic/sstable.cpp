#include "agentmem/dynamic/sstable.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace agentmem::dynamic {
namespace {

constexpr std::uint32_t kSSTableDataMagic = 0x44535450;   // "PSTD".
constexpr std::uint32_t kSSTableIndexMagic = 0x49535450;  // "PSTI".
constexpr std::uint16_t kSSTableVersion = 1;
constexpr std::size_t kMaxRecordPayloadBytes = 512ULL * 1024ULL * 1024ULL;

struct DataRecordHeader {
  std::uint32_t magic = kSSTableDataMagic;
  std::uint16_t version = kSSTableVersion;
  std::uint8_t deleted = 0;
  std::uint64_t sequence_id = 0;
  std::uint64_t node_id = 0;
  std::uint32_t dim = 0;
  std::uint32_t vector_bytes = 0;
  std::uint32_t neighbor_count = 0;
  std::uint32_t payload_checksum = 0;
};

std::string sstable_name(std::uint64_t sstable_id) {
  std::ostringstream name;
  name << "sst_" << std::setw(6) << std::setfill('0') << sstable_id;
  return name.str();
}

std::filesystem::path with_extension(std::filesystem::path path,
                                     const std::string& extension) {
  path.replace_extension(extension);
  return path;
}

std::filesystem::path normalize_base_path(const std::filesystem::path& path) {
  const auto extension = path.extension().string();
  if (extension == ".data" || extension == ".index" || extension == ".meta") {
    auto base = path;
    base.replace_extension();
    return base;
  }
  return path;
}

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

bool write_data_header(std::ofstream& output, const DataRecordHeader& header) {
  return write_value(output, header.magic) &&
         write_value(output, header.version) &&
         write_value(output, header.deleted) &&
         write_value(output, header.sequence_id) &&
         write_value(output, header.node_id) &&
         write_value(output, header.dim) &&
         write_value(output, header.vector_bytes) &&
         write_value(output, header.neighbor_count) &&
         write_value(output, header.payload_checksum);
}

bool read_data_header(std::ifstream& input, DataRecordHeader& header) {
  return read_value(input, header.magic) && read_value(input, header.version) &&
         read_value(input, header.deleted) &&
         read_value(input, header.sequence_id) &&
         read_value(input, header.node_id) && read_value(input, header.dim) &&
         read_value(input, header.vector_bytes) &&
         read_value(input, header.neighbor_count) &&
         read_value(input, header.payload_checksum);
}

bool data_header_is_valid(const DataRecordHeader& header) {
  if (header.magic != kSSTableDataMagic ||
      header.version != kSSTableVersion || header.deleted > 1) {
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
  if (payload_bytes > kMaxRecordPayloadBytes) {
    return false;
  }
  return header.node_id <= std::numeric_limits<NodeId>::max();
}

bool write_record(std::ofstream& output, const DynamicRecord& record) {
  if (record.dim != record.vector.size() ||
      record.vector.size() > std::numeric_limits<std::uint32_t>::max() /
                                 sizeof(float) ||
      record.neighbors.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  DataRecordHeader header;
  header.deleted = record.deleted ? 1 : 0;
  header.sequence_id = record.sequence_id;
  header.node_id = record.node_id;
  header.dim = record.dim;
  header.vector_bytes =
      static_cast<std::uint32_t>(record.vector.size() * sizeof(float));
  header.neighbor_count = static_cast<std::uint32_t>(record.neighbors.size());
  header.payload_checksum = checksum_payload(record.vector, record.neighbors);

  if (!write_data_header(output, header)) {
    return false;
  }
  if (!record.vector.empty()) {
    output.write(reinterpret_cast<const char*>(record.vector.data()),
                 record.vector.size() * sizeof(float));
    if (!output) {
      return false;
    }
  }
  if (!record.neighbors.empty()) {
    output.write(reinterpret_cast<const char*>(record.neighbors.data()),
                 record.neighbors.size() * sizeof(NodeId));
    if (!output) {
      return false;
    }
  }
  return true;
}

bool read_record_at(const std::filesystem::path& data_path,
                    std::uint64_t offset, DynamicRecord& out) {
  std::ifstream input(data_path, std::ios::binary);
  if (!input) {
    return false;
  }
  input.seekg(static_cast<std::streamoff>(offset));
  if (!input) {
    return false;
  }

  DataRecordHeader header;
  if (!read_data_header(input, header) || !data_header_is_valid(header)) {
    return false;
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
      return false;
    }
  }
  if (header.neighbor_count > 0) {
    input.read(reinterpret_cast<char*>(record.neighbors.data()),
               record.neighbors.size() * sizeof(NodeId));
    if (!input) {
      return false;
    }
  }
  if (checksum_payload(record.vector, record.neighbors) !=
      header.payload_checksum) {
    return false;
  }

  out = std::move(record);
  return true;
}

std::vector<DynamicRecord> deduplicate_records(
    const std::vector<DynamicRecord>& records) {
  std::unordered_map<NodeId, DynamicRecord> latest;
  latest.reserve(records.size());
  for (const auto& record : records) {
    const auto found = latest.find(record.node_id);
    if (found == latest.end() ||
        record.sequence_id > found->second.sequence_id) {
      latest[record.node_id] = record;
    }
  }

  std::vector<DynamicRecord> result;
  result.reserve(latest.size());
  for (const auto& item : latest) {
    result.push_back(item.second);
  }
  std::sort(result.begin(), result.end(),
            [](const DynamicRecord& lhs, const DynamicRecord& rhs) {
              return lhs.node_id < rhs.node_id;
            });
  return result;
}

bool write_index_header(std::ofstream& output, std::size_t record_count) {
  const auto count = static_cast<std::uint64_t>(record_count);
  return write_value(output, kSSTableIndexMagic) &&
         write_value(output, kSSTableVersion) && write_value(output, count);
}

bool read_index_header(std::ifstream& input, std::uint64_t& record_count) {
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  if (!read_value(input, magic) || !read_value(input, version) ||
      !read_value(input, record_count)) {
    return false;
  }
  return magic == kSSTableIndexMagic && version == kSSTableVersion;
}

bool write_index_entry(std::ofstream& output, NodeId node_id,
                       std::uint64_t sequence_id, std::uint64_t offset) {
  const std::uint64_t stored_node_id = node_id;
  return write_value(output, stored_node_id) &&
         write_value(output, sequence_id) && write_value(output, offset);
}

bool write_meta(const std::filesystem::path& meta_path, std::uint64_t id,
                int level, std::size_t record_count,
                std::uint64_t min_sequence_id,
                std::uint64_t max_sequence_id) {
  std::ofstream output(meta_path, std::ios::trunc);
  if (!output) {
    return false;
  }
  output << "{\n"
         << "  \"magic\": \"AM_P5_SSTABLE\",\n"
         << "  \"version\": " << kSSTableVersion << ",\n"
         << "  \"sstable_id\": " << id << ",\n"
         << "  \"level\": " << level << ",\n"
         << "  \"record_count\": " << record_count << ",\n"
         << "  \"min_sequence_id\": " << min_sequence_id << ",\n"
         << "  \"max_sequence_id\": " << max_sequence_id << "\n"
         << "}\n";
  return static_cast<bool>(output);
}

bool parse_json_integer(const std::string& text, const std::string& key,
                        std::int64_t& out) {
  const auto key_pos = text.find("\"" + key + "\"");
  if (key_pos == std::string::npos) {
    return false;
  }
  const auto colon_pos = text.find(':', key_pos);
  if (colon_pos == std::string::npos) {
    return false;
  }

  std::size_t value_pos = colon_pos + 1;
  while (value_pos < text.size() &&
         std::isspace(static_cast<unsigned char>(text[value_pos])) != 0) {
    ++value_pos;
  }

  std::size_t value_end = value_pos;
  if (value_end < text.size() && text[value_end] == '-') {
    ++value_end;
  }
  while (value_end < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[value_end])) != 0) {
    ++value_end;
  }
  if (value_end == value_pos) {
    return false;
  }

  const auto* begin = text.data() + value_pos;
  const auto* end = text.data() + value_end;
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc{} && result.ptr == end;
}

bool load_meta(const std::filesystem::path& meta_path, std::uint64_t& id,
               int& level, std::size_t& record_count,
               std::uint64_t& min_sequence_id,
               std::uint64_t& max_sequence_id) {
  std::ifstream input(meta_path);
  if (!input) {
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  if (text.find("\"AM_P5_SSTABLE\"") == std::string::npos) {
    return false;
  }

  std::int64_t version_value = 0;
  std::int64_t id_value = 0;
  std::int64_t level_value = 0;
  std::int64_t count_value = 0;
  std::int64_t min_value = 0;
  std::int64_t max_value = 0;
  if (!parse_json_integer(text, "version", version_value) ||
      version_value != kSSTableVersion ||
      !parse_json_integer(text, "sstable_id", id_value) ||
      !parse_json_integer(text, "level", level_value) ||
      !parse_json_integer(text, "record_count", count_value) ||
      !parse_json_integer(text, "min_sequence_id", min_value) ||
      !parse_json_integer(text, "max_sequence_id", max_value) ||
      id_value < 0 || count_value < 0 || min_value < 0 || max_value < 0 ||
      level_value < std::numeric_limits<int>::min() ||
      level_value > std::numeric_limits<int>::max()) {
    return false;
  }

  id = static_cast<std::uint64_t>(id_value);
  level = static_cast<int>(level_value);
  record_count = static_cast<std::size_t>(count_value);
  min_sequence_id = static_cast<std::uint64_t>(min_value);
  max_sequence_id = static_cast<std::uint64_t>(max_value);
  return true;
}

}  // namespace

SSTableWriter::SSTableWriter(const std::filesystem::path& dir,
                             std::uint64_t sstable_id, int level)
    : dir_(dir), sstable_id_(sstable_id), level_(level) {}

bool SSTableWriter::write(const std::vector<DynamicRecord>& records) {
  std::error_code error;
  std::filesystem::create_directories(dir_, error);
  if (error) {
    return false;
  }

  const auto base_path = dir_ / sstable_name(sstable_id_);
  const auto data_path = with_extension(base_path, ".data");
  const auto index_path = with_extension(base_path, ".index");
  const auto meta_path = with_extension(base_path, ".meta");
  const auto latest_records = deduplicate_records(records);

  std::uint64_t min_sequence_id = 0;
  std::uint64_t max_sequence_id = 0;
  if (!latest_records.empty()) {
    min_sequence_id = latest_records.front().sequence_id;
    max_sequence_id = latest_records.front().sequence_id;
    for (const auto& record : latest_records) {
      min_sequence_id = std::min(min_sequence_id, record.sequence_id);
      max_sequence_id = std::max(max_sequence_id, record.sequence_id);
    }
  }

  std::ofstream data_output(data_path, std::ios::binary | std::ios::trunc);
  std::ofstream index_output(index_path, std::ios::binary | std::ios::trunc);
  if (!data_output || !index_output ||
      !write_index_header(index_output, latest_records.size())) {
    return false;
  }

  for (const auto& record : latest_records) {
    const auto offset = static_cast<std::uint64_t>(data_output.tellp());
    if (!write_record(data_output, record) ||
        !write_index_entry(index_output, record.node_id, record.sequence_id,
                           offset)) {
      return false;
    }
  }
  data_output.flush();
  index_output.flush();
  if (!data_output || !index_output) {
    return false;
  }

  return write_meta(meta_path, sstable_id_, level_, latest_records.size(),
                    min_sequence_id, max_sequence_id);
}

SSTableReader::SSTableReader(const std::filesystem::path& base_path)
    : base_path_(normalize_base_path(base_path)),
      data_path_(with_extension(base_path_, ".data")),
      index_path_(with_extension(base_path_, ".index")),
      meta_path_(with_extension(base_path_, ".meta")) {}

bool SSTableReader::open() {
  index_.clear();
  opened_ = false;

  if (!load_meta(meta_path_, id_, level_, record_count_, min_sequence_id_,
                 max_sequence_id_)) {
    return false;
  }

  std::ifstream index_input(index_path_, std::ios::binary);
  if (!index_input) {
    return false;
  }

  std::uint64_t stored_count = 0;
  if (!read_index_header(index_input, stored_count) ||
      stored_count != record_count_) {
    return false;
  }

  for (std::uint64_t i = 0; i < stored_count; ++i) {
    std::uint64_t node_id = 0;
    std::uint64_t sequence_id = 0;
    std::uint64_t offset = 0;
    if (!read_value(index_input, node_id) ||
        !read_value(index_input, sequence_id) ||
        !read_value(index_input, offset) ||
        node_id > std::numeric_limits<NodeId>::max()) {
      return false;
    }
    index_[static_cast<NodeId>(node_id)] = IndexEntry{sequence_id, offset};
  }

  std::ifstream data_input(data_path_, std::ios::binary);
  if (!data_input) {
    return false;
  }

  opened_ = true;
  return true;
}

bool SSTableReader::get(NodeId node_id, DynamicRecord& out) const {
  if (!opened_) {
    return false;
  }
  const auto found = index_.find(node_id);
  if (found == index_.end()) {
    return false;
  }
  DynamicRecord record;
  if (!read_record_at(data_path_, found->second.offset, record) ||
      record.node_id != node_id ||
      record.sequence_id != found->second.sequence_id) {
    return false;
  }
  out = std::move(record);
  return true;
}

std::vector<DynamicRecord> SSTableReader::scan_all() const {
  std::vector<DynamicRecord> records;
  if (!opened_) {
    return records;
  }
  records.reserve(index_.size());
  for (const auto& item : index_) {
    DynamicRecord record;
    if (get(item.first, record)) {
      records.push_back(std::move(record));
    }
  }
  std::sort(records.begin(), records.end(),
            [](const DynamicRecord& lhs, const DynamicRecord& rhs) {
              return lhs.node_id < rhs.node_id;
            });
  return records;
}

std::uint64_t SSTableReader::id() const {
  return id_;
}

int SSTableReader::level() const {
  return level_;
}

std::size_t SSTableReader::record_count() const {
  return record_count_;
}

std::uint64_t SSTableReader::max_sequence_id() const {
  return max_sequence_id_;
}

}  // namespace agentmem::dynamic
