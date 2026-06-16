#pragma once

#include <cstdint>
#include <fstream>
#include <string>

#include "agentmem/storage/disk_record.h"

namespace agentmem {

class DiskIndexReader {
 public:
  explicit DiskIndexReader(const std::string& path, bool use_direct_io);
  ~DiskIndexReader();

  DiskIndexReader(const DiskIndexReader&) = delete;
  DiskIndexReader& operator=(const DiskIndexReader&) = delete;

  const IndexFileHeader& header() const {
    return header_;
  }

  NodeRecord read_node(std::uint32_t node_id);

  void read_node_into(std::uint32_t node_id, void* aligned_buffer);

  void close();

 private:
  void read_page_at(void* buffer, std::uint64_t offset);

  std::string path_;
  bool use_direct_io_ = false;
  bool closed_ = false;
  int fd_ = -1;
  std::ifstream input_;
  IndexFileHeader header_;
};

}  // namespace agentmem
