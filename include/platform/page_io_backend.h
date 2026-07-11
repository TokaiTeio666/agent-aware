#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agent_aware::platform {

using IoToken = std::uint64_t;
enum class IoPriority : std::uint8_t { kDemandRead = 0, kWrite = 1, kPrefetch = 2 };

struct PageReadRequest {
  std::uint32_t page_id = 0;
  std::uint64_t offset = 0;
  std::size_t bytes = 0;
  IoPriority priority = IoPriority::kDemandRead;
};

struct PageReadResult {
  IoToken token = 0;
  std::uint32_t page_id = 0;
  bool ok = false;
  std::string error;
  std::vector<char> data;
};

struct PageIoStatus {
  std::string requested_mode;
  std::string effective_mode;
  std::string fallback_reason;
  std::size_t queue_capacity = 0;
  std::size_t queue_depth = 0;
  std::size_t pending = 0;
};

class PageIoBackend {
 public:
  virtual ~PageIoBackend() = default;
  virtual bool open(const std::string& path, std::size_t page_size,
                    std::size_t queue_depth, std::string& error) = 0;
  virtual PageReadResult read(std::uint64_t offset, std::size_t bytes) = 0;
  virtual std::vector<IoToken> submit(
      const std::vector<PageReadRequest>& requests, std::string& error) = 0;
  virtual PageReadResult wait(IoToken token, std::uint32_t timeout_ms) = 0;
  virtual bool cancel(IoToken token) = 0;
  virtual PageIoStatus status() const = 0;
  virtual void close() = 0;
};

std::unique_ptr<PageIoBackend> make_thread_pool_pread_backend();

}  // namespace agent_aware::platform
