#pragma once

#include <cstddef>
#include <string>

namespace agentmem {

struct DiskGraphIoStatus {
  std::string requested_mode = "pread";
  std::string effective_mode = "pread";
  std::string fallback_reason;
  bool direct_enabled = false;
  bool io_uring_enabled = false;
  std::size_t batch_size = 1;
  std::size_t depth = 1;
};

struct P4IoStats {
  std::size_t ssd_reads = 0;
  std::size_t async_submits = 0;
  std::size_t async_completions = 0;
  std::size_t prefetch_issued = 0;
  std::size_t prefetch_used = 0;
  std::size_t prefetch_dropped = 0;
  std::size_t pending_reads = 0;
  std::size_t fallback_reads = 0;
  std::size_t sync_reads = 0;
  std::size_t dedup_hits = 0;
};

}  // namespace agentmem
