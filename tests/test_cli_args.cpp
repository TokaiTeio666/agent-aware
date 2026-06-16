#include <stdexcept>
#include <string>
#include <vector>

#include "agentmem/benchmark/cli_args.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

agentmem::benchmark::Args parse(std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  return agentmem::benchmark::parse_args(
      static_cast<int>(argv.size()), argv.data());
}

void test_p4_plan_aliases() {
  const auto args = parse({
      "agentmem_flow",
      "--engine=graph",
      "--async_io=1",
      "--io_backend=uring",
      "--io_depth=64",
      "--io_batch_size=16",
      "--prefetch=topology",
      "--prefetch_width=8",
      "--prefetch_depth=1",
      "--dedup_pages=1",
  });

  require(args.engine == "graph", "engine parses from inline value");
  require(args.io_mode == "io_uring", "io backend alias maps to io_uring");
  require(args.io_depth == 64, "io depth alias parses");
  require(args.io_batch_size == 16, "io batch size alias parses");
  require(args.prefetch_policy == "frontier-next-hop",
          "topology prefetch alias maps to frontier-next-hop");
  require(args.prefetch_width == 8, "prefetch width alias parses");
  require(args.prefetch_depth == 1, "prefetch depth alias parses");
  require(args.page_dedup, "dedup pages alias parses");
}

void test_async_io_disable_alias() {
  const auto args = parse({
      "agentmem_flow",
      "--async-io=0",
      "--io-backend=sync",
      "--prefetch=off",
      "--page-dedup=0",
  });

  require(args.io_mode == "pread", "async disable alias maps to pread");
  require(args.prefetch_policy == "none", "prefetch off alias parses");
  require(!args.page_dedup, "page dedup bool parses");
}

}  // namespace

int main() {
  test_p4_plan_aliases();
  test_async_io_disable_alias();
  return 0;
}
