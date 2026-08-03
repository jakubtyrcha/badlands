#pragma once

// Stage 3's ONLY view of where a map comes from.
//
// This interface is the whole decoupling story. Map-detailing -- the cluster-LOD
// terrain, the biome materials, the foliage, the water surfaces -- must run
// against a patch cut from a simulated world, a patch read off disk, or a patch
// invented analytically by a test, with no code change. So it does not take a
// directory, does not take simulation parameters, and does not know that a
// coarse world exists.
//
// THE IMPLEMENTATION BEHIND THIS IS FREE. A source may be stateful, may cache,
// may stream, may hold a coarse world open across calls, may do none of that.
// None of it crosses the line. That freedom is the point: it is what lets stage
// 2 be rewritten -- or replaced entirely -- without stage 3 noticing.
//
// The test that this boundary is right: stage 3 renders a SyntheticPatchSource
// patch and a FilePatchSource patch through the identical path. If it ever has
// to know which one it got, the interface is wrong.
//
// Fetch is `const` and must be safe to call more than once with the same
// request. It is NOT required to be cheap -- a coarse-world source resamples
// rasters -- but it is required to be deterministic: the same request against
// the same source yields the same patch, or the resolution- and
// density-independence guarantees mean nothing.

#include "mapgen/patch_data.hpp"

namespace badlands::mapgen {

struct PatchSource {
  virtual ~PatchSource() = default;

  // Produce the patch the request describes. A source that cannot honour a
  // request -- a region outside its world, a degenerate resolution -- returns a
  // default-constructed PatchData, which `empty()` below detects. There is no
  // error channel here on purpose: a provider that needs to explain itself
  // (a missing file, a contradictory manifest) reports at CONSTRUCTION, where
  // the caller still has somewhere useful to put the message.
  virtual PatchData Fetch(const PatchRequest& req) const = 0;
};

// Derived over the interface rather than added as a virtual, so every source
// gets the same answer instead of each inventing one.
inline bool empty(const PatchData& p) {
  return p.height.width <= 0 || p.height.height <= 0;
}

}  // namespace badlands::mapgen
