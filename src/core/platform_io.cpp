#include "core/platform_io.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
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
#endif

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

}  // namespace agent_aware
