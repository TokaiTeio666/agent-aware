#include "graph/disk_page_codec.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace agent_aware {

// Packed page 使用 SoA 布局：
// page header -> ids -> degrees -> vectors -> neighbor ids。
// decode 必须严格按 encode 的顺序消费字段。
std::size_t graph_record_bytes(std::size_t dim, std::size_t degree) {
  return sizeof(std::uint32_t) * 2 + dim * sizeof(float) +
         degree * sizeof(std::uint32_t);
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
  if (alignment == 0) {
    return value;
  }
  const std::uint64_t remainder = value % alignment;
  return remainder == 0 ? value : value + alignment - remainder;
}

std::size_t packed_nodes_per_page(std::size_t dim, std::size_t degree,
                                  std::size_t page_size) {
  if (page_size <= sizeof(std::uint32_t) * 2) {
    return 0;
  }
  const std::size_t per_node = sizeof(std::uint32_t) * 2 +
                               dim * sizeof(float) +
                               degree * sizeof(std::uint32_t);
  return (page_size - sizeof(std::uint32_t) * 2) / per_node;
}

std::vector<char> DiskPageCodec::encode_packed_page(
    std::uint32_t page_id, const VectorSet& base,
    const std::vector<std::vector<std::uint32_t>>& graph,
    const std::vector<std::uint32_t>& order, std::size_t nodes_per_page,
    std::size_t page_size, std::size_t degree) {
  const std::size_t begin = static_cast<std::size_t>(page_id) * nodes_per_page;
  const std::size_t end = std::min(begin + nodes_per_page, order.size());
  const auto node_count = static_cast<std::uint32_t>(end - begin);

  std::vector<char> page(page_size, 0);
  std::size_t offset = 0;
  put_bytes(page, offset, page_id);
  put_bytes(page, offset, node_count);

  // 定宽 id 和 degree 区块让每个 slot 的向量 payload 位于可预测偏移。
  for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {
    const std::uint32_t id =
        slot < node_count ? order[begin + slot]
                          : std::numeric_limits<std::uint32_t>::max();
    put_bytes(page, offset, id);
  }

  for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {
    const std::uint32_t packed_degree =
        slot < node_count
            ? static_cast<std::uint32_t>(graph[order[begin + slot]].size())
            : 0;
    put_bytes(page, offset, packed_degree);
  }

  for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {
    if (offset + base.dim * sizeof(float) > page.size()) {
      throw std::runtime_error("Packed vector block exceeds page size");
    }
    if (slot < node_count) {
      std::memcpy(page.data() + offset, base.row(order[begin + slot]),
                  base.dim * sizeof(float));
    }
    offset += base.dim * sizeof(float);
  }

  for (std::size_t slot = 0; slot < nodes_per_page; ++slot) {
    const std::uint32_t node_id =
        slot < node_count ? order[begin + slot]
                          : std::numeric_limits<std::uint32_t>::max();
    for (std::size_t n = 0; n < degree; ++n) {
      std::uint32_t neighbor = std::numeric_limits<std::uint32_t>::max();
      if (slot < node_count && n < graph[node_id].size()) {
        neighbor = graph[node_id][n];
      }
      put_bytes(page, offset, neighbor);
    }
  }

  return page;
}

PackedDiskGraphIndex::DecodedPage DiskPageCodec::decode_packed_page(
    const DiskGraphMetadata& metadata, std::uint32_t page_id,
    std::vector<char> page) {
  std::size_t cursor = 0;
  PackedDiskGraphIndex::DecodedPage decoded;
  decoded.bytes = std::move(page);
  decoded.page_id = get_bytes<std::uint32_t>(decoded.bytes, cursor);
  decoded.node_count = get_bytes<std::uint32_t>(decoded.bytes, cursor);
  if (decoded.page_id != page_id ||
      decoded.node_count > metadata.nodes_per_page) {
    throw std::runtime_error("Corrupted packed graph page header");
  }

  const std::size_t required = sizeof(std::uint32_t) * 2 +
      static_cast<std::size_t>(metadata.nodes_per_page) *
          (sizeof(std::uint32_t) * 2 +
           static_cast<std::size_t>(metadata.dim) * sizeof(float) +
           static_cast<std::size_t>(metadata.degree) * sizeof(std::uint32_t));
  if (required > decoded.bytes.size()) {
    throw std::runtime_error("Packed graph page payload is truncated");
  }

  return decoded;
}

PackedDiskGraphIndex::DiskNode DiskPageCodec::node_at(
    const DiskGraphMetadata& metadata,
    const PackedDiskGraphIndex::DecodedPage& page, std::uint32_t slot) {
  if (slot >= page.node_count) {
    throw std::runtime_error("Packed graph node slot is out of range");
  }
  const std::size_t ids = sizeof(std::uint32_t) * 2;
  const std::size_t degrees =
      ids + static_cast<std::size_t>(metadata.nodes_per_page) * sizeof(std::uint32_t);
  const std::size_t vectors =
      degrees + static_cast<std::size_t>(metadata.nodes_per_page) * sizeof(std::uint32_t);
  const std::size_t neighbors = vectors +
      static_cast<std::size_t>(metadata.nodes_per_page) * metadata.dim * sizeof(float);
  PackedDiskGraphIndex::DiskNode node;
  std::memcpy(&node.id, page.data() + ids + slot * sizeof(std::uint32_t),
              sizeof(node.id));
  std::memcpy(&node.neighbor_count,
              page.data() + degrees + slot * sizeof(std::uint32_t),
              sizeof(node.neighbor_count));
  if (node.neighbor_count > metadata.degree) {
    throw std::runtime_error("Packed graph node degree is corrupted");
  }
  node.vector_offset = vectors +
      static_cast<std::size_t>(slot) * metadata.dim * sizeof(float);
  node.neighbor_offset = neighbors +
      static_cast<std::size_t>(slot) * metadata.degree * sizeof(std::uint32_t);
  return node;
}

PackedDiskGraphIndex::DiskNode DiskPageCodec::find_node(
    const DiskGraphMetadata& metadata,
    const PackedDiskGraphIndex::DecodedPage& page, std::uint32_t node_id) {
  for (std::uint32_t slot = 0; slot < page.node_count; ++slot) {
    auto node = node_at(metadata, page, slot);
    if (node.id == node_id) return node;
  }
  throw std::runtime_error("Node is missing from packed page");
}

const float* DiskPageCodec::vector_data(
    const DiskGraphMetadata& metadata,
    const PackedDiskGraphIndex::DecodedPage& page,
    const PackedDiskGraphIndex::DiskNode& node) {
  const std::size_t bytes = static_cast<std::size_t>(metadata.dim) *
                            sizeof(float);
  if (node.vector_offset + bytes > page.size()) {
    throw std::runtime_error("Packed node vector points outside page");
  }
  return reinterpret_cast<const float*>(page.data() + node.vector_offset);
}

}  // namespace agent_aware
