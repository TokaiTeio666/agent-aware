#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "agent_aware/storage/disk_record.h"

namespace agent_aware {

class DiskIndexWriter {
 public:
  DiskIndexWriter(const std::string& path, std::uint32_t dim,
                  std::uint32_t max_degree, bool use_direct_io);
  ~DiskIndexWriter();

  DiskIndexWriter(const DiskIndexWriter&) = delete;
  DiskIndexWriter& operator=(const DiskIndexWriter&) = delete;

  void write_header(std::uint64_t num_nodes, std::uint64_t entry_node);

  void write_node(std::uint32_t node_id, const float* vector,
                  std::uint32_t dim,
                  const std::vector<std::uint32_t>& neighbors,
                  const std::vector<std::uint8_t>& neighbor_pq_codes = {},
                  std::uint16_t neighbor_pq_code_bytes = 0);

  void close();

  const IndexFileHeader& header() const {
    return header_;
  }

 private:
  void write_page_at(const void* buffer, std::uint64_t offset);

  std::string path_;
  std::uint32_t dim_ = 0;
  std::uint32_t max_degree_ = 0;
  bool use_direct_io_ = false;
  bool header_written_ = false;
  bool closed_ = false;
  int fd_ = -1;
  std::ofstream output_;
  IndexFileHeader header_;
};

}  // namespace agent_aware
