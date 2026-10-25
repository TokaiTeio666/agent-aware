#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "dynamic/dynamic_record.h"

namespace agent_aware::dynamic {

class WalWriter {
 public:
  explicit WalWriter(const std::filesystem::path& path);
  ~WalWriter();

  bool append(const DynamicRecord& record);
  bool append_batch(const std::vector<DynamicRecord>& records);
  bool sync();
  bool close();
  bool valid() const;

 private:
  std::filesystem::path path_;
  // 使用 C stdio 而不是裸 fd，使 WAL 在 Linux 与 Windows 上共用同一份实现。
  std::FILE* file_ = nullptr;
  bool closed_ = false;
};

struct WalReplayReport {
  std::vector<DynamicRecord> records;
  std::size_t replayed_records = 0;
  std::uint64_t last_sequence = 0;
  // 第一条坏记录的文件偏移。== 文件大小时表示干净读完。
  std::uint64_t truncated_bytes = 0;
  std::string reason;
  bool missing = false;
};

class WalReader {
 public:
  explicit WalReader(const std::filesystem::path& path);

  std::vector<DynamicRecord> replay();
  // 与 replay() 相同，但额外返回截断位置与原因，供恢复流程告警或截断使用。
  WalReplayReport replay_with_report();

 private:
  std::filesystem::path path_;
};

}  // namespace agent_aware::dynamic
