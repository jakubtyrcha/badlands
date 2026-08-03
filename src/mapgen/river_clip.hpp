#pragma once

// River graph algebra: clipping a graph to an axis-aligned rect, minting a
// node exactly where each reach crosses the boundary.
//
// Needed because routing happens on a GHOST-PADDED grid: reaches legitimately
// run one cell past the frame, and a reach that leaves must end at a real place
// rather than dangle outside the map or stop at whichever sample happened to be
// last inside -- which would move every time the resolution changed.

#include <glm/glm.hpp>

#include "mapgen/river_network.hpp"

namespace badlands::mapgen {

// Clips every reach to the axis-aligned rect [lo_m, hi_m), minting a node
// exactly where the chain crosses the rect with all of its properties
// (discharge, width, depth, speed) interpolated along the crossing segment.
//
// The node minted where a chain ENTERS the rect is RiverNodeKind::FrameEntry --
// the inbound twin of Mouth -- never Source: a trunk that merely crosses into
// the rect is not a headwater, and mislabelling it as one let the length prune
// (river_prune.hpp) treat it as one and eat a 700 m trunk fragment by fragment.
void clip_river_graph_to_rect(RiverGraph& g, glm::vec2 lo_m, glm::vec2 hi_m);

}  // namespace badlands::mapgen
