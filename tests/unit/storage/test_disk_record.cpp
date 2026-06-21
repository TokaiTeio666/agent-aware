#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "agent_aware/storage/disk_record.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::uint32_t fnv1a_bytes(const void* data, std::size_t bytes,
                          std::uint32_t seed = 2166136261u) {
  const auto* cursor = static_cast<const std::uint8_t*>(data);
  std::uint32_t hash = seed;
  for (std::size_t i = 0; i < bytes; ++i) {
    hash ^= cursor[i];
    hash *= 16777619u;
  }
  return hash;
}

std::uint32_t old_payload_checksum(const float* vector, std::uint32_t dim,
                                   const std::uint32_t* neighbors,
                                   std::uint32_t degree) {
  std::uint32_t hash = fnv1a_bytes(vector, dim * sizeof(float));
  return fnv1a_bytes(neighbors, degree * sizeof(std::uint32_t), hash);
}

struct OldDiskNodeRecordHeader {
  std::uint32_t magic = agent_aware::kDiskNodeRecordMagic;
  std::uint32_t node_id = 0;
  std::uint16_t degree = 0;
  std::uint16_t dim = 0;
  std::uint32_t flags = 0;
  std::uint32_t checksum = 0;
  std::uint32_t vector_offset = 0;
  std::uint32_t neighbors_offset = 0;
  std::uint32_t payload_bytes = 0;
};

static_assert(sizeof(OldDiskNodeRecordHeader) == 32);

void test_record_round_trip() {
  const std::vector<float> vector = {1.25f, -2.5f, 3.75f, 4.0f};
  const std::vector<std::uint32_t> neighbors = {7, 8, 11};
  auto page = agent_aware::allocate_aligned_buffer();

  agent_aware::encode_node_record(42, vector.data(), vector.size(), neighbors,
                               page.get(), agent_aware::kDiskIndexPageSize);
  const auto decoded =
      agent_aware::decode_node_record(page.get(), agent_aware::kDiskIndexPageSize);

  require(decoded.node_id == 42, "record node id round trip");
  require(decoded.dim == vector.size(), "record dim round trip");
  require(decoded.degree == neighbors.size(), "record degree round trip");
  require(decoded.vector == vector, "record vector round trip");
  require(decoded.neighbors == neighbors, "record neighbors round trip");
  require(decoded.neighbor_pq_code_bytes == 0, "empty pq code byte width");
  require(decoded.neighbor_pq_codes.empty(), "empty pq codes round trip");

  agent_aware::DiskNodeRecordHeader header;
  std::memcpy(&header, page.get(), sizeof(header));
  require(header.payload_bytes ==
              agent_aware::disk_record_bytes(vector.size(), neighbors.size()),
          "record payload byte count");
  for (std::size_t i = header.payload_bytes; i < agent_aware::kDiskIndexPageSize;
       ++i) {
    require(page.get()[i] == 0, "record padding is zeroed");
  }
}

void test_record_round_trip_with_neighbor_pq_codes() {
  const std::vector<float> vector = {0.5f, 1.5f};
  const std::vector<std::uint32_t> neighbors = {3, 4};
  const std::vector<std::uint8_t> pq_codes = {1, 2, 3, 4};
  auto page = agent_aware::allocate_aligned_buffer();

  agent_aware::encode_node_record(9, vector.data(), vector.size(), neighbors,
                               page.get(), agent_aware::kDiskIndexPageSize,
                               pq_codes, 2);
  const auto decoded =
      agent_aware::decode_node_record(page.get(), agent_aware::kDiskIndexPageSize);

  require(decoded.node_id == 9, "pq record node id round trip");
  require(decoded.neighbor_pq_code_bytes == 2, "pq code byte width");
  require(decoded.neighbors == neighbors, "pq record neighbors");
  require(decoded.neighbor_pq_codes == pq_codes, "pq codes round trip");
}

void test_legacy_record_header_is_decoded() {
  const std::vector<float> vector = {2.0f, -1.0f};
  const std::vector<std::uint32_t> neighbors = {5, 6};
  auto page = agent_aware::allocate_aligned_buffer();

  OldDiskNodeRecordHeader header;
  header.node_id = 77;
  header.degree = static_cast<std::uint16_t>(neighbors.size());
  header.dim = static_cast<std::uint16_t>(vector.size());
  header.vector_offset = sizeof(OldDiskNodeRecordHeader);
  header.neighbors_offset =
      header.vector_offset + static_cast<std::uint32_t>(
                                 vector.size() * sizeof(float));
  header.payload_bytes =
      header.neighbors_offset +
      static_cast<std::uint32_t>(neighbors.size() * sizeof(std::uint32_t));
  header.checksum = old_payload_checksum(vector.data(), header.dim,
                                         neighbors.data(), header.degree);

  std::memcpy(page.get(), &header, sizeof(header));
  std::memcpy(page.get() + header.vector_offset, vector.data(),
              vector.size() * sizeof(float));
  std::memcpy(page.get() + header.neighbors_offset, neighbors.data(),
              neighbors.size() * sizeof(std::uint32_t));

  const auto decoded =
      agent_aware::decode_node_record(page.get(), agent_aware::kDiskIndexPageSize);
  require(decoded.node_id == 77, "legacy record node id");
  require(decoded.vector == vector, "legacy record vector");
  require(decoded.neighbors == neighbors, "legacy record neighbors");
  require(decoded.neighbor_pq_code_bytes == 0, "legacy record has no pq codes");
  require(decoded.neighbor_pq_codes.empty(), "legacy record pq payload empty");
}

void test_record_overflow_rejected() {
  auto page = agent_aware::allocate_aligned_buffer();
  const std::vector<float> vector(2000, 1.0f);
  const std::vector<std::uint32_t> neighbors = {1};

  bool threw = false;
  try {
    agent_aware::encode_node_record(1, vector.data(), vector.size(), neighbors,
                                 page.get(), agent_aware::kDiskIndexPageSize);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "oversized record is rejected");
}

void test_checksum_detects_corruption() {
  const std::vector<float> vector = {1.0f, 2.0f};
  const std::vector<std::uint32_t> neighbors = {3, 4};
  auto page = agent_aware::allocate_aligned_buffer();

  agent_aware::encode_node_record(2, vector.data(), vector.size(), neighbors,
                               page.get(), agent_aware::kDiskIndexPageSize);
  page.get()[sizeof(agent_aware::DiskNodeRecordHeader)] ^= 0x1u;

  bool threw = false;
  try {
    (void)agent_aware::decode_node_record(page.get(),
                                       agent_aware::kDiskIndexPageSize);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "record checksum catches payload corruption");
}

}  // namespace

int main() {
  test_record_round_trip();
  test_record_round_trip_with_neighbor_pq_codes();
  test_legacy_record_header_is_decoded();
  test_record_overflow_rejected();
  test_checksum_detects_corruption();
  return 0;
}
