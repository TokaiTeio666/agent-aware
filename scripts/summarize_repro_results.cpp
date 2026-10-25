// Summarize results/<run>/ JSON into Markdown, for hosts without Python.
// Keeps the same tables as scripts/summarize_repro_results.py (search matrix,
// mixed read/write, recovery); keep the two in sync.
//
// Build (MSVC): cl /nologo /EHsc /O2 /MD /std:c++17 /utf-8
//                    scripts/summarize_repro_results.cpp /Fe:summarize.exe
// Usage: summarize.exe results\2026-10-25-windows-msvc

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Flattens a JSON object into "path": value pairs, where path uses '.' between
// keys (e.g. "write_pipeline.flush_failures") and "[i]" for array elements.
using Values = std::map<std::string, std::string>;

class Parser {
 public:
  explicit Parser(std::string text) : text_(std::move(text)) {}

  Values flatten() {
    Values values;
    skip_space();
    parse_value("", values);
    return values;
  }

 private:
  void skip_space() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
            text_[pos_] == '\t')) {
      ++pos_;
    }
  }

  std::string parse_string() {
    std::string out;
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      return out;
    }
    ++pos_;
    while (pos_ < text_.size() && text_[pos_] != '"') {
      char ch = text_[pos_++];
      if (ch == '\\' && pos_ < text_.size()) {
        const char escaped = text_[pos_++];
        switch (escaped) {
          case 'n': out.push_back(' '); break;
          case 't': out.push_back(' '); break;
          default: out.push_back(escaped); break;
        }
      } else {
        out.push_back(ch);
      }
    }
    ++pos_;
    return out;
  }

  std::string parse_scalar() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != '}' &&
           text_[pos_] != ']' && text_[pos_] != ' ' && text_[pos_] != '\n' &&
           text_[pos_] != '\r' && text_[pos_] != '\t') {
      ++pos_;
    }
    return text_.substr(start, pos_ - start);
  }

  std::string join(const std::string& path, const std::string& key) const {
    return path.empty() ? key : path + "." + key;
  }

  void parse_value(const std::string& path, Values& values) {
    skip_space();
    if (pos_ >= text_.size()) {
      return;
    }
    const char ch = text_[pos_];
    if (ch == '{') {
      ++pos_;
      skip_space();
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        values[path] = "{}";
        return;
      }
      while (pos_ < text_.size()) {
        skip_space();
        const std::string key = parse_string();
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ':') {
          ++pos_;
        }
        parse_value(join(path, key), values);
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < text_.size() && text_[pos_] == '}') {
          ++pos_;
        }
        break;
      }
      return;
    }
    if (ch == '[') {
      ++pos_;
      std::size_t index = 0;
      skip_space();
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        values[path] = "[]";
        return;
      }
      while (pos_ < text_.size()) {
        parse_value(path + "[" + std::to_string(index++) + "]", values);
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (pos_ < text_.size() && text_[pos_] == ']') {
          ++pos_;
        }
        break;
      }
      return;
    }
    if (ch == '"') {
      values[path] = parse_string();
      return;
    }
    values[path] = parse_scalar();
  }

  std::string text_;
  std::size_t pos_ = 0;
};

Values load(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  return Parser(std::move(text)).flatten();
}

bool get_number(const Values& values, const std::string& key, double* out) {
  const auto found = values.find(key);
  if (found == values.end() || found->second.empty() ||
      found->second == "null" || found->second == "true" ||
      found->second == "false") {
    return false;
  }
  try {
    std::size_t used = 0;
    *out = std::stod(found->second, &used);
    return used == found->second.size();
  } catch (...) {
    return false;
  }
}

std::vector<double> collect(const std::vector<Values>& rounds,
                            const std::string& key) {
  std::vector<double> values;
  for (const auto& round : rounds) {
    double value = 0.0;
    if (get_number(round, key, &value)) {
      values.push_back(value);
    }
  }
  return values;
}

std::string format(double value, int digits) {
  char buffer[64] = {0};
  std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
  return buffer;
}

std::string range_text(std::vector<double> values, int digits) {
  if (values.empty()) {
    return "-";
  }
  std::sort(values.begin(), values.end());
  double median = values[values.size() / 2];
  if (values.size() % 2 == 0) {
    median = (values[values.size() / 2 - 1] + median) / 2.0;
  }
  return format(median, digits) + " (" + format(values.front(), digits) + "-" +
         format(values.back(), digits) + ")";
}

constexpr double kMiB = 1024.0 * 1024.0;

std::vector<fs::path> subdirs(const fs::path& dir, const std::string& prefix) {
  std::vector<fs::path> out;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(dir, error)) {
    if (entry.is_directory() && entry.path().filename().string().rfind(prefix, 0) == 0) {
      out.push_back(entry.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<Values> rounds_in(const fs::path& dir) {
  std::vector<fs::path> paths;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(dir, error)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file() && name.rfind("round", 0) == 0 &&
        name.size() > 5 && name.substr(name.size() - 5) == ".json") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  std::vector<Values> rounds;
  for (const auto& path : paths) {
    rounds.push_back(load(path));
  }
  return rounds;
}

std::string text_of(const Values& values, const std::string& key) {
  const auto found = values.find(key);
  return found == values.end() ? std::string("-") : found->second;
}

void print_search(const fs::path& run) {
  std::printf(
      "| cache policy | search width | Recall@10 | Avg ms | P95 ms | P99 ms | "
      "P999 ms | QPS | reads/query | cache hit | peak RSS (MB) |\n");
  std::printf(
      "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
      "---: |\n");
  for (const auto& policy_dir : subdirs(run, "search-")) {
    std::vector<fs::path> widths;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(policy_dir, error)) {
      if (entry.is_directory()) {
        widths.push_back(entry.path());
      }
    }
    std::sort(widths.begin(), widths.end(), [](const fs::path& lhs, const fs::path& rhs) {
      return std::stoi(lhs.filename().string().substr(2)) <
             std::stoi(rhs.filename().string().substr(2));
    });
    for (const auto& width_dir : widths) {
      const auto rounds = rounds_in(width_dir);
      if (rounds.empty()) {
        continue;
      }
      std::vector<double> rss;
      for (double value : collect(rounds, "measured_peak_rss_bytes")) {
        rss.push_back(value / kMiB);
      }
      std::printf("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
                  text_of(rounds[0], "cache_policy").c_str(),
                  text_of(rounds[0], "effective_search_width").c_str(),
                  range_text(collect(rounds, "recall_at_k"), 4).c_str(),
                  range_text(collect(rounds, "avg_query_latency_ms"), 4).c_str(),
                  range_text(collect(rounds, "latency_p95_ms"), 4).c_str(),
                  range_text(collect(rounds, "latency_p99_ms"), 4).c_str(),
                  range_text(collect(rounds, "latency_p999_ms"), 4).c_str(),
                  range_text(collect(rounds, "qps"), 1).c_str(),
                  range_text(collect(rounds, "reads_per_query"), 2).c_str(),
                  range_text(collect(rounds, "cache_hit_rate"), 4).c_str(),
                  range_text(rss, 2).c_str());
    }
  }
}

// 混合读写 JSON 有两种布局：未指定 --duration_sec 时只有一个未命名的顶层场景
// （指标在顶层）；指定 --duration_sec（实验驱动就是这么跑的）时指标被包在
// "scenarios"[0] 里。两种都必须能被读到，否则混合读写表会整列显示 "-"。
// 驱动每个文件只产生一个场景，取第一个即驱动意图。
Values scenario_of(const Values& document) {
  if (document.count("read_qps") != 0) {
    return document;
  }
  Values flat = document;
  for (const auto& [key, value] : document) {
    const std::string prefix = "scenarios[0].";
    if (key.rfind(prefix, 0) == 0) {
      flat[key.substr(prefix.size())] = value;
    }
  }
  return flat;
}

void print_mixed(const fs::path& run) {
  std::printf(
      "| config | Read QPS | Write QPS | Read P95 ms | Read P99 ms | "
      "Recovery ms | Delta search ms/read | Compaction ok/attempt | SSTables | "
      "Peak RSS MB |\n");
  std::printf(
      "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
  for (const auto& config_dir : subdirs(run / "mixed", "")) {
    std::vector<Values> raw = rounds_in(config_dir);
    if (raw.empty()) {
      continue;
    }
    std::vector<Values> rounds;
    for (const auto& document : raw) {
      rounds.push_back(scenario_of(document));
    }
    std::vector<double> rss;
    for (double value : collect(rounds, "process_peak_rss_bytes")) {
      rss.push_back(value / kMiB);
    }
    std::string compaction;
    for (const auto& round : rounds) {
      if (!compaction.empty()) {
        compaction += ", ";
      }
      compaction += text_of(round, "compaction_count") + "/" +
                    text_of(round, "compaction_attempted");
    }
    std::printf("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
                config_dir.filename().string().c_str(),
                range_text(collect(rounds, "read_qps"), 1).c_str(),
                range_text(collect(rounds, "write_qps"), 2).c_str(),
                range_text(collect(rounds, "read_p95_ms"), 3).c_str(),
                range_text(collect(rounds, "read_p99_ms"), 3).c_str(),
                range_text(collect(rounds, "recovery_time_ms"), 2).c_str(),
                range_text(collect(rounds, "read_breakdown.delta_search_avg_ms"), 3)
                    .c_str(),
                compaction.c_str(),
                range_text(collect(rounds, "sstable_count"), 0).c_str(),
                range_text(rss, 2).c_str());
  }
}

void print_recovery(const fs::path& run) {
  std::printf(
      "| file | status | writes committed | visible delta records | SSTables "
      "| WAL segments | WAL records replayed | truncated WAL segments | "
      "recovery ms |\n");
  std::printf("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
  std::vector<fs::path> paths;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(run / "failure", error)) {
    if (entry.is_regular_file()) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const auto& path : paths) {
    const auto flat = scenario_of(load(path));
    std::printf("| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
                path.filename().string().c_str(), text_of(flat, "status").c_str(),
                text_of(flat, "committed_write_records").c_str(),
                text_of(flat, "recovery.visible_delta_records").c_str(),
                text_of(flat, "recovery.sstable_count").c_str(),
                text_of(flat, "recovery.wal_segments").c_str(),
                text_of(flat, "recovery.wal_records").c_str(),
                text_of(flat, "recovery.wal_truncated_segments").c_str(),
                text_of(flat, "recovery_time_ms").c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <results/run directory>\n", argv[0]);
    return 2;
  }
  const fs::path run = argv[1];
  std::printf("### Search matrix\n\n");
  print_search(run);
  std::printf("\n### Mixed read/write\n\n");
  print_mixed(run);
  std::printf("\n### Recovery check\n\n");
  print_recovery(run);
  return 0;
}
