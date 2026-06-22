#include "agent_aware/core/async_page_reader.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "agent_aware/graph/disk_graph_index.h"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef AGENT_AWARE_HAS_LIBURING
#define AGENT_AWARE_HAS_LIBURING 0
#endif

#if defined(__linux__) && AGENT_AWARE_HAS_LIBURING
#include <liburing.h>
#define AGENT_AWARE_ASYNC_CAN_USE_LIBURING 1
#else
#define AGENT_AWARE_ASYNC_CAN_USE_LIBURING 0
#endif

namespace agent_aware {
namespace {

#ifdef __linux__
constexpr std::size_t kDirectIoAlignment = 4096;

std::string errno_reason(const std::string& prefix) {
  return prefix + "_" + std::to_string(errno);
}
#endif

void record_pending_peak(DiskGraphSearchStats& stats,
                         std::size_t pending_reads) {
  stats.p4_io.pending_reads =
      std::max(stats.p4_io.pending_reads, pending_reads);
}

class AlignedBufferPool {
 public:
  AlignedBufferPool() = default;

  AlignedBufferPool(std::size_t count, std::size_t bytes,
                    std::size_t alignment) {
    reset(count, bytes, alignment);
  }

  ~AlignedBufferPool() {
    clear();
  }

  AlignedBufferPool(const AlignedBufferPool&) = delete;
  AlignedBufferPool& operator=(const AlignedBufferPool&) = delete;

  void reset(std::size_t count, std::size_t bytes, std::size_t alignment) {
    clear();
    if (count == 0 || bytes == 0) {
      return;
    }
    bytes_ = bytes;
    alignment_ = alignment;
    buffers_.reserve(count);
    free_list_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      void* block = nullptr;
#ifdef __linux__
      if (posix_memalign(&block, alignment_, bytes_) != 0) {
        clear();
        throw std::runtime_error("Failed to allocate aligned I/O buffer pool");
      }
#else
      block = std::malloc(bytes_);
      if (block == nullptr) {
        clear();
        throw std::runtime_error("Failed to allocate graph I/O buffer pool");
      }
#endif
      buffers_.push_back(block);
      free_list_.push_back(i);
    }
  }

  void clear() {
    for (void* buffer : buffers_) {
      std::free(buffer);
    }
    buffers_.clear();
    free_list_.clear();
    bytes_ = 0;
    alignment_ = 0;
  }

  bool acquire(std::size_t& index, void*& buffer) {
    if (free_list_.empty()) {
      return false;
    }
    index = free_list_.back();
    free_list_.pop_back();
    buffer = buffers_[index];
    return true;
  }

  void release(std::size_t index) {
    if (index >= buffers_.size()) {
      throw std::runtime_error("Invalid graph I/O buffer pool release");
    }
    free_list_.push_back(index);
  }

  std::size_t capacity() const {
    return buffers_.size();
  }

 private:
  std::vector<void*> buffers_;
  std::vector<std::size_t> free_list_;
  std::size_t bytes_ = 0;
  std::size_t alignment_ = 0;
};

#if AGENT_AWARE_ASYNC_CAN_USE_LIBURING
class LibUring {
 public:
  LibUring() = default;

  ~LibUring() {
    close();
  }

  LibUring(const LibUring&) = delete;
  LibUring& operator=(const LibUring&) = delete;

  struct Completion {
    std::uint64_t user_data = 0;
    int result = 0;
  };

  bool open(std::size_t entries, std::string& reason) {
    close();
    const auto depth = static_cast<unsigned>(
        std::max<std::size_t>(1, std::min<std::size_t>(entries, 4096)));
    const int rc = io_uring_queue_init(depth, &ring_, 0);
    if (rc < 0) {
      reason = "io_uring_queue_init_failed_errno_" + std::to_string(-rc);
      return false;
    }
    opened_ = true;
    queued_submissions_ = 0;
    next_user_data_ = 1;
    return true;
  }

  void close() {
    if (opened_) {
      io_uring_queue_exit(&ring_);
      opened_ = false;
    }
    queued_submissions_ = 0;
  }

  bool queue_read(int fd, std::uint64_t offset, void* buffer,
                  std::size_t bytes, std::uint64_t user_data,
                  std::string& reason) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      reason = "io_uring_get_sqe_queue_full";
      return false;
    }
    io_uring_prep_read(sqe, fd, buffer, static_cast<unsigned>(bytes), offset);
    io_uring_sqe_set_data64(sqe, user_data);
    ++queued_submissions_;
    return true;
  }

  bool submit_queued(std::string& reason,
                     std::size_t* submit_syscalls = nullptr) {
    std::size_t syscall_count = 0;
    while (queued_submissions_ > 0) {
      const int submitted = io_uring_submit(&ring_);
      if (submitted < 0) {
        reason = "io_uring_submit_failed_errno_" +
                 std::to_string(-submitted);
        return false;
      }
      if (submitted == 0) {
        reason = "io_uring_submit_submitted_zero";
        return false;
      }
      const auto accepted = static_cast<unsigned>(submitted);
      queued_submissions_ =
          accepted >= queued_submissions_ ? 0 : queued_submissions_ - accepted;
      ++syscall_count;
    }
    if (submit_syscalls != nullptr) {
      *submit_syscalls += syscall_count;
    }
    return true;
  }

  bool pop_completion(Completion& completion, bool wait,
                      std::string& reason) {
    io_uring_cqe* cqe = nullptr;
    const int rc = wait ? io_uring_wait_cqe(&ring_, &cqe)
                        : io_uring_peek_cqe(&ring_, &cqe);
    if (rc == -EAGAIN) {
      return false;
    }
    if (rc < 0) {
      reason = (wait ? "io_uring_wait_cqe_failed_errno_"
                     : "io_uring_peek_cqe_failed_errno_") +
               std::to_string(-rc);
      return false;
    }
    if (cqe == nullptr) {
      return false;
    }
    completion.user_data = io_uring_cqe_get_data64(cqe);
    completion.result = cqe->res;
    io_uring_cqe_seen(&ring_, cqe);
    return true;
  }

  bool read(int fd, std::uint64_t offset, void* buffer, std::size_t bytes,
            DiskGraphSearchStats& stats, std::string& reason) {
    const std::uint64_t user_data = next_user_data_++;
    std::size_t submit_syscalls = 0;
    if (!queue_read(fd, offset, buffer, bytes, user_data, reason) ||
        !submit_queued(reason, &submit_syscalls)) {
      return false;
    }
    ++stats.io_submits;
    ++stats.submitted_reads;
    ++stats.p4_io.async_submits;
    stats.io_submit_syscalls += submit_syscalls;
    stats.uring_submit_count += submit_syscalls;

    Completion completion;
    do {
      if (!pop_completion(completion, true, reason)) {
        return false;
      }
    } while (completion.user_data != user_data);

    ++stats.io_completions;
    ++stats.completed_reads;
    ++stats.p4_io.async_completions;
    ++stats.uring_cqe_count;
    const int result = completion.result;
    if (result != static_cast<int>(bytes)) {
      reason = result < 0
                   ? "io_uring_read_failed_errno_" + std::to_string(-result)
                   : "io_uring_short_read_" + std::to_string(result);
      return false;
    }
    return true;
  }

 private:
  io_uring ring_{};
  bool opened_ = false;
  std::uint64_t next_user_data_ = 1;
  unsigned queued_submissions_ = 0;
};
#endif

}  // namespace

struct AsyncPageReader::Impl {
  struct AsyncBuffer {
    void* aligned = nullptr;
    std::size_t bytes = 0;
    std::size_t index = 0;
  };

  Impl(std::string reader_path, std::size_t reader_page_size)
      : path(std::move(reader_path)), page_size(reader_page_size) {
    configure("pread", 1, 1);
  }

  ~Impl() {
    close_native();
  }

  void configure(const std::string& mode, std::size_t batch_size,
                 std::size_t io_depth) {
    if (mode != "pread" && mode != "odirect" && mode != "io_uring") {
      throw std::runtime_error("Unsupported disk I/O mode: " + mode);
    }
    if (batch_size == 0) {
      throw std::runtime_error("Disk I/O batch size must be positive");
    }
    if (io_depth == 0) {
      throw std::runtime_error("Disk I/O depth must be positive");
    }

    close_native();
    inline_completions.clear();
    status = DiskGraphIoStatus{};
    status.requested_mode = mode;
    status.batch_size = batch_size;
    status.depth = io_depth;

#ifdef __linux__
    if (mode == "pread") {
      open_linux_fd(O_RDONLY | O_CLOEXEC, "pread_open_failed_errno");
      return;
    }

    if (mode == "odirect") {
      if (!direct_io_layout_supported()) {
        fallback_to_pread("odirect_requires_4096_aligned_pages");
        return;
      }
      if (!try_open_linux_fd(O_RDONLY | O_CLOEXEC | O_DIRECT)) {
        fallback_to_pread(errno_reason("odirect_open_failed_errno"));
        return;
      }
      status.effective_mode = "odirect";
      status.direct_enabled = true;
      return;
    }

#if AGENT_AWARE_ASYNC_CAN_USE_LIBURING
    bool direct_opened = false;
    if (direct_io_layout_supported()) {
      direct_opened = try_open_linux_fd(O_RDONLY | O_CLOEXEC | O_DIRECT);
    }
    if (!direct_opened) {
      fallback_to_pread(direct_io_layout_supported()
                            ? errno_reason("io_uring_odirect_open_failed_errno")
                            : "io_uring_requires_4096_aligned_pages");
      return;
    }

    std::string reason;
    if (!ring.open(io_depth, reason)) {
      fallback_to_pread(reason);
      return;
    }
    async_buffer_pool =
        std::make_unique<AlignedBufferPool>(io_depth, page_size,
                                            kDirectIoAlignment);
    status.effective_mode = "io_uring";
    status.direct_enabled = direct_opened;
    status.io_uring_enabled = true;
#else
    fallback_to_pread("liburing_unavailable");
#endif
#else
    if (mode == "pread") {
      open_buffered_stream();
      return;
    }
    fallback_to_pread(mode + "_requires_linux");
#endif
  }

  bool async_enabled() const {
#ifdef __linux__
    return status.effective_mode == "io_uring";
#else
    return false;
#endif
  }

  std::size_t max_pending_reads() const {
    return async_enabled() ? std::max<std::size_t>(1, status.depth) : 0;
  }

  std::size_t pending_async_reads() const {
    return async_buffers.size() + inline_completions.size();
  }

  std::uint64_t start_async_read(std::uint64_t offset, std::size_t bytes,
                                 DiskGraphSearchStats& stats) {
    if (!async_enabled()) {
      throw std::runtime_error("Async graph reads require io_uring mode");
    }
#ifdef __linux__
#if AGENT_AWARE_ASYNC_CAN_USE_LIBURING
    if (status.direct_enabled &&
        (offset % kDirectIoAlignment != 0 || bytes % kDirectIoAlignment != 0)) {
      throw std::runtime_error(
          "Direct graph I/O requires aligned page offsets and lengths");
    }

    if (async_buffer_pool == nullptr || async_buffer_pool->capacity() == 0) {
      throw std::runtime_error("Graph async I/O buffer pool is not initialized");
    }
    std::size_t buffer_index = 0;
    void* aligned = nullptr;
    if (!async_buffer_pool->acquire(buffer_index, aligned)) {
      throw std::runtime_error("Graph async I/O buffer pool is exhausted");
    }

    const std::uint64_t token = next_async_token++;
    std::string reason;
    if (!ring.queue_read(fd, offset, aligned, bytes, token, reason)) {
      async_buffer_pool->release(buffer_index);
      throw std::runtime_error("Graph page async read failed: " + reason);
    }

    async_buffers.emplace(token, AsyncBuffer{aligned, bytes, buffer_index});
    ++stats.io_submits;
    ++stats.submitted_reads;
    ++stats.p4_io.async_submits;
    ++stats.p4_io.ssd_reads;
    record_pending_peak(stats, pending_async_reads());
    return token;
#else
    (void)offset;
    (void)bytes;
    (void)stats;
    throw std::runtime_error("liburing is unavailable");
#endif
#else
    (void)offset;
    (void)bytes;
    (void)stats;
    throw std::runtime_error("Async graph reads require Linux");
#endif
  }

  std::vector<AsyncPageReader::SubmittedRead> batch_submit(
      const std::vector<AsyncPageReader::ReadRequest>& read_requests,
      DiskGraphSearchStats& stats) {
    std::vector<AsyncPageReader::SubmittedRead> submitted;
    submitted.reserve(read_requests.size());
    if (read_requests.empty()) {
      return submitted;
    }

    if (!async_enabled()) {
      for (const auto& request : read_requests) {
        const std::uint64_t token = next_async_token++;
        inline_completions.push_back(
            AsyncPageReader::CompletedRead{token,
                                           read(request.offset, request.bytes,
                                                stats)});
        submitted.push_back(AsyncPageReader::SubmittedRead{request.page_id,
                                                           token});
      }
      return submitted;
    }

    for (const auto& request : read_requests) {
      const std::uint64_t token =
          start_async_read(request.offset, request.bytes, stats);
      submitted.push_back(AsyncPageReader::SubmittedRead{request.page_id,
                                                         token});
    }
    submit_async_reads(stats);
    return submitted;
  }

  void submit_async_reads(DiskGraphSearchStats& stats) {
    if (!async_enabled()) {
      return;
    }
#ifdef __linux__
#if AGENT_AWARE_ASYNC_CAN_USE_LIBURING
    std::string reason;
    std::size_t submit_syscalls = 0;
    if (!ring.submit_queued(reason, &submit_syscalls)) {
      throw std::runtime_error("Graph page async submit failed: " + reason);
    }
    stats.io_submit_syscalls += submit_syscalls;
    stats.uring_submit_count += submit_syscalls;
#else
    (void)stats;
#endif
#else
    (void)stats;
#endif
  }

  bool reap_async_read(bool wait, AsyncPageReader::CompletedRead& completed,
                       DiskGraphSearchStats& stats) {
    if (!inline_completions.empty()) {
      completed = std::move(inline_completions.front());
      inline_completions.pop_front();
      return true;
    }
    if (!async_enabled()) {
      return false;
    }
#ifdef __linux__
#if AGENT_AWARE_ASYNC_CAN_USE_LIBURING
    LibUring::Completion completion;
    std::string reason;
    if (!ring.pop_completion(completion, wait, reason)) {
      if (!reason.empty()) {
        throw std::runtime_error("Graph page async wait failed: " + reason);
      }
      return false;
    }

    auto found = async_buffers.find(completion.user_data);
    if (found == async_buffers.end()) {
      throw std::runtime_error("Unknown graph async completion token");
    }
    AsyncBuffer buffer = found->second;
    async_buffers.erase(found);
    ++stats.io_completions;
    ++stats.completed_reads;
    ++stats.p4_io.async_completions;
    ++stats.uring_cqe_count;

    if (completion.result != static_cast<int>(buffer.bytes)) {
      const std::string failure =
          completion.result < 0
              ? "io_uring_read_failed_errno_" +
                    std::to_string(-completion.result)
              : "io_uring_short_read_" + std::to_string(completion.result);
      if (async_buffer_pool != nullptr) {
        async_buffer_pool->release(buffer.index);
      }
      throw std::runtime_error("Graph page async read failed: " + failure);
    }

    completed.token = completion.user_data;
    completed.data.assign(static_cast<const char*>(buffer.aligned),
                          static_cast<const char*>(buffer.aligned) +
                              buffer.bytes);
    if (async_buffer_pool != nullptr) {
      async_buffer_pool->release(buffer.index);
    }
    record_pending_peak(stats, pending_async_reads());
    return true;
#else
    (void)wait;
    (void)completed;
    (void)stats;
    return false;
#endif
#else
    (void)wait;
    (void)completed;
    (void)stats;
    return false;
#endif
  }

  std::vector<char> read(std::uint64_t offset, std::size_t bytes,
                         DiskGraphSearchStats& stats) {
    std::vector<char> output(bytes, 0);
#ifdef __linux__
    if (status.effective_mode == "odirect" ||
        status.effective_mode == "io_uring") {
      if (status.effective_mode == "io_uring" && !async_buffers.empty()) {
        throw std::runtime_error(
            "Cannot perform synchronous io_uring read with pending async reads");
      }
      if (status.direct_enabled &&
          (offset % kDirectIoAlignment != 0 ||
           bytes % kDirectIoAlignment != 0)) {
        throw std::runtime_error(
            "Direct graph I/O requires aligned page offsets and lengths");
      }

      void* aligned = nullptr;
      if (posix_memalign(&aligned, kDirectIoAlignment, bytes) != 0) {
        throw std::runtime_error("Failed to allocate aligned graph I/O buffer");
      }
      bool ok = false;
      std::string reason;
      if (status.effective_mode == "io_uring") {
#if AGENT_AWARE_ASYNC_CAN_USE_LIBURING
        ok = ring.read(fd, offset, aligned, bytes, stats, reason);
        ++stats.p4_io.sync_reads;
        ++stats.p4_io.ssd_reads;
#endif
      } else {
        ok = read_with_pread(aligned, bytes, offset, stats, reason);
      }
      if (ok) {
        std::memcpy(output.data(), aligned, bytes);
      }
      std::free(aligned);
      if (!ok) {
        throw std::runtime_error("Graph page read failed: " + reason);
      }
      return output;
    }

    std::string reason;
    if (!read_with_pread(output.data(), bytes, offset, stats, reason)) {
      throw std::runtime_error("Graph page read failed: " + reason);
    }
#else
    buffered_input.clear();
    buffered_input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!buffered_input) {
      throw std::runtime_error("Failed to seek graph index: " + path);
    }
    buffered_input.read(output.data(), static_cast<std::streamsize>(bytes));
    if (!buffered_input) {
      throw std::runtime_error("Failed to read graph page");
    }
    ++stats.io_submits;
    ++stats.submitted_reads;
    ++stats.io_completions;
    ++stats.completed_reads;
    ++stats.p4_io.sync_reads;
    ++stats.p4_io.ssd_reads;
#endif
    return output;
  }

#ifdef __linux__
  bool direct_io_layout_supported() const {
    return page_size % kDirectIoAlignment == 0;
  }

  bool try_open_linux_fd(int flags) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    fd = ::open(path.c_str(), flags);
    return fd >= 0;
  }

  void open_linux_fd(int flags, const std::string& error_prefix) {
    if (!try_open_linux_fd(flags)) {
      throw std::runtime_error(errno_reason(error_prefix) + ": " + path);
    }
  }

  void fallback_to_pread(const std::string& reason) {
    close_native();
    open_linux_fd(O_RDONLY | O_CLOEXEC, "pread_fallback_open_failed_errno");
    status.effective_mode = "pread";
    status.direct_enabled = false;
    status.io_uring_enabled = false;
    status.fallback_reason = reason;
  }

  bool read_with_pread(void* buffer, std::size_t bytes, std::uint64_t offset,
                       DiskGraphSearchStats& stats, std::string& reason) {
    const ssize_t result =
        ::pread(fd, buffer, bytes, static_cast<off_t>(offset));
    ++stats.io_submits;
    ++stats.submitted_reads;
    ++stats.io_submit_syscalls;
    ++stats.p4_io.sync_reads;
    ++stats.p4_io.ssd_reads;
    if (status.requested_mode != status.effective_mode) {
      ++stats.p4_io.fallback_reads;
    }
    if (result < 0) {
      reason = errno_reason("pread_failed_errno");
      return false;
    }
    ++stats.io_completions;
    ++stats.completed_reads;
    if (result != static_cast<ssize_t>(bytes)) {
      reason = "pread_short_read_" + std::to_string(result);
      return false;
    }
    return true;
  }
#else
  void open_buffered_stream() {
    buffered_input.open(path, std::ios::binary);
    if (!buffered_input) {
      throw std::runtime_error("Cannot open graph index: " + path);
    }
  }

  void fallback_to_pread(const std::string& reason) {
    open_buffered_stream();
    status.effective_mode = "pread";
    status.fallback_reason = reason;
  }
#endif

  void close_native() {
#ifdef __linux__
#if AGENT_AWARE_ASYNC_CAN_USE_LIBURING
    while (!async_buffers.empty()) {
      LibUring::Completion completion;
      std::string reason;
      if (!ring.pop_completion(completion, true, reason)) {
        break;
      }
      const auto found = async_buffers.find(completion.user_data);
      if (found == async_buffers.end()) {
        continue;
      }
      const auto buffer_index = found->second.index;
      async_buffers.erase(found);
      if (async_buffer_pool != nullptr) {
        async_buffer_pool->release(buffer_index);
      }
    }
    ring.close();
#endif
    async_buffers.clear();
    inline_completions.clear();
    async_buffer_pool.reset();
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
#else
    async_buffers.clear();
    inline_completions.clear();
    async_buffer_pool.reset();
    if (buffered_input.is_open()) {
      buffered_input.close();
    }
#endif
  }

  std::string path;
  std::size_t page_size = 0;
  DiskGraphIoStatus status;
  std::unordered_map<std::uint64_t, AsyncBuffer> async_buffers;
  std::deque<AsyncPageReader::CompletedRead> inline_completions;
  std::unique_ptr<AlignedBufferPool> async_buffer_pool;
  std::uint64_t next_async_token = 1;
#ifdef __linux__
  int fd = -1;
#if AGENT_AWARE_ASYNC_CAN_USE_LIBURING
  LibUring ring;
#endif
#else
  std::ifstream buffered_input;
#endif
};

AsyncPageReader::AsyncPageReader(std::string path, std::size_t page_size)
    : impl_(std::make_unique<Impl>(std::move(path), page_size)) {}

AsyncPageReader::~AsyncPageReader() = default;

void AsyncPageReader::configure(const std::string& mode,
                                std::size_t batch_size,
                                std::size_t io_depth) {
  impl_->configure(mode, batch_size, io_depth);
}

const DiskGraphIoStatus& AsyncPageReader::status() const {
  return impl_->status;
}

bool AsyncPageReader::async_enabled() const {
  return impl_->async_enabled();
}

std::size_t AsyncPageReader::max_pending_reads() const {
  return impl_->max_pending_reads();
}

std::size_t AsyncPageReader::pending_async_reads() const {
  return impl_->pending_async_reads();
}

std::vector<char> AsyncPageReader::read(std::uint64_t offset,
                                        std::size_t bytes,
                                        DiskGraphSearchStats& stats) {
  return impl_->read(offset, bytes, stats);
}

std::uint64_t AsyncPageReader::start_async_read(
    std::uint64_t offset, std::size_t bytes, DiskGraphSearchStats& stats) {
  return impl_->start_async_read(offset, bytes, stats);
}

std::vector<AsyncPageReader::SubmittedRead> AsyncPageReader::batch_submit(
    const std::vector<ReadRequest>& read_requests,
    DiskGraphSearchStats& stats) {
  return impl_->batch_submit(read_requests, stats);
}

void AsyncPageReader::submit_async_reads(DiskGraphSearchStats& stats) {
  impl_->submit_async_reads(stats);
}

bool AsyncPageReader::reap_async_read(bool wait, CompletedRead& completed,
                                      DiskGraphSearchStats& stats) {
  return impl_->reap_async_read(wait, completed, stats);
}

}  // namespace agent_aware
