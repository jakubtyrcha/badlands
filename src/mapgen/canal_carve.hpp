#pragma once

// Agent-based canal pre-carve: cuts a drainage skeleton across the plains
// BEFORE the erosion sim runs, so the hydrology has real gradients to follow
// instead of having to invent them.
//
// Why this exists: the plains carry kPlainsReliefM (2 m) of relief across the
// whole map and contain no incised drainage. Three separate symptoms come from
// that one gap — a third to a half of channel texels routed by flood order
// rather than gradient, lakes filling to their rims with no outlet, and an
// outlet-notch pass with nowhere to drain to. Earlier fixes each treated a
// symptom; this gives the plains the skeleton they lack.
//
// See docs/superpowers/specs/2026-07-29-mapgen-canal-precarve-design.md.
//
// Pure function of its inputs apart from the B it mutates — no I/O.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/erosion.hpp"
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// --- source ownership -------------------------------------------------------

// Disjoint-set union over canal source ids, with merge aliasing.
//
// Trail cells are NEVER rewritten when their network merges — that would be
// O(n) per union — so every cell keeps the id it was laid with and that id
// goes STALE the moment its network joins another. Consequently every
// same-source question must resolve through find(), and a raw
// `cell.source == agent.source` comparison is wrong in a way that passes
// almost every test: it only misbehaves on cells laid BEFORE a merge, which
// are most of the network. Those read as a different source, so the combined
// flow is attracted back into its own trunk and braids.
//
// Union by SMALLER ROOT (not by rank or size) so the resulting root does not
// depend on the order unions are applied.
class SourceSets {
 public:
  int32_t add();                       // new singleton; returns its id
  int32_t find(int32_t id);            // representative, path-compressed
  void merge(int32_t a, int32_t b);    // union by smaller root
  bool same(int32_t a, int32_t b) { return find(a) == find(b); }
  size_t size() const { return parent_.size(); }

 private:
  std::vector<int32_t> parent_;
};

// --- output -----------------------------------------------------------------

// Why an agent stopped. A histogram of these is the main tuning diagnostic;
// any StepCap at all is a bug, since self-avoidance already makes
// non-termination impossible over a finite grid. TrunkEnd is separate from BoxedIn on purpose: a merged
// agent that follows its trunk to the trunk's terminus is behaving correctly,
// and folding it into BoxedIn hides how often agents genuinely strand. On seed
// 2, 18 of 27 "box-ins" turned out to be this.
enum class CanalEnd : uint8_t { LeftMap, Lake, BoxedIn, TrunkEnd, StepCap };
inline constexpr int kCanalEndCount = 5;

struct CanalStats {
  int agents = 0;
  int merges = 0;
  int merges_same_root = 0;  // must be 0 — that would be a braid
  int ends[kCanalEndCount] = {0, 0, 0, 0, 0};
  int climb_fallbacks = 0;  // steps taken onto ground above max_climb_m
  float max_carve_m = 0.0f;   // tune against the LONGEST canal, not the mean
  float total_excavated_m = 0.0f;
  int longest_path = 0;
  // Committed steps that left the destination ABOVE its predecessor, measured
  // immediately after the carve. Must be 0 — that is the descent guarantee, at
  // the moment it is supposed to hold.
  //
  // Counted here rather than by walking the finished network on purpose. The
  // guarantee is per-agent at carve time and the height field is SHARED: a
  // later agent crossing a cell with a lower ref deepens it, which can invert
  // the relationship inside an EARLIER agent's channel. A post-hoc walk
  // therefore finds genuine uphill steps that were never carved as such — a
  // limitation of the design, not a violation of the rule.
  int uphill_carve_steps = 0;
};

struct CanalResult {
  Field2D<float> trail_discharge_m3_s;  // 0 off-canal
  Field2D<int32_t> trail_source;        // -1 off-canal; STALE ids, resolve via sets
  // Compass index (0..7) of the direction flow LEAVES each trail cell by —
  // the outgoing direction, not the arrival one. Meaningless where
  // trail_source < 0. Exposed so the network can be walked: that is how the
  // spatial descent guarantee is checked, rather than inferred from the rule.
  Field2D<uint8_t> trail_dir;
  SourceSets sets;
  CanalStats stats;
};

// The compass table trail_dir indexes into, in circular order from +x.
inline constexpr int kCanalDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
inline constexpr int kCanalDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
// trail_dir value meaning "flow does not continue past this cell" — the last
// cell of a trail, which left the map or was absorbed by a lake.
inline constexpr uint8_t kCanalNoDir = 0xFF;

// Cuts canals into `B`. `lake_mask` marks seeded basins (absorb + attract);
// `dist_to_plains` supplies the highland edge that seeds agents.
CanalResult carve_canals(Field2D<float>& B, const Field2D<uint8_t>& lake_mask,
                         const Field2D<float>& dist_to_plains,
                         const ErosionParams& p, float texel_m, uint32_t seed);

}  // namespace badlands::mapgen
