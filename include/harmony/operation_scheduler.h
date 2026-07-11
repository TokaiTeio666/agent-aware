#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace agent_aware::harmony {

class OperationScheduler {
 public:
  enum class Kind { kSearch, kWrite, kLifecycle };

  class Permit {
   public:
    Permit() = default;
    Permit(OperationScheduler* owner, Kind kind) : owner_(owner), kind_(kind) {}
    Permit(Permit&& other) noexcept : owner_(other.owner_), kind_(other.kind_) {
      other.owner_ = nullptr;
    }
    Permit& operator=(Permit&& other) noexcept;
    ~Permit();
    explicit operator bool() const { return owner_ != nullptr; }

   private:
    OperationScheduler* owner_ = nullptr;
    Kind kind_ = Kind::kSearch;
  };

  explicit OperationScheduler(std::size_t max_searches = 4,
                              std::size_t query_burst = 16);
  Permit acquire(Kind kind);
  void close();

 private:
  friend class Permit;
  void release(Kind kind);

  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t max_searches_ = 4;
  std::size_t query_burst_ = 16;
  std::size_t active_searches_ = 0;
  std::size_t waiting_writes_ = 0;
  std::size_t searches_since_write_ = 0;
  bool write_active_ = false;
  bool lifecycle_active_ = false;
  bool closing_ = false;
};

}  // namespace agent_aware::harmony
