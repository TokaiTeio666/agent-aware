#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "harmony/operation_scheduler.h"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() {
  using agent_aware::harmony::OperationScheduler;
  OperationScheduler scheduler(2, 16);
  auto first = scheduler.acquire(OperationScheduler::Kind::kSearch);
  auto second = scheduler.acquire(OperationScheduler::Kind::kSearch);
  require(first && second, "search permits failed");
  std::atomic<bool> write_started{false};
  std::thread writer([&] {
    auto permit = scheduler.acquire(OperationScheduler::Kind::kWrite);
    write_started.store(static_cast<bool>(permit));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  require(!write_started.load(), "write overlapped active searches");
  first = {};
  second = {};
  writer.join();
  require(write_started.load(), "waiting write did not run");
  scheduler.close();
  require(!scheduler.acquire(OperationScheduler::Kind::kSearch),
          "closed scheduler accepted search");
  return 0;
}
