#pragma once

#include <cstddef>
#include <string>

namespace agent_aware {

struct DiskGraphIoStatus {
  std::string requested_mode = "pread";
  std::string effective_mode = "pread";
  std::string fallback_reason;
  bool direct_enabled = false;
  bool io_uring_enabled = false;
  std::size_t batch_size = 1;
  std::size_t depth = 1;
};

}  // namespace agent_aware
