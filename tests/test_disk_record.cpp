#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "agentmem/storage/disk_record.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_record_round_trip() {
  const std::vector<float> vector = {1.25f, -2.5f, 3.75f, 4.0f};
  const std::vector<std::uint32_t> neighbors = {7, 8, 11};
  auto page = agentmem::allocate_aligned_buffer();

  agentmem::encode_node_record(42, vector.data(), vector.size(), neighbors,
                               page.get(), agentmem::kDiskIndexPageSize);
  const auto decoded =
      agentmem::decode_node_record(page.get(), agentmem::kDiskIndexPageSize);

  require(decoded.node_id == 42, "record node id round trip");
  require(decoded.dim == vector.size(), "record dim round trip");
  require(decoded.degree == neighbors.size(), "record degree round trip");
  require(decoded.vector == vector, "record vector round trip");
  require(decoded.neighbors == neighbors, "record neighbors round trip");

  agentmem::DiskNodeRecordHeader header;
  std::memcpy(&header, page.get(), sizeof(header));
  require(header.payload_bytes ==
              agentmem::disk_record_bytes(vector.size(), neighbors.size()),
          "record payload byte count");
  for (std::size_t i = header.payload_bytes; i < agentmem::kDiskIndexPageSize;
       ++i) {
    require(page.get()[i] == 0, "record padding is zeroed");
  }
}

void test_record_overflow_rejected() {
  auto page = agentmem::allocate_aligned_buffer();
  const std::vector<float> vector(2000, 1.0f);
  const std::vector<std::uint32_t> neighbors = {1};

  bool threw = false;
  try {
    agentmem::encode_node_record(1, vector.data(), vector.size(), neighbors,
                                 page.get(), agentmem::kDiskIndexPageSize);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "oversized record is rejected");
}

void test_checksum_detects_corruption() {
  const std::vector<float> vector = {1.0f, 2.0f};
  const std::vector<std::uint32_t> neighbors = {3, 4};
  auto page = agentmem::allocate_aligned_buffer();

  agentmem::encode_node_record(2, vector.data(), vector.size(), neighbors,
                               page.get(), agentmem::kDiskIndexPageSize);
  page.get()[sizeof(agentmem::DiskNodeRecordHeader)] ^= 0x1u;

  bool threw = false;
  try {
    (void)agentmem::decode_node_record(page.get(),
                                       agentmem::kDiskIndexPageSize);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "record checksum catches payload corruption");
}

}  // namespace

int main() {
  test_record_round_trip();
  test_record_overflow_rejected();
  test_checksum_detects_corruption();
  return 0;
}
