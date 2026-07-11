#include "platform/page_io_backend.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(__linux__) || defined(__OHOS__)
#include <unistd.h>
#endif

namespace agent_aware::platform {
namespace {

struct Task {
  PageReadRequest request;
  std::mutex mutex;
  std::condition_variable condition;
  bool started = false;
  bool completed = false;
  bool cancelled = false;
  std::string error;
  std::vector<char> data;
};

class ThreadPoolPreadBackend final : public PageIoBackend {
 public:
  ~ThreadPoolPreadBackend() override { close(); }

  bool open(const std::string& path, std::size_t page_size,
            std::size_t queue_depth, std::string& error) override {
    close();
    if (page_size == 0 || queue_depth == 0) {
      error = "invalid_thread_pool_configuration";
      return false;
    }
#if defined(__linux__) || defined(__OHOS__)
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
      error = "pread_open_failed_errno_" + std::to_string(errno);
      return false;
    }
#else
    error = "thread_pool_pread_requires_posix";
    return false;
#endif
    page_size_ = page_size;
    capacity_ = queue_depth;
    stopping_ = false;
    for (std::size_t i = 0; i < queue_depth; ++i)
      workers_.emplace_back([this] { worker_loop(); });
    return true;
  }

  PageReadResult read(std::uint64_t offset, std::size_t bytes) override {
    PageReadResult result;
    result.data.resize(bytes);
#if defined(__linux__) || defined(__OHOS__)
    std::size_t done = 0;
    while (done < bytes) {
      const ssize_t count = ::pread(fd_, result.data.data() + done,
                                    bytes - done,
                                    static_cast<off_t>(offset + done));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) {
        result.error = count == 0 ? "pread_short_read"
                                  : "pread_failed_errno_" + std::to_string(errno);
        result.data.clear();
        return result;
      }
      done += static_cast<std::size_t>(count);
    }
    result.ok = true;
#else
    result.error = "thread_pool_pread_requires_posix";
#endif
    return result;
  }

  std::vector<IoToken> submit(const std::vector<PageReadRequest>& requests,
                              std::string& error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) { error = "backend_closed"; return {}; }
    std::vector<IoToken> tokens;
    tokens.reserve(requests.size());
    for (const auto& request : requests) {
      if (request.bytes == 0) { error = "invalid_read_size"; return {}; }
      while (queue_.size() >= capacity_) {
        auto prefetch = std::find_if(queue_.rbegin(), queue_.rend(),
            [](const auto& task) {
              return task->request.priority == IoPriority::kPrefetch &&
                     !task->started;
            });
        if (prefetch == queue_.rend()) { error = "read_queue_full"; return {}; }
        (*prefetch)->cancelled = true;
        (*prefetch)->completed = true;
        (*prefetch)->condition.notify_all();
        queue_.erase(std::next(prefetch).base());
      }
      const std::string key = std::to_string(request.offset) + ":" +
                              std::to_string(request.bytes);
      std::shared_ptr<Task> task;
      auto existing = inflight_by_key_.find(key);
      if (existing != inflight_by_key_.end()) task = existing->second.lock();
      if (!task) {
        task = std::make_shared<Task>();
        task->request = request;
        inflight_by_key_[key] = task;
        auto position = std::find_if(queue_.begin(), queue_.end(),
            [&](const auto& queued) {
              return static_cast<int>(queued->request.priority) >
                     static_cast<int>(request.priority);
            });
        queue_.insert(position, task);
      }
      const IoToken token = next_token_++;
      tokens_[token] = task;
      tokens.push_back(token);
    }
    condition_.notify_all();
    return tokens;
  }

  PageReadResult wait(IoToken token, std::uint32_t timeout_ms) override {
    std::shared_ptr<Task> task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = tokens_.find(token);
      if (found == tokens_.end()) return {token, 0, false, "unknown_io_token", {}};
      task = found->second;
    }
    std::unique_lock<std::mutex> task_lock(task->mutex);
    const bool ready = timeout_ms == UINT32_MAX
        ? (task->condition.wait(task_lock, [&] { return task->completed; }), true)
        : task->condition.wait_for(task_lock, std::chrono::milliseconds(timeout_ms),
                                   [&] { return task->completed; });
    if (!ready) return {token, task->request.page_id, false, "io_wait_timeout", {}};
    PageReadResult result{token, task->request.page_id,
                          task->error.empty() && !task->cancelled,
                          task->cancelled ? "io_cancelled" : task->error,
                          task->data};
    task_lock.unlock();
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_.erase(token);
    return result;
  }

  bool cancel(IoToken token) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = tokens_.find(token);
    if (found == tokens_.end()) return false;
    auto& task = found->second;
    std::lock_guard<std::mutex> task_lock(task->mutex);
    if (task->started) return false;
    task->cancelled = true;
    task->completed = true;
    task->condition.notify_all();
    return true;
  }

  PageIoStatus status() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return {"thread_pool_pread", "thread_pool_pread", "", capacity_,
            queue_.size(), tokens_.size()};
  }

  void close() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ && workers_.empty()) return;
      stopping_ = true;
      for (auto& task : queue_) {
        std::lock_guard<std::mutex> task_lock(task->mutex);
        if (!task->started) {
          task->cancelled = true;
          task->completed = true;
          task->condition.notify_all();
        }
      }
      condition_.notify_all();
    }
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
    queue_.clear(); tokens_.clear(); inflight_by_key_.clear();
#if defined(__linux__) || defined(__OHOS__)
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
  }

 private:
  void worker_loop() {
    while (true) {
      std::shared_ptr<Task> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) return;
        task = queue_.front(); queue_.pop_front();
      }
      {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (task->cancelled) continue;
        task->started = true;
      }
      auto result = read(task->request.offset, task->request.bytes);
      {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->data = std::move(result.data);
        task->error = std::move(result.error);
        task->completed = true;
      }
      task->condition.notify_all();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::shared_ptr<Task>> queue_;
  std::unordered_map<IoToken, std::shared_ptr<Task>> tokens_;
  std::unordered_map<std::string, std::weak_ptr<Task>> inflight_by_key_;
  std::vector<std::thread> workers_;
  IoToken next_token_ = 1;
  std::size_t page_size_ = 0;
  std::size_t capacity_ = 0;
  bool stopping_ = true;
  int fd_ = -1;
};

}  // namespace

std::unique_ptr<PageIoBackend> make_thread_pool_pread_backend() {
  return std::make_unique<ThreadPoolPreadBackend>();
}

}  // namespace agent_aware::platform
