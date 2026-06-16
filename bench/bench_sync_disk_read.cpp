#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "agentmem/storage/disk_index_reader.h"
#include "agentmem/storage/disk_index_writer.h"

namespace {

struct BenchConfig {
  std::string path = "build/bench_sync_disk_read.ssdindex";
  std::uint64_t nodes = 10000;
  std::uint32_t dim = 128;
  std::uint32_t degree = 64;
  std::uint32_t reads = 10000;
  bool direct_io = false;
  bool rebuild = false;
};

bool parse_bool(const std::string& value) {
  if (value == "1" || value == "true" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "off") {
    return false;
  }
  throw std::runtime_error("Expected boolean value, got: " + value);
}

BenchConfig parse_args(int argc, char** argv) {
  BenchConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto need_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + name);
      }
      return argv[++i];
    };

    if (flag == "--path") {
      config.path = need_value("--path");
    } else if (flag == "--nodes") {
      config.nodes = std::stoull(need_value("--nodes"));
    } else if (flag == "--dim") {
      config.dim = static_cast<std::uint32_t>(std::stoul(need_value("--dim")));
    } else if (flag == "--degree") {
      config.degree =
          static_cast<std::uint32_t>(std::stoul(need_value("--degree")));
    } else if (flag == "--reads") {
      config.reads =
          static_cast<std::uint32_t>(std::stoul(need_value("--reads")));
    } else if (flag == "--direct") {
      config.direct_io = parse_bool(need_value("--direct"));
    } else if (flag == "--rebuild") {
      config.rebuild = parse_bool(need_value("--rebuild"));
    } else {
      throw std::runtime_error("Unknown benchmark flag: " + flag);
    }
  }
  return config;
}

void fill_vector(std::uint64_t node_id, std::vector<float>& vector) {
  for (std::size_t d = 0; d < vector.size(); ++d) {
    vector[d] =
        static_cast<float>((node_id * 131 + d * 17) % 1009) / 100.0f;
  }
}

void fill_neighbors(std::uint64_t node_id, std::uint64_t nodes,
                    std::vector<std::uint32_t>& neighbors) {
  for (std::size_t i = 0; i < neighbors.size(); ++i) {
    neighbors[i] = static_cast<std::uint32_t>((node_id + i + 1) % nodes);
  }
}

void build_index(const BenchConfig& config) {
  const std::filesystem::path path(config.path);
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }

  agentmem::DiskIndexWriter writer(config.path, config.dim, config.degree,
                                   config.direct_io);
  writer.write_header(config.nodes, 0);

  std::vector<float> vector(config.dim, 0.0f);
  const std::uint32_t effective_degree =
      config.nodes <= 1
          ? 0
          : std::min<std::uint32_t>(config.degree,
                                    static_cast<std::uint32_t>(config.nodes - 1));
  std::vector<std::uint32_t> neighbors(effective_degree, 0);
  for (std::uint64_t node = 0; node < config.nodes; ++node) {
    fill_vector(node, vector);
    fill_neighbors(node, config.nodes, neighbors);
    writer.write_node(static_cast<std::uint32_t>(node), vector.data(),
                      config.dim, neighbors);
  }
  writer.close();
}

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) {
    return 0.0;
  }
  const std::size_t index =
      std::min<std::size_t>(sorted.size() - 1,
                            static_cast<std::size_t>(p * (sorted.size() - 1)));
  return sorted[index];
}

void run_benchmark(const BenchConfig& config) {
  const bool needs_build =
      config.rebuild || !std::filesystem::exists(config.path);
  if (needs_build) {
    build_index(config);
  }

  agentmem::DiskIndexReader reader(config.path, config.direct_io);
  if (reader.header().num_nodes == 0) {
    throw std::runtime_error("Cannot benchmark an empty disk index");
  }

  auto page = agentmem::allocate_aligned_buffer();
  std::mt19937 rng(42);
  std::uniform_int_distribution<std::uint32_t> pick(
      0, static_cast<std::uint32_t>(reader.header().num_nodes - 1));
  std::vector<double> latencies_us;
  latencies_us.reserve(config.reads);

  const auto bench_start = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < config.reads; ++i) {
    const std::uint32_t node_id = pick(rng);
    const auto read_start = std::chrono::steady_clock::now();
    reader.read_node_into(node_id, page.get());
    const auto read_end = std::chrono::steady_clock::now();
    latencies_us.push_back(
        std::chrono::duration<double, std::micro>(read_end - read_start)
            .count());
  }
  const auto bench_end = std::chrono::steady_clock::now();

  const double total_seconds =
      std::chrono::duration<double>(bench_end - bench_start).count();
  const double qps =
      total_seconds > 0.0 ? static_cast<double>(config.reads) / total_seconds
                          : 0.0;
  double sum = 0.0;
  for (const double value : latencies_us) {
    sum += value;
  }
  std::sort(latencies_us.begin(), latencies_us.end());

  std::cout << "sync_disk_read_bench\n";
  std::cout << "nodes=" << reader.header().num_nodes
            << " dim=" << reader.header().dim
            << " degree=" << reader.header().max_degree
            << " direct_io=" << (config.direct_io ? 1 : 0) << "\n";
  std::cout << "reads=" << config.reads << "\n";
  std::cout << "qps=" << qps << "\n";
  std::cout << "avg_us=" << (latencies_us.empty() ? 0.0 : sum / latencies_us.size())
            << "\n";
  std::cout << "p50_us=" << percentile(latencies_us, 0.50) << "\n";
  std::cout << "p95_us=" << percentile(latencies_us, 0.95) << "\n";
  std::cout << "p99_us=" << percentile(latencies_us, 0.99) << "\n";
  std::cout << "bytes_read="
            << static_cast<std::uint64_t>(config.reads) *
                   agentmem::kDiskIndexPageSize
            << "\n";
  reader.close();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    run_benchmark(parse_args(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
  return 0;
}
