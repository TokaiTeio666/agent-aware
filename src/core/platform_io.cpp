#include "core/platform_io.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent_aware {
namespace {

#ifdef _WIN32
bool windows_memory_counters(PROCESS_MEMORY_COUNTERS& counters) {
  return ::GetProcessMemoryInfo(::GetCurrentProcess(), &counters,
                                sizeof(counters)) != 0;
}
#else
// 只读取一个小文件的指定字段，避免为了一个整数打开 /proc 整个目录。
std::uint64_t proc_status_kb(const char* key) {
  std::ifstream status("/proc/self/status");
  if (!status) {
    return 0;
  }
  std::string line;
  const std::string prefix(key);
  while (std::getline(status, line)) {
    if (line.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    return std::strtoull(line.c_str() + colon + 1, nullptr, 10) * 1024ULL;
  }
  return 0;
}

// /proc/diskstats 的字段顺序（内核 Documentation/admin-guide/iostats.rst）：
//   major minor name reads_done reads_merged sectors_read ms_reading
//   writes_done writes_merged sectors_written ...
// 只取 name 之后的第 0/2/4/6 项。sectors 恒为 512 字节单位。
bool read_diskstats(const std::string& target, PhysicalIoSnapshot& out) {
  std::ifstream stats("/proc/diskstats");
  if (!stats) {
    return false;
  }
  std::string line;
  while (std::getline(stats, line)) {
    std::istringstream fields(line);
    std::string major;
    std::string minor;
    std::string name;
    if (!(fields >> major >> minor >> name)) {
      continue;
    }
    const std::string key = major + ":" + minor;
    if (key != target) {
      continue;
    }
    std::uint64_t reads_done = 0;
    std::uint64_t reads_merged = 0;
    std::uint64_t sectors_read = 0;
    std::uint64_t ms_reading = 0;
    std::uint64_t writes_done = 0;
    std::uint64_t writes_merged = 0;
    std::uint64_t sectors_written = 0;
    if (!(fields >> reads_done >> reads_merged >> sectors_read >> ms_reading >>
          writes_done >> writes_merged >> sectors_written)) {
      return false;
    }
    out.valid = true;
    out.device = key + " (" + name + ")";
    out.read_ops = reads_done;
    out.read_sectors = sectors_read;
    out.write_ops = writes_done;
    out.write_sectors = sectors_written;
    return true;
  }
  return false;
}
#endif  // _WIN32

}  // namespace

std::uint64_t process_peak_resident_bytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  return windows_memory_counters(counters)
             ? static_cast<std::uint64_t>(counters.PeakWorkingSetSize)
             : 0;
#else
  return proc_status_kb("VmHWM:");
#endif
}

std::uint64_t process_current_resident_bytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  return windows_memory_counters(counters)
             ? static_cast<std::uint64_t>(counters.WorkingSetSize)
             : 0;
#else
  return proc_status_kb("VmRSS:");
#endif
}

bool fsync_file(std::FILE* file) {
  if (file == nullptr) {
    return false;
  }
#ifdef _WIN32
  // 调用方常常只用 "rb" 打开文件（写流已关闭）；此时底层句柄没有写权限，
  // _commit() 和 FlushFileBuffers() 都会以 EBADF/ERROR_ACCESS_DENIED 失败。
  // 先尝试 _commit，失败后按同一路径以 "r+b" 重新打开再做 _commit：MSVC 的
  // _commit 内部会先 fflush 再 FlushFileBuffers，语义等同于 POSIX fsync。
  if (std::fflush(file) != 0) {
    return false;
  }
  const int fd = ::_fileno(file);
  if (fd >= 0 && ::_commit(fd) == 0) {
    return true;
  }

  char path[4096] = {0};
  const intptr_t os_handle =
      fd >= 0 ? ::_get_osfhandle(fd) : static_cast<intptr_t>(-1);
  if (os_handle != -1 &&
      ::GetFinalPathNameByHandleA(reinterpret_cast<HANDLE>(os_handle), path,
                                  static_cast<DWORD>(sizeof(path) - 1),
                                  FILE_NAME_NORMALIZED) < sizeof(path)) {
    std::FILE* writer = std::fopen(path, "r+b");
    if (writer == nullptr) {
      return false;
    }
    const bool synced = ::_commit(::_fileno(writer)) == 0;
    std::fclose(writer);
    return synced;
  }
  return false;
#else
  int rc = 0;
  do {
    rc = ::fsync(::fileno(file));
  } while (rc != 0 && errno == EINTR);
  return rc == 0;
#endif
}

bool fdatasync_fd(int fd) {
#ifdef _WIN32
  (void)fd;
  return false;
#else
  int rc = 0;
  do {
    rc = ::fdatasync(fd);
  } while (rc != 0 && errno == EINTR);
  return rc == 0;
#endif
}

bool fsync_directory(const char* path) {
#ifdef _WIN32
  (void)path;
  return true;
#else
  const int fd = ::open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return false;
  }
  int rc = 0;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  ::close(fd);
  return rc == 0;
#endif
}

PhysicalIoSnapshot physical_io_snapshot_for_path(const char* path) {
  PhysicalIoSnapshot snapshot;
  if (path == nullptr) {
    return snapshot;
  }
#ifdef _WIN32
  // Windows 没有与 /proc/diskstats 等价的按设备读计数接口；
  // 返回 invalid 让调用方明确知道"物理 I/O 未知"，而不是报 0 蒙混过去。
  (void)path;
  return snapshot;
#else
  struct stat info {};
  if (::stat(path, &info) != 0) {
    return snapshot;
  }
  // st_dev 的低 8 位是 minor，高位是 major（glibc gnu_dev_major/minor 的展开）。
  const unsigned int dev = static_cast<unsigned int>(info.st_dev);
  const std::string key = std::to_string((dev >> 8) & 0xfff) + ":" +
                          std::to_string((dev & 0xff) | ((dev >> 12) & 0xfff00));
  read_diskstats(key, snapshot);
  return snapshot;
#endif
}

PhysicalIoSnapshot physical_io_delta(const PhysicalIoSnapshot& before,
                                     const PhysicalIoSnapshot& after) {
  PhysicalIoSnapshot delta;
  if (!before.valid || !after.valid || before.device != after.device) {
    return delta;
  }
  delta.valid = true;
  delta.device = after.device;
  // 计数器是单调递增的；若设备被重新枚举导致回绕，做饱和处理而不是无符号下溢。
  const auto delta_of = [](std::uint64_t a, std::uint64_t b) {
    return a >= b ? a - b : 0;
  };
  delta.read_sectors = delta_of(after.read_sectors, before.read_sectors);
  delta.write_sectors = delta_of(after.write_sectors, before.write_sectors);
  delta.read_ops = delta_of(after.read_ops, before.read_ops);
  delta.write_ops = delta_of(after.write_ops, before.write_ops);
  return delta;
}

double page_cache_residency_ratio(const char* path) {
#ifdef _WIN32
  (void)path;
  return -1.0;
#else
  if (path == nullptr) {
    return -1.0;
  }
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1.0;
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || info.st_size == 0) {
    ::close(fd);
    return -1.0;
  }
  const std::size_t length = static_cast<std::size_t>(info.st_size);
  void* mapping = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapping == MAP_FAILED) {
    return -1.0;
  }
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    ::munmap(mapping, length);
    return -1.0;
  }
  const std::size_t pages =
      (length + static_cast<std::size_t>(page_size) - 1) /
      static_cast<std::size_t>(page_size);
  std::vector<unsigned char> resident(pages, 0);
  const int rc = ::mincore(mapping, length, resident.data());
  ::munmap(mapping, length);
  if (rc != 0) {
    return -1.0;
  }
  std::size_t resident_pages = 0;
  for (const unsigned char flag : resident) {
    if ((flag & 0x1) != 0) {
      ++resident_pages;
    }
  }
  return static_cast<double>(resident_pages) / static_cast<double>(pages);
#endif
}

bool drop_page_cache_for_path(const char* path) {
#ifdef _WIN32
  (void)path;
  return false;
#else
  if (path == nullptr) {
    return false;
  }
  // DONTNEED 只丢弃"当前没有脏页"的部分；文件只读时总是成立的。
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const int rc = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  ::close(fd);
  return rc == 0;
#endif
}

}  // namespace agent_aware
