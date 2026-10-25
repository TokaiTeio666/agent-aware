#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace agent_aware {

// 进程内存与文件落盘两类平台能力的薄封装。基准程序依赖它们给出
// 真实常驻内存（而不是估算）和 POSIX/Windows 一致的 fsync 语义。
// 所有函数都可失败：调用方必须读取返回值，而不是假设成功。

// 当前进程峰值物理内存（Linux: /proc/self/status VmHWM；
// Windows: GetProcessMemoryInfo PeWSS）。失败返回 0。
std::uint64_t process_peak_resident_bytes();

// 当前进程常驻内存（Linux: VmRSS；Windows: WorkingSetSize）。失败返回 0。
std::uint64_t process_current_resident_bytes();

// 把本进程已写入的 FILE 数据刷到磁盘。失败返回 false。
bool fsync_file(std::FILE* file);

// 把本进程已写入的裸描述符数据刷到磁盘（Linux: fdatasync）。
// 非 Linux 平台或失败返回 false。
bool fdatasync_fd(int fd);

// 对目录项（rename/unlink 之后）落盘，确保崩溃后目录元数据可见。
// 非 POSIX 平台返回 true（NTFS 通过 RenameFile 的日志语义保证）。
bool fsync_directory(const char* path);

}  // namespace agent_aware
