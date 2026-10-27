#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace agent_aware {

// 进程内存、文件落盘、物理 I/O 三类平台能力的薄封装。基准程序依赖它们给出
// 真实常驻内存（而不是估算）、POSIX/Windows 一致的 fsync 语义，以及
// "逻辑读调用"之外的物理 I/O 量。
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

// ---------------------------------------------------------------------------
// 物理 I/O 计量
//
// 统计 "pread/io_uring 提交了几次" 只反映调用次数。当数据能装进系统页缓存时，
// 这些调用一次磁盘都没有碰；此时任何 I/O 结论都不成立。要判断一次实验究竟
// 落在哪一侧，必须同时采集块设备层面的真实读写量。
// ---------------------------------------------------------------------------

struct PhysicalIoSnapshot {
  bool valid = false;
  // 形如 "8:16"，来自 stat() 的 st_dev，与 /proc/diskstats 的 major:minor 对应。
  std::string device;
  std::uint64_t read_sectors = 0;
  std::uint64_t write_sectors = 0;
  std::uint64_t read_ops = 0;
  std::uint64_t write_ops = 0;
};

// 读取 path 所在块设备的累计读写计数（Linux: /proc/diskstats）。
// 匹配的是 stat() 给出的精确 major:minor（整盘或分区，取决于挂载方式），
// 因此不会把整盘和其分区重复计数。非 Linux 或失败时 valid=false。
PhysicalIoSnapshot physical_io_snapshot_for_path(const char* path);

// 两次快照之间的增量。任一快照无效时返回 valid=false 的增量。
PhysicalIoSnapshot physical_io_delta(const PhysicalIoSnapshot& before,
                                     const PhysicalIoSnapshot& after);

// ---------------------------------------------------------------------------
// 页缓存状态
//
// "冷缓存实验" 如果没有可验证的证据，就只是口头声明。用 mmap + mincore 直接
// 查询文件页是否驻留在页缓存中，得到可复核的驻留比例。
// ---------------------------------------------------------------------------

// path 的文件页当前驻留在页缓存中的比例 [0,1]。
// 返回 -1.0 表示不可用（非 Linux、文件不存在、mmap/mincore 失败）。
double page_cache_residency_ratio(const char* path);

// 尝试把 path 的页缓存页逐出（Linux: POSIX_FADV_DONTNEED）。
// 返回调用是否成功；实际效果需再用 page_cache_residency_ratio 验证。
bool drop_page_cache_for_path(const char* path);

}  // namespace agent_aware

