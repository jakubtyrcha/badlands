#include "mapgen/river_io.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

namespace badlands::mapgen {

namespace {

// "RIVG" read as a little-endian uint32 -- rejects an unrelated or
// zero-length file before a single count from it is trusted.
constexpr uint32_t kMagic = 0x47564952u;

// Field counts double as a structural version: a reader built against a
// different RiverNode/RiverEdge shape sees a mismatch here and errors
// instead of misinterpreting the bytes that follow.
constexpr uint32_t kNodeFieldCount = 11;       // pos.x, pos.y, ground, area,
                                                // Q, w, d, v, lake_id,
                                                // lake_kind, kind
constexpr uint32_t kEdgeScalarFieldCount = 4;  // from, to, strahler, shreve
constexpr uint32_t kEdgeArrayCount = 5;        // points, Q, width, depth, speed

// A node record's byte size is fixed, so the whole node block can be
// bounds-checked before allocating anything.
constexpr uint64_t kNodeRecordBytes =
    8 * sizeof(float) + sizeof(int32_t) + 2 * sizeof(uint8_t);

// An edge record is variable-length (its per-point arrays follow), but its
// HEADER is fixed: from, to, strahler, shreve, point_count. That minimum is
// enough to bound-check the edge count before allocating.
constexpr uint64_t kEdgeHeaderBytes = 4 * sizeof(int32_t) + sizeof(uint64_t);

template <typename T>
bool write_pod(std::ofstream& f, const T& v) {
  f.write(reinterpret_cast<const char*>(&v), sizeof(T));
  return static_cast<bool>(f);
}

template <typename T>
bool write_pod_vec(std::ofstream& f, const std::vector<T>& v) {
  if (v.empty()) return static_cast<bool>(f);
  f.write(reinterpret_cast<const char*>(v.data()),
         static_cast<std::streamsize>(v.size() * sizeof(T)));
  return static_cast<bool>(f);
}

template <typename T>
bool read_pod(std::ifstream& f, T& v) {
  f.read(reinterpret_cast<char*>(&v), sizeof(T));
  return static_cast<bool>(f);
}

// Reads `count` elements of T, but only after checking they fit in what is
// left of the file. A corrupt or merely truncated count must be reported
// rather than driving an attempt to allocate gigabytes before failing.
template <typename T>
bool read_pod_vec(std::ifstream& f, uint64_t count, std::vector<T>& out,
                  uint64_t file_size, const std::string& path,
                  std::string* error) {
  const uint64_t pos = static_cast<uint64_t>(f.tellg());
  // DIVIDE, do not multiply. `count * sizeof(T)` wraps for a count near
  // 2^61 -- so a corrupt file could produce a small product, sail past the
  // check, and reach resize() with the huge count. Comparing against the
  // remaining bytes divided by the element size cannot overflow.
  if (pos > file_size || count > (file_size - pos) / sizeof(T)) {
    if (error) *error = path + ": truncated (claims " + std::to_string(count) +
                        " elements, " + std::to_string(file_size - pos) +
                        " bytes remain)";
    return false;
  }
  const uint64_t want_bytes = count * sizeof(T);
  out.resize(count);
  if (count == 0) return true;
  f.read(reinterpret_cast<char*>(out.data()),
        static_cast<std::streamsize>(want_bytes));
  if (!f) {
    if (error) *error = path + ": short read";
    return false;
  }
  return true;
}

}  // namespace

bool write_river_graph(const std::string& path, const RiverGraph& g,
                       std::string* error) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot open " + path + " for writing";
    return false;
  }

  bool ok = write_pod(f, kMagic) && write_pod(f, kNodeFieldCount) &&
           write_pod(f, kEdgeScalarFieldCount) &&
           write_pod(f, kEdgeArrayCount);
  const uint64_t node_count = g.nodes.size();
  const uint64_t edge_count = g.edges.size();
  ok = ok && write_pod(f, node_count) && write_pod(f, edge_count);

  for (const RiverNode& n : g.nodes) {
    if (!ok) break;
    ok = write_pod(f, n.pos_m.x) && write_pod(f, n.pos_m.y) &&
        write_pod(f, n.ground_m) && write_pod(f, n.drainage_area_m2) &&
        write_pod(f, n.discharge_m3_s) && write_pod(f, n.width_m) &&
        write_pod(f, n.depth_m) && write_pod(f, n.speed_m_s) &&
        write_pod(f, n.lake_id) &&
        write_pod(f, static_cast<uint8_t>(n.lake_kind)) &&
        write_pod(f, static_cast<uint8_t>(n.kind));
  }

  for (const RiverEdge& e : g.edges) {
    if (!ok) break;
    ok = write_pod(f, e.from) && write_pod(f, e.to) &&
        write_pod(f, e.strahler_order) && write_pod(f, e.shreve_magnitude);
    const uint64_t point_count = e.points_m.size();
    ok = ok && write_pod(f, point_count);
    ok = ok && write_pod_vec(f, e.points_m) &&
        write_pod_vec(f, e.discharge_m3_s) && write_pod_vec(f, e.width_m) &&
        write_pod_vec(f, e.depth_m) && write_pod_vec(f, e.speed_m_s);
  }

  if (!ok) {
    if (error) *error = "short write on " + path;
    return false;
  }
  return true;
}

std::optional<RiverGraph> read_river_graph(const std::string& path,
                                           std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return std::nullopt;
  }
  const std::streamsize size_s = f.tellg();
  if (size_s < 0) {
    if (error) *error = "cannot stat " + path;
    return std::nullopt;
  }
  const uint64_t file_size = static_cast<uint64_t>(size_s);
  f.seekg(0);

  uint32_t magic = 0, node_fields = 0, edge_scalars = 0, edge_arrays = 0;
  uint64_t node_count = 0, edge_count = 0;
  if (!read_pod(f, magic) || magic != kMagic) {
    if (error) *error = path + ": not a river graph file (bad magic)";
    return std::nullopt;
  }
  if (!read_pod(f, node_fields) || !read_pod(f, edge_scalars) ||
      !read_pod(f, edge_arrays) || !read_pod(f, node_count) ||
      !read_pod(f, edge_count)) {
    if (error) *error = path + ": truncated header";
    return std::nullopt;
  }
  if (node_fields != kNodeFieldCount || edge_scalars != kEdgeScalarFieldCount ||
      edge_arrays != kEdgeArrayCount) {
    if (error) {
      std::ostringstream os;
      os << path << ": field-count mismatch (node " << node_fields << " vs "
         << kNodeFieldCount << ", edge scalars " << edge_scalars << " vs "
         << kEdgeScalarFieldCount << ", edge arrays " << edge_arrays << " vs "
         << kEdgeArrayCount
         << ") -- built against a different RiverGraph shape, or the file is"
            " not a river graph at all";
      *error = os.str();
    }
    return std::nullopt;
  }

  {
    const uint64_t pos = static_cast<uint64_t>(f.tellg());
    if (pos > file_size || node_count > (file_size - pos) / kNodeRecordBytes) {
      if (error) *error = path + ": truncated node block";
      return std::nullopt;
    }
  }

  RiverGraph g;
  g.nodes.resize(node_count);
  for (RiverNode& n : g.nodes) {
    uint8_t lake_kind = 0, kind = 0;
    if (!read_pod(f, n.pos_m.x) || !read_pod(f, n.pos_m.y) ||
        !read_pod(f, n.ground_m) || !read_pod(f, n.drainage_area_m2) ||
        !read_pod(f, n.discharge_m3_s) || !read_pod(f, n.width_m) ||
        !read_pod(f, n.depth_m) || !read_pod(f, n.speed_m_s) ||
        !read_pod(f, n.lake_id) || !read_pod(f, lake_kind) ||
        !read_pod(f, kind)) {
      if (error) *error = path + ": truncated while reading a node";
      return std::nullopt;
    }
    if (lake_kind > static_cast<uint8_t>(LakeKind::Emergent) ||
        kind > static_cast<uint8_t>(RiverNodeKind::FrameEntry)) {
      if (error) *error = path + ": node holds an out-of-range kind";
      return std::nullopt;
    }
    n.lake_kind = static_cast<LakeKind>(lake_kind);
    n.kind = static_cast<RiverNodeKind>(kind);
  }

  // Same guard the node block gets. An edge record is at least kEdgeHeaderBytes
  // (from, to, strahler, shreve, point_count), so a count exceeding what the
  // remaining bytes could possibly hold is corruption -- and resizing to it
  // first would throw length_error/bad_alloc straight out of this function,
  // which nothing catches. patch_io.hpp promises a malformed rivers.bin is an
  // ERROR, and load_patch reads it from a user-supplied --load directory.
  {
    const uint64_t pos = static_cast<uint64_t>(f.tellg());
    if (pos > file_size || edge_count > (file_size - pos) / kEdgeHeaderBytes) {
      if (error) *error = path + ": truncated edge block";
      return std::nullopt;
    }
  }

  g.edges.resize(edge_count);
  for (RiverEdge& e : g.edges) {
    uint64_t point_count = 0;
    if (!read_pod(f, e.from) || !read_pod(f, e.to) ||
        !read_pod(f, e.strahler_order) || !read_pod(f, e.shreve_magnitude) ||
        !read_pod(f, point_count)) {
      if (error) *error = path + ": truncated while reading an edge header";
      return std::nullopt;
    }
    if (!read_pod_vec(f, point_count, e.points_m, file_size, path, error) ||
        !read_pod_vec(f, point_count, e.discharge_m3_s, file_size, path,
                      error) ||
        !read_pod_vec(f, point_count, e.width_m, file_size, path, error) ||
        !read_pod_vec(f, point_count, e.depth_m, file_size, path, error) ||
        !read_pod_vec(f, point_count, e.speed_m_s, file_size, path, error)) {
      return std::nullopt;
    }
  }
  return g;
}

}  // namespace badlands::mapgen
