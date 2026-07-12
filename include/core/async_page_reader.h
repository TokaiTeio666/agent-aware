#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "core/io_stats.h"

namespace agent_aware {

struct DiskGraphSearchStats;

class AsyncPageReader {
 public:
  struct ReadRequest {
    std::uint32_t page_id = 0;
    std::uint64_t offset = 0;
    std::size_t bytes = 0;
    bool prefetch = false;
    std::uint64_t query_id = 0;
  };

  struct SubmittedRead {
    std::uint32_t page_id = 0;
    std::uint64_t token = 0;
    std::uint64_t query_id = 0;
  };

  struct CompletedRead {
    std::uint64_t token = 0;
    std::vector<char> data;
    std::uint64_t query_id = 0;
  };

  AsyncPageReader(std::string path, std::size_t page_size);
  ~AsyncPageReader();

  AsyncPageReader(const AsyncPageReader&) = delete;
  AsyncPageReader& operator=(const AsyncPageReader&) = delete;

  void configure(const std::string& mode, std::size_t batch_size,
                 std::size_t io_depth);
  const DiskGraphIoStatus& status() const;

  bool async_enabled() const;
  std::size_t max_pending_reads() const;
  std::size_t pending_async_reads() const;

  std::vector<char> read(std::uint64_t offset, std::size_t bytes,
                         DiskGraphSearchStats& stats);
  std::uint64_t start_async_read(std::uint64_t offset, std::size_t bytes,
                                 DiskGraphSearchStats& stats);
  // io_uring requests remain queued until submit_async_reads(); fallback
  // modes complete them inline so callers can keep the same token flow.
  std::vector<SubmittedRead> queue_reads(
      const std::vector<ReadRequest>& read_requests,
      DiskGraphSearchStats& stats);
  std::vector<SubmittedRead> batch_submit(
      const std::vector<ReadRequest>& read_requests,
      DiskGraphSearchStats& stats);
  void submit_async_reads(DiskGraphSearchStats& stats);
  bool reap_async_read(bool wait, CompletedRead& completed,
                       DiskGraphSearchStats& stats);
  bool reap_async_read(std::uint64_t query_id, bool wait,
                       CompletedRead& completed,
                       DiskGraphSearchStats& stats);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  mutable std::mutex mutex_;
  std::condition_variable completion_condition_;
  std::unordered_map<std::uint64_t, std::deque<CompletedRead>> deferred_;
};

}  // namespace agent_aware
