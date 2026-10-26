// 验证 Phase 0 的两个测量前提：
//   1. /proc/diskstats 采集的读写量能在真实的、绕过页缓存的 I/O 上产生可观测增量；
//   2. mincore 的页缓存驻留比例能区分"文件在缓存里"和"文件不在缓存里"。
//
// 这两条不成立，基准里的任何 I/O 结论都没有意义。测试在非 Linux 上自动跳过。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "core/platform_io.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (condition) {
    std::printf("ok   %s\n", what);
  } else {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

// 测试必须在真实块设备上跑：/tmp 常是 tmpfs（纯内存，没有物理 I/O），
// 在它上面测 "/proc/diskstats 有增量" 会永远失败且原因看不出来。
std::string pick_backed_device_dir() {
  for (const char* candidate : {"/data", "/var/tmp", "."}) {
    const auto snap = agent_aware::physical_io_snapshot_for_path(candidate);
    if (snap.valid) {
      return candidate;
    }
  }
  return std::string();
}

// 生成一个不落在页缓存里的临时文件：写入后 fsync，再用 DONTNEED 逐出。
std::string make_temp_file(const std::string& dir, const std::string& tag,
                           std::size_t bytes, bool keep_in_cache) {
  std::string path = dir + "/agent_aware_io_" + tag + "_" +
                     std::to_string(::getpid());
  const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0) {
    return std::string();
  }
  std::vector<char> block(1 << 20, static_cast<char>(0x5a));
  std::size_t written = 0;
  while (written < bytes) {
    const std::size_t chunk = std::min(block.size(), bytes - written);
    const ssize_t rc = ::write(fd, block.data(), chunk);
    if (rc <= 0) {
      break;
    }
    written += static_cast<std::size_t>(rc);
  }
  ::fsync(fd);
  if (!keep_in_cache) {
    agent_aware::drop_page_cache_for_path(path.c_str());
  }
  ::close(fd);
  return path;
}

}  // namespace

int main() {
  using agent_aware::PhysicalIoSnapshot;

  const std::string dir = pick_backed_device_dir();
  if (dir.empty()) {
    std::printf(
        "skip: no block-device-backed directory found (need Linux with "
        "/proc/diskstats)\n");
    return 0;
  }
  const PhysicalIoSnapshot initial =
      agent_aware::physical_io_snapshot_for_path(dir.c_str());
  std::printf("dir: %s  device: %s\n", dir.c_str(), initial.device.c_str());
  check(initial.valid, "physical_io_snapshot_for_path returns a valid snapshot");

  // --- 物理 I/O 增量：一次 O_DIRECT 读必须让设备读计数增长 ---
  constexpr std::size_t kFileBytes = 32u << 20;  // 32 MiB，足以超过任何预读窗口
  const std::string path = make_temp_file(dir, "disk", kFileBytes, false);
  if (path.empty()) {
    std::printf("skip: cannot create temp file\n");
    return failures == 0 ? 0 : 1;
  }

  const PhysicalIoSnapshot before =
      agent_aware::physical_io_snapshot_for_path(path.c_str());
  // 用 O_DIRECT 逐页读，保证请求一定落到设备而不是页缓存。
  const int out_fd = ::open("/dev/null", O_WRONLY);
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
  std::size_t direct_bytes = 0;
  if (fd >= 0) {
    void* buffer = nullptr;
    constexpr std::size_t kAlign = 4096;
    if (::posix_memalign(&buffer, kAlign, kAlign) == 0) {
      std::size_t offset = 0;
      while (offset < kFileBytes) {
        const ssize_t rc = ::pread(fd, buffer, kAlign, static_cast<off_t>(offset));
        if (rc <= 0) {
          break;
        }
        direct_bytes += static_cast<std::size_t>(rc);
        offset += kAlign;
      }
      std::free(buffer);
    }
    ::close(fd);
  }
  if (out_fd >= 0) {
    ::close(out_fd);
  }
  // diskstats 是异步累加的，给内核一点时间把完成量记进去。
  ::usleep(200 * 1000);
  const PhysicalIoSnapshot after =
      agent_aware::physical_io_snapshot_for_path(path.c_str());

  check(fd >= 0, "O_DIRECT open succeeds on the data device");
  if (fd >= 0) {
    const PhysicalIoSnapshot delta =
        agent_aware::physical_io_delta(before, after);
    check(delta.valid, "physical_io_delta is valid for the same device");
    const std::uint64_t delta_bytes = delta.read_sectors * 512ULL;
    std::printf("read %zu bytes via O_DIRECT, device counted %llu bytes\n",
                direct_bytes, static_cast<unsigned long long>(delta_bytes));
    // 允许设备预读带来的额外量，但要求"至少读到了我们请求的量的一半"：
    // 只要物理计数在真实 I/O 上确实增长，这条计量的意义就成立。
    check(direct_bytes > 0,
          "O_DIRECT read transferred data");
    check(delta_bytes >= direct_bytes / 2,
          "device read counter grows with real (cache-bypassing) reads");
  }

  // --- 页缓存驻留：冷文件应接近 0，读热后应接近 1 ---
  const std::string cached_path = make_temp_file(dir, "cache", 8u << 20, true);
  if (!cached_path.empty()) {
    agent_aware::drop_page_cache_for_path(cached_path.c_str());
    const double cold =
        agent_aware::page_cache_residency_ratio(cached_path.c_str());
    std::printf("cold residency: %.3f\n", cold);
    check(cold >= 0.0, "page_cache_residency_ratio is measurable");
    check(cold < 0.5, "DONTNEED leaves most pages out of the page cache");

    // 读一遍让它进缓存
    const int rfd = ::open(cached_path.c_str(), O_RDONLY);
    if (rfd >= 0) {
      std::vector<char> sink(1 << 20);
      while (::read(rfd, sink.data(), sink.size()) > 0) {
      }
      ::close(rfd);
    }
    const double hot =
        agent_aware::page_cache_residency_ratio(cached_path.c_str());
    std::printf("hot residency:  %.3f\n", hot);
    check(hot > 0.9, "reading the file brings nearly all pages into the cache");
    ::unlink(cached_path.c_str());
  }

  ::unlink(path.c_str());

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
