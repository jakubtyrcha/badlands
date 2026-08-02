#pragma once

// River graph algebra: dropping reaches that fall short of a width or length
// threshold. Pure graph transforms over an already-extracted RiverGraph -- no
// I/O, no raster, no routing.

#include "mapgen/river_network.hpp"

namespace badlands::mapgen {

// Drops every reach that never reaches `min_width_m`, and trims the headwater
// end of the survivors back to the point where they do — so a kept river starts
// where it BECOMES one rather than at a hairline that happens to grow.
//
// Width is monotone downstream (w = k_w * sqrt(Q), and Q only accumulates), so a
// reach's maximum is its downstream end: testing that tests the whole reach, and
// the trim is a single cut rather than a filter.
//
// Node kinds are recomputed against the surviving topology. A confluence whose
// tributaries were all pruned IS a source afterwards, and leaving it labelled
// Confluence would misreport where the water enters — which is exactly what the
// source list is for. Lake and mouth kinds describe a boundary rather than an
// upstream count, so they are left alone.
void prune_river_graph_by_width(RiverGraph& g, float min_width_m);

// Drops stubby headwater BRANCHES -- the whole chain from a headwater down to
// the first confluence, when that chain is shorter than `min_length_m`.
//
// A branch, not a reach. Clipping splits a reach at the frame and gives each
// fragment its own start node, so a per-reach test saw every fragment as a
// headwater and ate a 700 m trunk one fragment at a time (peak Q fell
// 0.7183 -> 0.0218 m3/s). Accumulating along the chain is immune to how a reach
// happens to be subdivided.
//
// Only headwater chains, because removing an interior reach would cut the
// network in two and strand everything above it. Applied REPEATEDLY, since
// removing a branch can expose the next one; it converges because every round
// strictly shrinks the edge set.
//
// A chain rooted at a FrameEntry node is never a headwater, even though it
// also has in_deg == 0: its upstream continues beyond the window. Treating it
// as one let a trunk that merely clips through a corner of the frame get eaten
// as if its whole in-frame stretch were a stubby headwater.
//
// This is a different filter from the width one and neither implies the other: a
// wide reach can be stubby (a lake inlet metres from the shore) and a hairline
// can run for a kilometre. Width says "is this a river", length says "is this
// worth drawing".
void prune_river_graph_by_length(RiverGraph& g, float min_length_m);

}  // namespace badlands::mapgen
