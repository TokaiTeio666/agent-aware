#include "agentmem/storage/disk_index_reader.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifndef AGENTMEM_ENABLE_DIRECT_IO
#define AGENTMEM_ENABLE_DIRECT_IO 1
#endif

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#define AGENTMEM_CAN_USE_POSIX_IO 1
#else
#define AGENTMEM_CAN_USE_POSIX_IO 0
#endif

#if defined(__linux__) && AGENTMEM_ENABLE_DIRECT_IO
#define AGENTMEM_CAN_USE_DIRECT_IO 1
#else
#define AGENTMEM_CAN_USE_DIRECT_IO 0
#endif

namespace agentmem {
namespace {

std::string errno_message(const std::string& prefix, const std::string& path) {
  return prefix + ": " + path + ": " + std::strerror(errno);
}

std::streamoff checked_stream_offset(std::uint64_t offset) {
  if (offset >
      static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    throw std::runtime_error("Disk index offset exceeds streamoff range");
  }
  return static_cast<std::streamoff>(offset);
}

void require_direct_aligned(const void* buffer) {
  if (reinterpret_cast<std::uintptr_t>(buffer) % kDiskIndexPageSize != 0) {
    throw std::runtime_error("O_DIRECT buffer is not 4096-byte aligned");
  }
}

#if AGENTMEM_CAN_USE_POSIX_IO
void read_posix_page(int fd, void* buffer, std::size_t bytes,
                     std::uint64_t offset, bool direct,
                     const std::string& path) {
  if (offset >
          static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) -
                  offset) {
    throw std::runtime_error("Disk index offset exceeds off_t range");
  }

  std::size_t total = 0;
  auto* cursor = static_cast<char*>(buffer);
  while (total < bytes) {
    const ssize_t read_bytes =
        ::pread(fd, cursor + total, bytes - total,
                static_cast<off_t>(offset + total));
    if (read_bytes < 0 && errno == EINTR) {
      continue;
    }
    if (read_bytes < 0) {
      throw std::runtime_error(errno_message("Failed to read disk index", path));
    }
    if (read_bytes == 0) {
      throw std::runtime_error("Short read while reading disk index: " + path);
    }
    if (direct && static_cast<std::size_t>(read_bytes) != bytes) {
      throw std::runtime_error("Short O_DIRECT read while reading disk index: " +
                               path);
    }
    total += static_cast<std::size_t>(read_bytes);
  }
}
#endif

}  // namespace

DiskIndexReader::DiskIndexReader(const std::string& path, bool use_direct_io)
    : path_(path), use_direct_io_(use_direct_io) {
#if AGENTMEM_CAN_USE_POSIX_IO
  int flags = O_RDONLY;
  if (use_direct_io_) {
#if AGENTMEM_CAN_USE_DIRECT_IO
    flags |= O_DIRECT;
#else
    throw std::runtime_error("O_DIRECT is unavailable in this build");
#endif
  }
  fd_ = ::open(path_.c_str(), flags);
  if (fd_ < 0) {
    throw std::runtime_error(errno_message("Cannot open disk index", path_));
  }
#else
  if (use_direct_io_) {
    throw std::runtime_error("O_DIRECT is unavailable in this build");
  }
    input_.open(path_, std::ios::binary);
    if (!input_) {
      throw std::runtime_error("Cannot open disk index: " + path_);
    }
#endif

  auto page = allocate_aligned_buffer();
  read_page_at(page.get(), 0);
  std::memcpy(&header_, page.get(), sizeof(header_));
  validate_index_file_header(header_);
}

DiskIndexReader::~DiskIndexReader() {
  try {
    close();
  } catch (...) {
  }
}

NodeRecord DiskIndexReader::read_node(std::uint32_t node_id) {
  auto page = allocate_aligned_buffer();
  read_node_into(node_id, page.get());
  NodeRecord record = decode_node_record(page.get(), kDiskIndexPageSize);
  if (record.node_id != node_id) {
    throw std::runtime_error("Disk node id does not match requested offset");
  }
  if (record.dim != header_.dim) {
    throw std::runtime_error("Disk node dimension does not match header");
  }
  if (record.degree > header_.max_degree) {
    throw std::runtime_error("Disk node degree exceeds header max_degree");
  }
  return record;
}

void DiskIndexReader::read_node_into(std::uint32_t node_id,
                                     void* aligned_buffer) {
  if (closed_) {
    throw std::runtime_error("Cannot read from closed disk index reader");
  }
  if (aligned_buffer == nullptr) {
    throw std::runtime_error("Disk index read buffer is null");
  }
  if (use_direct_io_) {
    require_direct_aligned(aligned_buffer);
  }
  read_page_at(aligned_buffer, disk_record_offset(node_id, header_));
}

void DiskIndexReader::read_page_at(void* buffer, std::uint64_t offset) {
#if AGENTMEM_CAN_USE_POSIX_IO
  if (fd_ >= 0) {
    read_posix_page(fd_, buffer, kDiskIndexPageSize, offset, use_direct_io_,
                    path_);
    return;
  }
#endif

  input_.clear();
  input_.seekg(checked_stream_offset(offset), std::ios::beg);
  if (!input_) {
    throw std::runtime_error("Failed to seek disk index for read: " + path_);
  }
  input_.read(static_cast<char*>(buffer), kDiskIndexPageSize);
  if (input_.gcount() != static_cast<std::streamsize>(kDiskIndexPageSize)) {
    throw std::runtime_error("Short read while reading disk index: " + path_);
  }
}

void DiskIndexReader::close() {
  if (closed_) {
    return;
  }

#if AGENTMEM_CAN_USE_POSIX_IO
  if (fd_ >= 0) {
    if (::close(fd_) != 0) {
      fd_ = -1;
      closed_ = true;
      throw std::runtime_error(errno_message("Failed to close disk index", path_));
    }
    fd_ = -1;
  } else
#endif
  if (input_.is_open()) {
    input_.close();
    if (!input_) {
      closed_ = true;
      throw std::runtime_error("Failed to close disk index: " + path_);
    }
  }

  closed_ = true;
}

}  // namespace agentmem
