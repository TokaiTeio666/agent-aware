#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent_aware/storage/disk_index_reader.h"
#include "agent_aware/storage/disk_index_writer.h"

#ifndef AGENT_AWARE_ENABLE_DIRECT_IO
#define AGENT_AWARE_ENABLE_DIRECT_IO 1
#endif

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct Fixture {
  std::vector<std::vector<float>> vectors;
  std::vector<std::vector<std::uint32_t>> neighbors;
  std::vector<std::vector<std::uint8_t>> neighbor_pq_codes;
};

Fixture make_fixture(std::size_t nodes, std::uint32_t dim,
                     std::uint32_t max_degree) {
  Fixture fixture;
  fixture.vectors.resize(nodes);
  fixture.neighbors.resize(nodes);
  fixture.neighbor_pq_codes.resize(nodes);

  for (std::size_t node = 0; node < nodes; ++node) {
    auto& vector = fixture.vectors[node];
    vector.resize(dim);
    for (std::uint32_t d = 0; d < dim; ++d) {
      vector[d] = static_cast<float>((node * 131 + d * 17) % 1009) / 100.0f;
    }

    const std::uint32_t degree = 1 + static_cast<std::uint32_t>(node % max_degree);
    auto& neighbors = fixture.neighbors[node];
    neighbors.reserve(degree);
    for (std::uint32_t j = 0; j < degree; ++j) {
      neighbors.push_back(static_cast<std::uint32_t>((node + j + 1) % nodes));
    }
    auto& pq_codes = fixture.neighbor_pq_codes[node];
    pq_codes.resize(static_cast<std::size_t>(degree) * 2);
    for (std::size_t j = 0; j < pq_codes.size(); ++j) {
      pq_codes[j] = static_cast<std::uint8_t>((node + j) % 251);
    }
  }
  return fixture;
}

void compare_record(const agent_aware::NodeRecord& record, std::uint32_t node_id,
                    const Fixture& fixture) {
  require(record.node_id == node_id, "read node id matches");
  require(record.vector == fixture.vectors[node_id], "read vector matches");
  require(record.neighbors == fixture.neighbors[node_id],
          "read neighbors match");
  require(record.neighbor_pq_code_bytes == 2, "read pq code width matches");
  require(record.neighbor_pq_codes == fixture.neighbor_pq_codes[node_id],
          "read pq codes match");
}

std::filesystem::path test_path(const std::string& name) {
  return std::filesystem::current_path() / name;
}

void test_index_rw(bool direct_io, const std::string& name) {
  constexpr std::uint32_t kDim = 128;
  constexpr std::uint32_t kMaxDegree = 64;
  constexpr std::uint32_t kNodes = 1000;
  constexpr std::uint32_t kReads = 10000;

  const auto path = test_path(name);
  std::filesystem::remove(path);
  const Fixture fixture = make_fixture(kNodes, kDim, kMaxDegree);

  {
    agent_aware::DiskIndexWriter writer(path.string(), kDim, kMaxDegree,
                                     direct_io);
    writer.write_header(kNodes, 0);
    for (std::uint32_t node = 0; node < kNodes; ++node) {
      writer.write_node(node, fixture.vectors[node].data(), kDim,
                        fixture.neighbors[node],
                        fixture.neighbor_pq_codes[node], 2);
    }
    writer.close();
  }

  {
    agent_aware::DiskIndexReader reader(path.string(), direct_io);
    require(reader.header().dim == kDim, "header dim");
    require(reader.header().num_nodes == kNodes, "header node count");
    require(reader.header().max_degree == kMaxDegree, "header max degree");
    require(reader.header().record_size == agent_aware::kDiskIndexPageSize,
            "header record size");
    require(reader.header().header_size == agent_aware::kDiskIndexPageSize,
            "header header size");

    std::mt19937 rng(123);
    std::uniform_int_distribution<std::uint32_t> pick(0, kNodes - 1);
    for (std::uint32_t i = 0; i < kReads; ++i) {
      const std::uint32_t node_id = pick(rng);
      compare_record(reader.read_node(node_id), node_id, fixture);
    }

    bool threw = false;
    try {
      (void)reader.read_node(kNodes);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    require(threw, "out-of-range node id is rejected");
    reader.close();
  }

  std::filesystem::remove(path);
}

void test_writer_rejects_degree_overflow() {
  constexpr std::uint32_t kDim = 8;
  constexpr std::uint32_t kMaxDegree = 4;
  const auto path = test_path("disk_index_degree_overflow.ssdindex");
  std::filesystem::remove(path);

  std::vector<float> vector(kDim, 1.0f);
  std::vector<std::uint32_t> neighbors = {1, 2, 3, 4, 5};
  agent_aware::DiskIndexWriter writer(path.string(), kDim, kMaxDegree, false);
  writer.write_header(1, 0);

  bool threw = false;
  try {
    writer.write_node(0, vector.data(), kDim, neighbors);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "writer rejects degree greater than max_degree");
  writer.close();
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  test_index_rw(false, "disk_index_rw_stream.ssdindex");
#if defined(__linux__) && AGENT_AWARE_ENABLE_DIRECT_IO
  test_index_rw(true, "disk_index_rw_direct.ssdindex");
#endif
  test_writer_rejects_degree_overflow();
  return 0;
}
