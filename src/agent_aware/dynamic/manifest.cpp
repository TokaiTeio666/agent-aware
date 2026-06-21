#include "agent_aware/dynamic/manifest.h"

#include <charconv>
#include <cctype>
#include <fstream>
#include <regex>
#include <string>
#include <utility>

namespace agent_aware::dynamic {
namespace {

constexpr std::uint32_t kManifestVersion = 1;

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

std::string escape_json_string(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

}  // namespace

Manifest::Manifest(std::filesystem::path path) : path_(std::move(path)) {}

bool Manifest::load(ManifestData& data) const {
  if (!std::filesystem::exists(path_)) {
    data = ManifestData{};
    return true;
  }

  std::ifstream input(path_);
  if (!input) {
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());

  std::int64_t version_value = 0;
  std::int64_t next_sequence_value = 0;
  std::int64_t next_sstable_value = 0;
  std::int64_t checkpoint_value = 0;
  if (!parse_json_integer(text, "version", version_value) ||
      version_value != kManifestVersion ||
      !parse_json_integer(text, "next_sequence_id", next_sequence_value) ||
      !parse_json_integer(text, "next_sstable_id", next_sstable_value) ||
      !parse_json_integer(text, "wal_checkpoint", checkpoint_value) ||
      next_sequence_value <= 0 || next_sstable_value <= 0 ||
      checkpoint_value < 0) {
    return false;
  }

  ManifestData loaded;
  loaded.version = static_cast<std::uint32_t>(version_value);
  loaded.next_sequence_id = static_cast<std::uint64_t>(next_sequence_value);
  loaded.next_sstable_id = static_cast<std::uint64_t>(next_sstable_value);
  loaded.wal_checkpoint = static_cast<std::uint64_t>(checkpoint_value);

  const std::regex entry_pattern(
      R"manifest(\{\s*"id"\s*:\s*([0-9]+)\s*,\s*"level"\s*:\s*(-?[0-9]+)\s*,\s*"base_path"\s*:\s*"([^"]*)"\s*\})manifest");
  for (auto it = std::sregex_iterator(text.begin(), text.end(), entry_pattern);
       it != std::sregex_iterator(); ++it) {
    const auto& match = *it;
    ManifestSSTableEntry entry;
    entry.id = static_cast<std::uint64_t>(std::stoull(match[1].str()));
    entry.level = std::stoi(match[2].str());
    entry.base_path = match[3].str();
    loaded.sstables.push_back(std::move(entry));
  }

  data = std::move(loaded);
  return true;
}

bool Manifest::save(const ManifestData& data) const {
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error) {
    return false;
  }

  const auto tmp_path = path_.string() + ".tmp";
  {
    std::ofstream output(tmp_path, std::ios::trunc);
    if (!output) {
      return false;
    }
    output << "{\n";
    output << "  \"version\": " << kManifestVersion << ",\n";
    output << "  \"next_sequence_id\": " << data.next_sequence_id << ",\n";
    output << "  \"next_sstable_id\": " << data.next_sstable_id << ",\n";
    output << "  \"sstables\": [\n";
    for (std::size_t i = 0; i < data.sstables.size(); ++i) {
      const auto& entry = data.sstables[i];
      output << "    {\n";
      output << "      \"id\": " << entry.id << ",\n";
      output << "      \"level\": " << entry.level << ",\n";
      output << "      \"base_path\": \""
             << escape_json_string(entry.base_path.generic_string())
             << "\"\n";
      output << "    }" << (i + 1 == data.sstables.size() ? "\n" : ",\n");
    }
    output << "  ],\n";
    output << "  \"wal_checkpoint\": " << data.wal_checkpoint << "\n";
    output << "}\n";
    output.flush();
    if (!output) {
      return false;
    }
  }

  error.clear();
  std::filesystem::rename(tmp_path, path_, error);
  if (!error) {
    return true;
  }

  error.clear();
  std::filesystem::remove(path_, error);
  error.clear();
  std::filesystem::rename(tmp_path, path_, error);
  return !error;
}

}  // namespace agent_aware::dynamic
