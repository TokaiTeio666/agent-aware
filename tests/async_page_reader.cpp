#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

#include "core/async_page_reader.h"
#include "graph/disk_graph_index.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  using namespace agent_aware;
  constexpr std::size_t kPageSize = 4096;
  constexpr std::size_t kPageCount = 3;

  const fs::path root =
      fs::temp_directory_path() / "agent-aware-async-page-reader-test";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path data_path = root / "pages.bin";
  {
    std::ofstream output(data_path, std::ios::binary | std::ios::trunc);
    require(output.good(), "failed to create async reader test file");
    std::array<char, kPageSize> page{};
    for (std::size_t page_id = 0; page_id < kPageCount; ++page_id) {
      page.fill(static_cast<char>(page_id + 1));
      output.write(page.data(), page.size());
    }
  }

  AsyncPageReader reader(data_path.string(), kPageSize);
  reader.configure("io_uring", 4, 8);
  DiskGraphSearchStats stats;
  const auto first = reader.queue_reads({{1, kPageSize, kPageSize}}, stats);
  const auto second =
      reader.queue_reads({{2, 2 * kPageSize, kPageSize}}, stats);
  require(first.size() == 1 && second.size() == 1,
          "deferred queue did not accept both demand batches");
  require(first.front().token < second.front().token,
          "queued reads did not preserve token order");

  const std::size_t syscalls_before_flush = stats.io_submit_syscalls;
  require(!reader.async_enabled() || syscalls_before_flush == 0,
          "queue_reads flushed io_uring before explicit submit");
  reader.submit_async_reads(stats);
  if (reader.async_enabled()) {
    require(stats.io_submit_syscalls == syscalls_before_flush + 1,
            "queued demand reads did not share one explicit flush");
  }
  const std::size_t syscalls_after_flush = stats.io_submit_syscalls;
  reader.submit_async_reads(stats);
  require(stats.io_submit_syscalls == syscalls_after_flush,
          "empty explicit flush issued another I/O syscall");

  std::unordered_map<std::uint64_t, char> expected{
      {first.front().token, 2}, {second.front().token, 3}};
  while (!expected.empty()) {
    AsyncPageReader::CompletedRead completed;
    require(reader.reap_async_read(true, completed, stats),
            "failed to reap a queued demand read");
    const auto found = expected.find(completed.token);
    require(found != expected.end(), "reader returned an unknown token");
    require(completed.data.size() == kPageSize,
            "reader returned a partial page");
    require(completed.data.front() == found->second,
            "reader returned data from the wrong page");
    expected.erase(found);
  }

  fs::remove_all(root);
  return 0;
}
