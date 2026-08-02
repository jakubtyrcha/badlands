#pragma once

// Binary serialization of a RiverGraph, alongside the coarse artifact
// (world.txt lives next to it). Text would be ~35 MB at 16 km, so this is a
// little-endian POD dump: a magic plus a small field-count sanity, so a
// truncated file or one built against a different RiverNode/RiverEdge shape
// is an ERROR, never a silent misread -- same spirit as patch_io's
// read_raster size check, adapted to a variable-length (per-edge point
// count) record.

#include <optional>
#include <string>

#include "mapgen/river_network.hpp"

namespace badlands::mapgen {

// Writes `g` to `path`. Round-trips exactly through read_river_graph.
bool write_river_graph(const std::string& path, const RiverGraph& g,
                       std::string* error = nullptr);

// Reads a graph written by write_river_graph. Returns nullopt with a reason
// in `error` on a missing file, a bad magic, a field-count mismatch, or a
// file truncated anywhere in the node or edge blocks.
std::optional<RiverGraph> read_river_graph(const std::string& path,
                                           std::string* error = nullptr);

}  // namespace badlands::mapgen
