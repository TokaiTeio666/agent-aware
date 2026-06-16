#include "agentmem/storage/disk_index_writer.h"

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

#if AGENTMEM_CAN_USE_POSIX_IO
void write_posix_page(int fd, const void* buffer, std::size_t bytes,
                      std::uint64_t offset, bool direct,
                      const std::string& path) {
  if (offset >
          static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) -
                  offset) {
    throw std::runtime_error("Disk index offset exceeds off_t range");
  }

  std::size_t total = 0;
  const auto* cursor = static_cast<const char*>(buffer);
  while (total < bytes) {
    const ssize_t written =
        ::pwrite(fd, cursor + total, bytes - total,
                 static_cast<off_t>(offset + total));
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0) {
      throw std::runtime_error(errno_message("Failed to write disk index", path));
    }
    if (written == 0) {
      throw std::runtime_error("Short write while writing disk index: " + path);
    }
    if (direct && static_cast<std::size_t>(written) != bytes) {
      throw std::runtime_error("Short O_DIRECT write while writing disk index: " +
                               path);
    }
    total += static_cast<std::size_t>(written);
  }
}
#endif

}  // namespace

DiskIndexWriter::DiskIndexWriter(const std::string& path, std::uint32_t dim,
                                 std::uint32_t max_degree,
                                 bool use_direct_io)
    : path_(path),
      dim_(dim),
      max_degree_(max_degree),
      use_direct_io_(use_direct_io) {
  validate_disk_record_shape(dim_, max_degree_);

#if AGENTMEM_CAN_USE_POSIX_IO
  int flags = O_CREAT | O_TRUNC | O_WRONLY;
  if (use_direct_io_) {
#if AGENTMEM_CAN_USE_DIRECT_IO
    flags |= O_DIRECT;
#else
    throw std::runtime_error("O_DIRECT is unavailable in this build");
#endif
  }
  fd_ = ::open(path_.c_str(), flags, 0644);
  if (fd_ < 0) {
    throw std::runtime_error(errno_message("Cannot create disk index", path_));
  }
#else
  if (use_direct_io_) {
    throw std::runtime_error("O_DIRECT is unavailable in this build");
  }
    output_.open(path_, std::ios::binary | std::ios::trunc);
    if (!output_) {
      throw std::runtime_error("Cannot create disk index: " + path_);
    }
#endif
}

DiskIndexWriter::~DiskIndexWriter() {
  try {
    close();
  } catch (...) {
  }
}

void DiskIndexWriter::write_header(std::uint64_t num_nodes,
                                   std::uint64_t entry_node) {
  if (closed_) {
    throw std::runtime_error("Cannot write to closed disk index writer");
  }
  header_ = make_index_file_header(dim_, max_degree_, num_nodes, entry_node);
  auto page = allocate_aligned_buffer();
  std::memcpy(page.get(), &header_, sizeof(header_));
  write_page_at(page.get(), 0);
  header_written_ = true;
}

void DiskIndexWriter::write_node(
    std::uint32_t node_id, const float* vector, std::uint32_t dim,
    const std::vector<std::uint32_t>& neighbors) {
  if (closed_) {
    throw std::runtime_error("Cannot write to closed disk index writer");
  }
  if (!header_written_) {
    throw std::runtime_error("Disk index header must be written before records");
  }
  if (dim != dim_) {
    throw std::runtime_error("Disk node dimension does not match index header");
  }
  if (neighbors.size() > max_degree_) {
    throw std::runtime_error("Disk node degree exceeds index max_degree");
  }

  auto page = allocate_aligned_buffer();
  encode_node_record(node_id, vector, dim, neighbors, page.get(),
                     kDiskIndexPageSize);
  write_page_at(page.get(), disk_record_offset(node_id, header_));
}

void DiskIndexWriter::write_page_at(const void* buffer, std::uint64_t offset) {
#if AGENTMEM_CAN_USE_POSIX_IO
  if (fd_ >= 0) {
    write_posix_page(fd_, buffer, kDiskIndexPageSize, offset, use_direct_io_,
                     path_);
    return;
  }
#endif

  output_.clear();
  output_.seekp(checked_stream_offset(offset), std::ios::beg);
  if (!output_) {
    throw std::runtime_error("Failed to seek disk index for write: " + path_);
  }
  output_.write(static_cast<const char*>(buffer), kDiskIndexPageSize);
  if (!output_) {
    throw std::runtime_error("Failed to write disk index: " + path_);
  }
}

void DiskIndexWriter::close() {
  if (closed_) {
    return;
  }

#if AGENTMEM_CAN_USE_POSIX_IO
  if (fd_ >= 0) {
    if (::fsync(fd_) != 0) {
      const auto message = errno_message("Failed to fsync disk index", path_);
      ::close(fd_);
      fd_ = -1;
      closed_ = true;
      throw std::runtime_error(message);
    }
    if (::close(fd_) != 0) {
      fd_ = -1;
      closed_ = true;
      throw std::runtime_error(errno_message("Failed to close disk index", path_));
    }
    fd_ = -1;
  } else
#endif
  if (output_.is_open()) {
    output_.flush();
    if (!output_) {
      closed_ = true;
      throw std::runtime_error("Failed to flush disk index: " + path_);
    }
    output_.close();
    if (!output_) {
      closed_ = true;
      throw std::runtime_error("Failed to close disk index: " + path_);
    }
  }

  closed_ = true;
}

}  // namespace agentmem
