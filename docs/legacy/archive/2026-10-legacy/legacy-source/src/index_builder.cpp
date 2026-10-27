#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "data/dataset.h"
#include "graph/disk_graph_index.h"

namespace fs = std::filesystem;
using namespace agent_aware;

namespace {
struct Args {
  bool help = false;
  bool synthetic = false;
  std::string sift_dir = "data/sift";
  std::string base_path;
  std::size_t base_limit = 1000000;
  std::size_t synthetic_dim = 32;
  std::size_t synthetic_clusters = 16;
  std::uint32_t seed = 42;
  fs::path index_path = "indexes/sift.idx";
  fs::path manifest_path;
  bool rebuild = false;
  std::size_t graph_degree = 32;
  std::size_t page_size = 4096;
  std::size_t pq_subspaces = 32;
  std::size_t pq_centroids = 256;
  std::size_t pq_train_limit = 100000;
  std::size_t pq_iterations = 8;
};

std::string value(int& i, int argc, char** argv, const std::string& option) {
  const std::string current = argv[i];
  const auto eq = current.find('=');
  if (eq != std::string::npos) return current.substr(eq + 1);
  if (++i >= argc) throw std::runtime_error("Missing value for " + option);
  return argv[i];
}
std::size_t number(int& i, int argc, char** argv, const std::string& option) {
  return static_cast<std::size_t>(std::stoull(value(i, argc, argv, option)));
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string n = std::string(argv[i]).substr(0, std::string(argv[i]).find('='));
    if (n == "--help" || n == "-h") a.help = true;
    else if (n == "--synthetic") a.synthetic = number(i, argc, argv, n) != 0;
    else if (n == "--sift-dir") a.sift_dir = value(i, argc, argv, n);
    else if (n == "--base") a.base_path = value(i, argc, argv, n);
    else if (n == "--base-limit") a.base_limit = number(i, argc, argv, n);
    else if (n == "--synthetic-dim") a.synthetic_dim = number(i, argc, argv, n);
    else if (n == "--synthetic-clusters") a.synthetic_clusters = number(i, argc, argv, n);
    else if (n == "--seed") a.seed = static_cast<std::uint32_t>(number(i, argc, argv, n));
    else if (n == "--index-path") a.index_path = value(i, argc, argv, n);
    else if (n == "--manifest") a.manifest_path = value(i, argc, argv, n);
    else if (n == "--rebuild") a.rebuild = number(i, argc, argv, n) != 0;
    else if (n == "--graph-degree") a.graph_degree = number(i, argc, argv, n);
    else if (n == "--page-size") a.page_size = number(i, argc, argv, n);
    else if (n == "--pq-subspaces") a.pq_subspaces = number(i, argc, argv, n);
    else if (n == "--pq-centroids") a.pq_centroids = number(i, argc, argv, n);
    else if (n == "--pq-train-limit") a.pq_train_limit = number(i, argc, argv, n);
    else if (n == "--pq-iterations") a.pq_iterations = number(i, argc, argv, n);
    else throw std::runtime_error("Unknown builder option: " + n);
  }
  return a;
}

void help() {
  std::cout << "Usage: agent-aware-build [options]\n"
            << "  --base PATH or --sift-dir PATH\n"
            << "  --synthetic 0|1 --base-limit N --synthetic-dim N\n"
            << "  --index-path PATH --manifest PATH --rebuild 0|1\n"
            << "  --graph-degree N --page-size N\n"
            << "  --pq-subspaces N --pq-centroids N --pq-train-limit N --pq-iterations N\n";
}

void write_manifest_atomically(const fs::path& path,
                               const DiskGraphMetadata& metadata,
                               std::uint32_t entry_id) {
  if (entry_id >= metadata.vector_count) {
    throw std::runtime_error("Vamana medoid exceeds packed index vector count");
  }
  if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
  const fs::path temporary = fs::path(path.string() + ".tmp");
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot create build manifest: " + temporary.string());
    out << "{\n  \"format_version\": " << metadata.format_version
        << ",\n  \"vector_count\": " << metadata.vector_count
        << ",\n  \"dim\": " << metadata.dim
        << ",\n  \"index_bytes\": " << metadata.index_bytes
        << ",\n  \"entry_strategy\": \"single-medoid\""
        << ",\n  \"entry_id\": " << entry_id << "\n}\n";
    if (!out) throw std::runtime_error("Failed to write build manifest: " + temporary.string());
  }
  std::error_code error;
  fs::rename(temporary, path, error);
  if (error) {
    fs::remove(path, error);
    error.clear();
    fs::rename(temporary, path, error);
  }
  if (error) {
    fs::remove(temporary);
    throw std::runtime_error("Cannot publish build manifest: " + error.message());
  }
}
}

int main(int argc, char** argv) {
  try {
    const Args a = parse(argc, argv);
    if (a.help) { help(); return 0; }
    if (fs::exists(a.index_path) && !a.rebuild) {
      throw std::runtime_error(
          "Index already exists; pass --rebuild 1 or choose another --index-path");
    }
    VectorSet base;
    if (a.synthetic) {
      SyntheticConfig c;
      c.base_count = a.base_limit; c.query_count = 1; c.dim = a.synthetic_dim;
      c.clusters = std::min(a.synthetic_clusters, a.base_limit);
      c.seed = a.seed; c.workload = "agent";
      base = generate_synthetic(c).base;
    } else {
      DatasetPaths p;
      p.sift_dir = a.sift_dir; p.base = a.base_path;
      if (p.base.empty()) p.base = resolve_sift_paths(p.sift_dir).base;
      base = load_fvecs(p.base, a.base_limit);
    }
    PqAdcModel pq;
    PQTrainingConfig pc;
    pc.subspaces = a.pq_subspaces; pc.centroids = a.pq_centroids;
    pc.train_limit = a.pq_train_limit; pc.iterations = a.pq_iterations; pc.seed = a.seed;
    pq.train(base, pc);
    DiskGraphBuildConfig bc;
    bc.degree = a.graph_degree; bc.page_size = a.page_size; bc.random_seed = a.seed; bc.pq_model = &pq;
    if (!a.index_path.parent_path().empty()) fs::create_directories(a.index_path.parent_path());
    const auto build_result =
        PackedDiskGraphBuilder::build(base, a.index_path.string(), bc);
    const fs::path manifest = a.manifest_path.empty() ? fs::path(a.index_path.string() + ".manifest.json") : a.manifest_path;
    PackedDiskGraphIndex index(a.index_path.string());
    write_manifest_atomically(manifest, index.metadata(), build_result.medoid);
    std::cout << "built index " << a.index_path << "\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << "error: " << e.what() << "\n"; return 1; }
}
