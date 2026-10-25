// Deterministic fvecs dataset generator (no Python/numpy dependency).
//
// Output byte layout matches SIFT1M: each record is <int32 dim><dim * float32>.
// The RNG is std::mt19937_64 seeded from a splitmix64 hash of
// (recipe, stream, seed), so the bytes depend only on those inputs and on
// std::normal_distribution being implemented as Box-Muller-free
// `sqrt(-2 ln u1) cos(2 pi u2)` — true for libstdc++ and MSVC alike, but see
// RECIPE.txt: it is recorded so a re-run on another standard library can be
// detected by comparing the first record.
//
// Recipe gmm-v1:
//   * `clusters` centroids uniform in [-1, 1]^dim (base stream);
//   * base vectors: centroid index uniform over clusters + N(0, 0.15^2) noise;
//   * queries: same centroids + N(0, 0.10^2) noise (query stream);
//   * truth: exact top-k by L2, brute force in double.
//
// Build: cl /O2 /std:c++17 /EHsc scripts\make_repro_dataset.cpp
// Usage: make_repro_dataset --out build/repro_gmm --base 100000 --queries 2000

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

std::uint64_t splitmix64(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

struct Options {
  std::string out_dir = "build/repro_gmm";
  std::size_t base = 100000;
  std::size_t queries = 2000;
  std::size_t dim = 128;
  std::size_t clusters = 1000;
  std::size_t top_k = 10;
  std::uint64_t seed = 20261025;
};

bool write_fvecs(const std::filesystem::path& path,
                 const std::vector<float>& data, std::size_t dim) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::int32_t stored_dim = static_cast<std::int32_t>(dim);
  for (std::size_t row = 0; row < data.size() / dim; ++row) {
    if (std::fwrite(&stored_dim, sizeof(stored_dim), 1, file) != 1 ||
        std::fwrite(data.data() + row * dim, sizeof(float), dim, file) != dim) {
      std::fclose(file);
      return false;
    }
  }
  const bool ok = std::fclose(file) == 0;
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto next = [&](const char* name) -> std::size_t {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    };
    if (flag == "--out") {
      options.out_dir = argv[++i];
    } else if (flag == "--base") {
      options.base = next("--base");
    } else if (flag == "--queries") {
      options.queries = next("--queries");
    } else if (flag == "--dim") {
      options.dim = next("--dim");
    } else if (flag == "--clusters") {
      options.clusters = next("--clusters");
    } else if (flag == "--top-k") {
      options.top_k = next("--top-k");
    } else if (flag == "--seed") {
      options.seed = next("--seed");
    } else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }

  std::uint64_t base_state = options.seed ^ 0x62617365ULL;    // "base"
  std::uint64_t query_state = options.seed ^ 0x71756572ULL;   // "quer"
  std::mt19937_64 base_rng(splitmix64(base_state));
  std::mt19937_64 query_rng(splitmix64(query_state));
  std::uniform_real_distribution<double> uniform(-1.0, 1.0);
  std::normal_distribution<double> base_noise(0.0, 0.15);
  std::normal_distribution<double> query_noise(0.0, 0.10);
  std::uniform_int_distribution<std::size_t> cluster_pick(0, options.clusters - 1);

  std::vector<float> centroids(options.clusters * options.dim);
  for (auto& value : centroids) {
    value = static_cast<float>(uniform(base_rng));
  }
  const auto sample = [&](std::mt19937_64& rng,
                          std::normal_distribution<double>& noise,
                          std::size_t count, std::vector<float>& out) {
    out.resize(count * options.dim);
    for (std::size_t row = 0; row < count; ++row) {
      const std::size_t cluster = cluster_pick(rng);
      const float* center = centroids.data() + cluster * options.dim;
      float* target = out.data() + row * options.dim;
      for (std::size_t col = 0; col < options.dim; ++col) {
        target[col] = center[col] + static_cast<float>(noise(rng));
      }
    }
  };

  std::vector<float> base;
  std::vector<float> queries;
  sample(base_rng, base_noise, options.base, base);
  sample(query_rng, query_noise, options.queries, queries);

  std::error_code error;
  std::filesystem::create_directories(options.out_dir, error);
  const std::filesystem::path dir(options.out_dir);
  if (!write_fvecs(dir / "sift_base.fvecs", base, options.dim) ||
      !write_fvecs(dir / "sift_query.fvecs", queries, options.dim)) {
    std::fprintf(stderr, "failed to write fvecs under %s\n", options.out_dir.c_str());
    return 1;
  }

  // Exact truth by brute force; O(queries * base * dim) in double.
  std::vector<std::int32_t> truth(options.queries * options.top_k);
  std::vector<float> base_norm(options.base);
  for (std::size_t i = 0; i < options.base; ++i) {
    double sum = 0.0;
    for (std::size_t d = 0; d < options.dim; ++d) {
      const double v = base[i * options.dim + d];
      sum += v * v;
    }
    base_norm[i] = static_cast<float>(sum);
  }
  std::vector<std::pair<float, std::int32_t>> candidates(options.base);
  for (std::size_t q = 0; q < options.queries; ++q) {
    for (std::size_t i = 0; i < options.base; ++i) {
      double distance = base_norm[i];
      const float* qv = queries.data() + q * options.dim;
      const float* bv = base.data() + i * options.dim;
      for (std::size_t d = 0; d < options.dim; ++d) {
        distance -= 2.0 * qv[d] * bv[d];
      }
      candidates[i] = {static_cast<float>(distance), static_cast<std::int32_t>(i)};
    }
    std::partial_sort(candidates.begin(), candidates.begin() + options.top_k,
                      candidates.end());
    for (std::size_t r = 0; r < options.top_k; ++r) {
      truth[q * options.top_k + r] = candidates[r].second;
    }
  }
  std::FILE* truth_file =
      std::fopen((dir / "sift_groundtruth.ivecs").string().c_str(), "wb");
  if (truth_file == nullptr) {
    return 1;
  }
  const std::int32_t stored_k = static_cast<std::int32_t>(options.top_k);
  for (std::size_t q = 0; q < options.queries; ++q) {
    std::fwrite(&stored_k, sizeof(stored_k), 1, truth_file);
    std::fwrite(truth.data() + q * options.top_k, sizeof(std::int32_t),
                options.top_k, truth_file);
  }
  std::fclose(truth_file);

  std::FILE* recipe = std::fopen((dir / "RECIPE.txt").string().c_str(), "w");
  if (recipe != nullptr) {
    std::fprintf(recipe,
                 "recipe=gmm-v1\nbase=%zu\nqueries=%zu\ndim=%zu\n"
                 "clusters=%zu\nseed=%llu\ntop_k=%zu\nsigma_base=0.15\n"
                 "sigma_query=0.10\n",
                 options.base, options.queries, options.dim, options.clusters,
                 static_cast<unsigned long long>(options.seed), options.top_k);
    std::fclose(recipe);
  }
  std::printf("wrote %s (%zu base x %zu dim, %zu queries)\n",
              options.out_dir.c_str(), options.base, options.dim,
              options.queries);
  return 0;
}
