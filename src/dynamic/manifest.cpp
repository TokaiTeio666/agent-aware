#include "dynamic/manifest.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <regex>
#include <string>
#include <utility>

#include "core/platform_io.h"

namespace agent_aware::dynamic {
namespace {

// manifest 是动态层的提交记录；这里刻意使用很小的手写 JSON 格式，恢复时无需额外依赖。
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

  // 纯手写解析：不依赖 std::from_chars（需 <charconv>，且部分标准库实现与
  // iostream 头文件共存时整数重载会失效）或 std::stoll（受区域设置影响）。
  bool negative = false;
  std::size_t pos = value_pos;
  if (text[pos] == '-') {
    negative = true;
    ++pos;
  }
  if (pos >= value_end) {
    return false;
  }
  std::int64_t value = 0;
  for (; pos < value_end; ++pos) {
    const int digit = text[pos] - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  out = negative ? -value : value;
  return true;
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
    // manifest 缺失表示全新的动态目录，不是恢复错误。
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
  // wal_checkpoint 可能不存在：没有发生过 flush 的目录在 Checkpoint(0) 时
  // 不写 SSTable 就直接返回，因此 v1 文件里可能缺这个键，此时按 0 处理。
  // 其余三个键是必需字段，缺失说明文件被截断或格式不符。
  const bool has_checkpoint =
      parse_json_integer(text, "wal_checkpoint", checkpoint_value);
  if (!parse_json_integer(text, "version", version_value) ||
      version_value != kManifestVersion ||
      !parse_json_integer(text, "next_sequence_id", next_sequence_value) ||
      !parse_json_integer(text, "next_sstable_id", next_sstable_value) ||
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
    // 先写完整的下一版状态；后续 rename 是 DynamicWriteManager::open() 看到的提交点。
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
    // 先关闭流再重新打开做 fsync：否则缓冲区内容可能还没交给内核。
    output.close();
    if (!output) {
      return false;
    }
  }

  // 先把临时文件落盘，再 rename：顺序反过来会在崩溃窗口里留下一个
  // "名字已提交、内容未落盘"的 manifest。
  {
    std::FILE* file = std::fopen(tmp_path.c_str(), "rb");
    if (file == nullptr) {
      return false;
    }
    const bool synced = fsync_file(file);
    std::fclose(file);
    if (!synced) {
      return false;
    }
  }

  // Windows 上 rename 不能覆盖已存在的目标，因此"更新已有 manifest"时第一次
  // rename 必然失败，需要先删旧文件再 rename。窗口内 manifest 短暂缺失，但
  // tmp 文件已经 fsync，崩溃后最多退回"没有 manifest + 有 SSTable 文件"的
  // 旧目录语义，而不会引用半写文件。
  error.clear();
  std::filesystem::rename(tmp_path, path_, error);
  if (error) {
    error.clear();
    std::filesystem::remove(path_, error);
    error.clear();
    std::filesystem::rename(tmp_path, path_, error);
  }
  if (error) {
    return false;
  }
  // rename 只保证目录项可见性；目录本身还要 fsync 才算持久。
  return fsync_directory(path_.parent_path().string().c_str());
}

}  // namespace agent_aware::dynamic
