#include "core/memory_budget.h"

#include <algorithm>

namespace agent_aware {

MemoryBudget::MemoryBudget(std::uint64_t delta_soft_bytes,
                           std::uint64_t delta_high_bytes,
                           std::uint64_t delta_hard_bytes)
    : delta_soft_bytes_(delta_soft_bytes),
      delta_high_bytes_(std::max(delta_soft_bytes, delta_high_bytes)),
      delta_hard_bytes_(std::max(delta_high_bytes_, delta_hard_bytes)) {}

bool MemoryBudget::try_reserve(MemoryDomain domain, std::uint64_t bytes) {
  auto& counter = used_[index(domain)];
  const auto next = counter.fetch_add(bytes, std::memory_order_acq_rel) + bytes;
  if (domain != MemoryDomain::kDeltaControlled) return true;
  if (next >= delta_hard_bytes_) {
    delta_hard_events_.fetch_add(1, std::memory_order_relaxed);
    counter.fetch_sub(bytes, std::memory_order_acq_rel);
    return false;
  }
  if (next >= delta_high_bytes_) {
    delta_high_events_.fetch_add(1, std::memory_order_relaxed);
  } else if (next >= delta_soft_bytes_) {
    delta_soft_events_.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

void MemoryBudget::account_existing(MemoryDomain domain, std::uint64_t bytes) {
  used_[index(domain)].fetch_add(bytes, std::memory_order_acq_rel);
}

void MemoryBudget::release(MemoryDomain domain, std::uint64_t bytes) {
  auto& counter = used_[index(domain)];
  auto current = counter.load(std::memory_order_acquire);
  while (!counter.compare_exchange_weak(
      current, current > bytes ? current - bytes : 0,
      std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
}

std::uint64_t MemoryBudget::used(MemoryDomain domain) const {
  return used_[index(domain)].load(std::memory_order_acquire);
}

MemoryBudgetSnapshot MemoryBudget::snapshot() const {
  MemoryBudgetSnapshot output;
  for (std::size_t i = 0; i < output.bytes.size(); ++i) {
    output.bytes[i] = used_[i].load(std::memory_order_acquire);
  }
  output.delta_soft_events = delta_soft_events_.load();
  output.delta_high_events = delta_high_events_.load();
  output.delta_hard_events = delta_hard_events_.load();
  return output;
}

}  // namespace agent_aware
