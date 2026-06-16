#include "agentmem/graph/disk_page_codec.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace agentmem {

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
  const auto node_count = get_bytes<std::uint32_t>(decoded.bytes, cursor);
  if (decoded.page_id != page_id || node_count > metadata.nodes_per_page) {
    throw std::runtime_error("Corrupted packed graph page header");
  }

  std::vector<std::uint32_t> ids(metadata.nodes_per_page);
  std::vector<std::uint32_t> degrees(metadata.nodes_per_page);
  for (std::uint32_t i = 0; i < metadata.nodes_per_page; ++i) {
    ids[i] = get_bytes<std::uint32_t>(decoded.bytes, cursor);
  }
  for (std::uint32_t i = 0; i < metadata.nodes_per_page; ++i) {
    degrees[i] = get_bytes<std::uint32_t>(decoded.bytes, cursor);
  }

  decoded.nodes.reserve(node_count);
  for (std::uint32_t slot = 0; slot < metadata.nodes_per_page; ++slot) {
    if (cursor + metadata.dim * sizeof(float) > decoded.bytes.size()) {
      throw std::runtime_error("Packed vector block is truncated");
    }
    if (slot < node_count) {
      PackedDiskGraphIndex::DiskNode node;
      node.id = ids[slot];
      node.vector_offset = cursor;
      decoded.nodes.push_back(std::move(node));
    }
    cursor += metadata.dim * sizeof(float);
  }

  for (std::uint32_t slot = 0; slot < metadata.nodes_per_page; ++slot) {
    std::vector<std::uint32_t> neighbors;
    if (slot < node_count) {
      neighbors.reserve(degrees[slot]);
    }
    for (std::uint32_t n = 0; n < metadata.degree; ++n) {
      const auto neighbor = get_bytes<std::uint32_t>(decoded.bytes, cursor);
      if (slot < node_count && n < degrees[slot] &&
          neighbor != std::numeric_limits<std::uint32_t>::max()) {
        neighbors.push_back(neighbor);
      }
    }
    if (slot < node_count) {
      decoded.nodes[slot].neighbors = std::move(neighbors);
    }
  }

  return decoded;
}

const PackedDiskGraphIndex::DiskNode& DiskPageCodec::find_node(
    const PackedDiskGraphIndex::DecodedPage& page, std::uint32_t node_id) {
  for (const auto& node : page.nodes) {
    if (node.id == node_id) {
      return node;
    }
  }
  throw std::runtime_error("Node is missing from packed page");
}

const float* DiskPageCodec::vector_data(
    const DiskGraphMetadata& metadata,
    const PackedDiskGraphIndex::DecodedPage& page,
    const PackedDiskGraphIndex::DiskNode& node) {
  const std::size_t bytes = static_cast<std::size_t>(metadata.dim) *
                            sizeof(float);
  if (node.vector_offset + bytes > page.bytes.size()) {
    throw std::runtime_error("Packed node vector points outside page");
  }
  return reinterpret_cast<const float*>(page.bytes.data() + node.vector_offset);
}

}  // namespace agentmem
