#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace agent_aware {

enum class MemoryDomain : std::uint8_t {
  kDeltaControlled = 0,
  kBaseGraphCache,
  kMergeWorkspace,
  kBaseMetadata,
  kRuntimeReserve,
  kCount,
};

struct MemoryBudgetSnapshot {
  std::array<std::uint64_t, static_cast<std::size_t>(MemoryDomain::kCount)>
      bytes{};
  std::uint64_t delta_soft_events = 0;
  std::uint64_t delta_high_events = 0;
  std::uint64_t delta_hard_events = 0;
};

class MemoryBudget {
 public:
  MemoryBudget(std::uint64_t delta_soft_bytes = 5ULL * 1024ULL * 1024ULL,
               std::uint64_t delta_high_bytes = 6ULL * 1024ULL * 1024ULL,
               std::uint64_t delta_hard_bytes = 7ULL * 1024ULL * 1024ULL);

  bool try_reserve(MemoryDomain domain, std::uint64_t bytes);
  void account_existing(MemoryDomain domain, std::uint64_t bytes);
  void release(MemoryDomain domain, std::uint64_t bytes);
  std::uint64_t used(MemoryDomain domain) const;
  MemoryBudgetSnapshot snapshot() const;

  std::uint64_t delta_soft_bytes() const { return delta_soft_bytes_; }
  std::uint64_t delta_high_bytes() const { return delta_high_bytes_; }
  std::uint64_t delta_hard_bytes() const { return delta_hard_bytes_; }

 private:
  static constexpr std::size_t index(MemoryDomain domain) {
    return static_cast<std::size_t>(domain);
  }

  std::array<std::atomic<std::uint64_t>,
             static_cast<std::size_t>(MemoryDomain::kCount)>
      used_{};
  std::uint64_t delta_soft_bytes_;
  std::uint64_t delta_high_bytes_;
  std::uint64_t delta_hard_bytes_;
  std::atomic<std::uint64_t> delta_soft_events_{0};
  std::atomic<std::uint64_t> delta_high_events_{0};
  std::atomic<std::uint64_t> delta_hard_events_{0};
};

}  // namespace agent_aware
