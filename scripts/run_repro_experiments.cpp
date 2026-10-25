// Reproducible experiment driver for hosts without Python (e.g. the Windows
// MSVC build machine). Generates the same results/<run>/ layout as
// scripts/run_repro_experiments.py; keep the two matrices in sync.
//
// Build (MSVC): cl /O2 /std:c++17 /EHsc /utf-8 scripts\run_repro_experiments.cpp
// Usage: run_repro_experiments --bin-dir build\bin --index build\i100.idx ^
//            --dataset build\repro_gmm --out results\2026-10-25-windows-msvc ^
//            [--scratch build\repro_scratch] [--query-limit 2000] ^
//            [--mixed-duration 10] [--skip-failure]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace {

struct Args {
  fs::path bin_dir = "build/bin";
  fs::path index;
  fs::path dataset;
  fs::path out;
  fs::path scratch = "build/repro_scratch";
  std::size_t query_limit = 2000;
  std::size_t base_count = 100000;
  int mixed_duration = 10;
  int mixed_width = 156;
  bool skip_failure = false;
};

const std::vector<int> kWidths = {96, 128, 156, 192};
const std::vector<std::string> kPolicies = {"none", "lru", "2q",
                                            "graph-aware-2q"};
const int kRounds = 3;

struct MixedConfig {
  const char* id;
  const char* read;
  const char* write;
  int compaction;
};
const std::vector<MixedConfig> kMixed = {
    {"read-only", "1.0", "0.0", 0},
    {"95-5-no-compaction", "0.95", "0.05", 0},
    {"95-5-compaction", "0.95", "0.05", 1},
    {"80-20-compaction", "0.8", "0.2", 1},
};

std::string quote(const std::string& value) {
  return "\"" + value + "\"";
}

fs::path binary(const fs::path& dir, const std::string& name) {
  const fs::path with_exe = dir / (name + ".exe");
  return fs::exists(with_exe) ? with_exe : dir / name;
}

std::string join(const std::vector<std::string>& parts) {
  std::string joined;
  for (const auto& part : parts) {
    if (!joined.empty()) {
      joined.push_back(' ');
    }
    joined += quote(part);
  }
  return joined;
}

int run(const std::vector<std::string>& parts) {
  std::string command = join(parts);
  if (std::getenv("REPRO_VERBOSE") != nullptr) {
    std::fprintf(stderr, "CMD: %s\n", command.c_str());
    std::fflush(stderr);
  }
  // cmd.exe treats "/foo" as a switch and cannot open forward-slash paths for
  // redirection, so the whole command line uses backslashes. Options in this
  // matrix are all "--long" form, so this is unambiguous (keep this in sync
  // with run_repro_experiments.py, which only normalises the exec-path args).
  std::replace(command.begin(), command.end(), '/', '\\');
  // Create the benchmark's output directory here: with a fixed matrix the
  // benchmarks themselves only create the file.
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i] == "--output-json" || parts[i] == "--output") {
      fs::create_directories(fs::absolute(parts[i + 1]).parent_path());
    }
  }
  // A redirected stdout is fully buffered and is lost when a launched
  // benchmark aborts, so progress is appended to repro_progress.txt.
  // NOTE: do not call fs::remove() from this program — on this MSVC build it
  // crashes with STATUS_STACK_BUFFER_OVERRUN inside <filesystem>; use
  // std::remove() for single files.
  if (std::FILE* out = std::fopen("repro_progress.txt", "a")) {
    std::fprintf(out, "> %s\n", command.c_str());
    std::fclose(out);
  }
  // Calling `program args` directly lets CreateProcess locate the program even
  // when it has no directory component; routing it through cmd.exe fails with
  // "The filename, directory name, or volume label syntax is incorrect".
  std::string program = fs::absolute(fs::path(parts[0])).string();
  std::replace(program.begin(), program.end(), '/', '\\');
  command = quote(program) + command.substr(std::min(command.find(' '), command.size()));
  // Spawning the program directly keeps argv intact: MSYS/Git-bash runtimes in
  // PATH mangle a cmd.exe command line, so routing through the C runtime's
  // system() (which always goes via cmd.exe) is not reliable here.
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::string mutable_command = command;
  if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0,
                      nullptr, nullptr, &startup, &process)) {
    std::fprintf(stderr, "CreateProcess failed: %s\n", command.c_str());
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}

std::string sha256(const fs::path& path) {
  // certutil ships with Windows; parse its "SHA256 hash of ...:" block.
  const std::string tmp = (fs::temp_directory_path() / "repro_sha.txt").string();
  std::system(("certutil -hashfile " + quote(path.string()) + " SHA256 > " +
               quote(tmp) + " 2>&1").c_str());
  std::ifstream input(tmp);
  std::string line;
  std::string digest = "unknown";
  while (std::getline(input, line)) {
    // The hash line is hex only (plus CR); skip the header and trailing text.
    std::string trimmed;
    for (char ch : line) {
      if (ch != '\r' && ch != ' ') {
        trimmed.push_back(ch);
      }
    }
    const bool hex = !trimmed.empty() &&
                     trimmed.size() >= 40 &&
                     std::all_of(trimmed.begin(), trimmed.end(), [](char ch) {
                       return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                              (ch >= 'A' && ch <= 'F');
                     });
    if (hex) {
      digest = trimmed;
      break;
    }
  }
  std::remove(tmp.c_str());
  return digest;
}

std::string file_text(const fs::path& path) {
  std::ifstream input(path);
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

std::string json_escape(const std::string& text) {
  std::string escaped;
  for (char ch : text) {
    if (ch == '\\' || ch == '"') {
      escaped.push_back('\\');
    }
    if (ch == '\n' || ch == '\r') {
      escaped.push_back(' ');
      continue;
    }
    escaped.push_back(ch);
  }
  return escaped;
}

std::vector<std::string> search_command(const Args& args, const std::string& policy,
                                        int width, const fs::path& output) {
  // Prefer the current build; results/2026-10-25-windows-msvc-v2 was produced
  // with agent-aware-search_v5.exe (superseded name kept for reproducibility).
  const fs::path search_bin =
      fs::exists(args.bin_dir / "agent-aware-search_v5.exe")
          ? args.bin_dir / "agent-aware-search_v5.exe"
          : binary(args.bin_dir, "agent-aware-search");
  return {search_bin.string(),
          "--sift-dir", args.dataset.string(),
          "--query", (args.dataset / "sift_query.fvecs").string(),
          "--truth", (args.dataset / "sift_groundtruth.ivecs").string(),
          "--index-path", args.index.string(),
          "--query-offset", "0",
          "--query-limit", std::to_string(args.query_limit),
          "--top-k", "10",
          "--search-width", std::to_string(width),
          "--beam-width", "16",
          "--io-mode", "pread",
          "--cache-policy", policy,
          "--memory-budget-ratio", "0.20",
          // The measured baseline RSS exceeds a 20% budget for this dataset, so
          // enforcing it would leave zero cache pages; run the policy comparison
          // with the requested pages and report the budget separately.
          "--memory-budget-enforce", "0",
          "--num-search-threads", "1",
          "--run-id", "search-" + policy + "-sw" + std::to_string(width),
          "--output-json", output.string()};
}

std::vector<std::string> mixed_command(const Args& args, const std::string& read,
                                       const std::string& write, int compaction,
                                       const fs::path& dynamic_dir,
                                       const fs::path& output) {
  return {binary(args.bin_dir, "bench_mixed_rw").string(),
          "--data_path", (args.dataset / "sift_base.fvecs").string(),
          "--base_count", std::to_string(args.base_count),
          "--query_count", std::to_string(args.query_limit),
          "--index_path", args.index.string(),
          "--dynamic_dir", dynamic_dir.string(),
          "--output", output.string(),
          "--duration_sec", std::to_string(args.mixed_duration),
          "--read_threads", "4",
          "--write_threads", "1",
          "--read_ratio", read,
          "--write_ratio", write,
          "--insert_ratio", "0.70",
          "--update_ratio", "0.25",
          "--delete_ratio", "0.05",
          "--recall_sample_rate", "0",
          "--enable_compaction", std::to_string(compaction),
          "--compaction_background", std::to_string(compaction),
          "--compaction_interval_ms", "1000",
          "--topk", "10",
          "--search_width", std::to_string(args.mixed_width),
          "--beam_width", "16",
          "--io_mode", "pread",
          "--cache_policy", "graph-aware-2q",
          "--enable_flush", "1"};
}

void write_manifest(const Args& args) {
  std::FILE* out = std::fopen((args.out / "manifest.json").string().c_str(), "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "cannot write manifest under %s\n", args.out.string().c_str());
    return;
  }
  std::fprintf(out, "{\n");
  std::fprintf(out, "  \"matrix\": {\n");
  std::fprintf(out, "    \"search_widths\": [96, 128, 156, 192],\n");
  std::fprintf(out, "    \"cache_policies\": [\"none\", \"lru\", \"2q\", \"graph-aware-2q\"],\n");
  std::fprintf(out, "    \"rounds\": %d,\n", kRounds);
  std::fprintf(out, "    \"query_limit\": %zu,\n", args.query_limit);
  std::fprintf(out, "    \"base_count\": %zu,\n", args.base_count);
  std::fprintf(out, "    \"mixed_duration_sec\": %d,\n", args.mixed_duration);
  std::fprintf(out, "    \"mixed_search_width\": %d\n", args.mixed_width);
  std::fprintf(out, "  },\n");
  // Digests are computed before writing so the (slow) certutil spawns do not
  // run between manifest writes.
  const std::string index_digest = sha256(args.index);
  const std::vector<std::pair<std::string, std::string>> digests = {
      {"agent-aware-search", sha256(binary(args.bin_dir, "agent-aware-search"))},
      {"bench_mixed_rw", sha256(binary(args.bin_dir, "bench_mixed_rw"))}};
  std::fprintf(out, "  \"index\": {\"path\": \"%s\", \"sha256\": \"%s\"},\n",
               json_escape(args.index.string()).c_str(), index_digest.c_str());
  std::fprintf(out, "  \"binaries\": {\n");
  for (const auto& [name, digest] : digests) {
    std::fprintf(out, "    \"%s\": {\"sha256\": \"%s\"},\n", name.c_str(),
                 digest.c_str());
  }
  std::fprintf(out,
               "    \"note\": \"built with MSVC 14.42.34433, /O2 /MD /std:c++17, "
               "AGENT_AWARE_HAS_LIBURING=0, AGENT_AWARE_ENABLE_DIRECT_IO=0; the "
               "Windows build has no io_uring/O_DIRECT, so io_effective_mode is "
               "pread\"\n");
  std::fprintf(out, "  },\n");
  std::fprintf(out, "  \"dataset_recipe\": \"%s\"\n",
               json_escape(file_text(args.dataset / "RECIPE.txt")).c_str());
  std::fprintf(out, "}\n");
  std::fclose(out);
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", flag.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (flag == "--bin-dir") {
      args.bin_dir = value();
    } else if (flag == "--index") {
      args.index = value();
    } else if (flag == "--dataset") {
      args.dataset = value();
    } else if (flag == "--out") {
      args.out = value();
    } else if (flag == "--scratch") {
      args.scratch = value();
    } else if (flag == "--query-limit") {
      args.query_limit = static_cast<std::size_t>(std::stoull(value()));
    } else if (flag == "--base-count") {
      args.base_count = static_cast<std::size_t>(std::stoull(value()));
    } else if (flag == "--mixed-duration") {
      args.mixed_duration = std::stoi(value());
    } else if (flag == "--mixed-width") {
      args.mixed_width = std::stoi(value());
    } else if (flag == "--skip-failure") {
      args.skip_failure = true;
    } else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }
  if (args.index.empty() || args.dataset.empty() || args.out.empty()) {
    std::fprintf(stderr, "--index, --dataset and --out are required\n");
    return 2;
  }
  std::error_code error;
  fs::create_directories(args.out, error);
  fs::create_directories(args.scratch, error);
  write_manifest(args);

  for (const auto& policy : kPolicies) {
    for (int width : kWidths) {
      for (int round = 1; round <= kRounds; ++round) {
        const fs::path dir =
            args.out / ("search-" + policy) / ("sw" + std::to_string(width));
        fs::create_directories(dir, error);
        if (run(search_command(args, policy, width,
                               dir / ("round" + std::to_string(round) + ".json"))) !=
            0) {
          std::fprintf(stderr, "search run failed\n");
          return 1;
        }
      }
    }
  }

  for (const auto& config : kMixed) {
    for (int round = 1; round <= kRounds; ++round) {
      const fs::path dir = args.out / "mixed" / config.id;
      fs::create_directories(dir, error);
      const fs::path dynamic_dir =
          args.scratch / ("dyn-" + std::string(config.id) + "-" +
                          std::to_string(round));
      // A background compaction thread of the previous run can still hold a
      // handle for a moment on Windows; retry briefly instead of failing.
      for (int attempt = 0; attempt < 30; ++attempt) {
        error.clear();
        fs::remove_all(dynamic_dir, error);
        if (!error || !fs::exists(dynamic_dir)) {
          break;
        }
        Sleep(500);
      }
      if (run(mixed_command(args, config.read, config.write, config.compaction,
                            dynamic_dir,
                            dir / ("round" + std::to_string(round) + ".json"))) !=
          0) {
        std::fprintf(stderr, "mixed run failed\n");
        return 1;
      }
    }
  }

  if (!args.skip_failure) {
    const fs::path failure_dir = args.out / "failure";
    fs::create_directories(failure_dir, error);
    const fs::path clean = args.scratch / "fail-clean";
    fs::remove_all(clean, error);
    if (run(mixed_command(args, "0.95", "0.05", 0, clean,
                          failure_dir / "clean-close.json")) != 0 ||
        run(mixed_command(args, "1.0", "0.0", 0, clean,
                          failure_dir / "clean-reopen.json")) != 0) {
      std::fprintf(stderr, "failure check failed\n");
      return 1;
    }
  }
  std::printf("wrote %s\n", args.out.string().c_str());
  return 0;
}
