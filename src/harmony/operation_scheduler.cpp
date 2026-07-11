#include "harmony/operation_scheduler.h"

#include <utility>

namespace agent_aware::harmony {

OperationScheduler::OperationScheduler(std::size_t max_searches,
                                       std::size_t query_burst)
    : max_searches_(max_searches == 0 ? 1 : max_searches),
      query_burst_(query_burst == 0 ? 1 : query_burst) {}

OperationScheduler::Permit& OperationScheduler::Permit::operator=(
    Permit&& other) noexcept {
  if (this == &other) return *this;
  if (owner_ != nullptr) owner_->release(kind_);
  owner_ = other.owner_;
  kind_ = other.kind_;
  other.owner_ = nullptr;
  return *this;
}

OperationScheduler::Permit::~Permit() {
  if (owner_ != nullptr) owner_->release(kind_);
}

OperationScheduler::Permit OperationScheduler::acquire(Kind kind) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (kind == Kind::kWrite) ++waiting_writes_;
  condition_.wait(lock, [&] {
    if (closing_ && kind != Kind::kLifecycle) return true;
    if (kind == Kind::kLifecycle) {
      return !lifecycle_active_ && !write_active_ && active_searches_ == 0;
    }
    if (lifecycle_active_ || write_active_) return false;
    if (kind == Kind::kWrite) return active_searches_ == 0;
    return active_searches_ < max_searches_ &&
           !(waiting_writes_ > 0 && searches_since_write_ >= query_burst_);
  });
  if (closing_ && kind != Kind::kLifecycle) {
    if (kind == Kind::kWrite) --waiting_writes_;
    return {};
  }
  if (kind == Kind::kLifecycle) {
    lifecycle_active_ = true;
  } else if (kind == Kind::kWrite) {
    --waiting_writes_;
    write_active_ = true;
    searches_since_write_ = 0;
  } else {
    ++active_searches_;
    ++searches_since_write_;
  }
  return Permit(this, kind);
}

void OperationScheduler::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  closing_ = true;
  condition_.notify_all();
}

void OperationScheduler::release(Kind kind) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (kind == Kind::kLifecycle) lifecycle_active_ = false;
  else if (kind == Kind::kWrite) write_active_ = false;
  else if (active_searches_ > 0) --active_searches_;
  condition_.notify_all();
}

}  // namespace agent_aware::harmony
