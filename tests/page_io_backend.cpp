#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "platform/page_io_backend.h"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() {
  namespace fs = std::filesystem;
  using namespace agent_aware::platform;
  const fs::path path = fs::temp_directory_path() / "agent-aware-page-io.bin";
  std::vector<char> expected(8192);
  for (std::size_t i = 0; i < expected.size(); ++i)
    expected[i] = static_cast<char>(i % 127);
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(expected.data(), expected.size());
  }

  auto backend = make_thread_pool_pread_backend();
  std::string error;
  require(backend->open(path.string(), 4096, 2, error), "backend open failed");
  const auto direct = backend->read(0, 4096);
  require(direct.ok && direct.data[100] == expected[100], "direct read failed");
  const auto tokens = backend->submit({
      {1, 4096, 4096, IoPriority::kDemandRead},
      {1, 4096, 4096, IoPriority::kDemandRead}}, error);
  require(tokens.size() == 2, "deduplicated submit failed");
  const auto first = backend->wait(tokens[0], UINT32_MAX);
  const auto second = backend->wait(tokens[1], UINT32_MAX);
  require(first.ok && second.ok && first.data == second.data,
          "deduplicated results differ");
  require(first.data[25] == expected[4096 + 25], "async read data mismatch");
  backend->close();
  fs::remove(path);
  return 0;
}
