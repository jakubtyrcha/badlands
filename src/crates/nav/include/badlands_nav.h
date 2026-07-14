// C ABI for the `nav` Rust crate (src/crates/nav): a 2D navmesh-style path
// service. Linked into the badlands C++ app via Corrosion.
//
// The C++ sim owns movement/ECS and delegates *path geometry* to this module
// through the GamePathfinder vtable below: obstacles mutate one building at a
// time (add_obstacle/remove_obstacle) so the visibility graph is maintained
// incrementally rather than rebuilt from scratch on each change; find_path
// writes up to `cap` XZ waypoint pairs and returns the total waypoint count (a
// value > cap means truncated, matching the game_state snapshot idiom); 0
// means no path. `exempt_building` (UINT32_MAX for none) is a building whose
// clearance is ignored, so a unit can path to the very building it is
// entering.
//
// Stage 1: this crate is linked but has no C++ caller yet (nav is game-layer
// pathfinding, unused until Stage 2's game layer wires it up via
// game_set_pathfinder's GamePathfinder, declared in game/include/badlands_game.h).
#ifndef BADLANDS_NAV_H
#define BADLANDS_NAV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors game/include/badlands_game.h's GamePathfinder exactly; duplicated
// here so this crate's header is self-contained.
typedef struct GamePathfinder {
    void* ctx;
    void (*add_obstacle)(void* ctx, uint32_t building_id, const float* poly_xz, int32_t n_verts);
    void (*remove_obstacle)(void* ctx, uint32_t building_id);
    int32_t (*find_path)(void* ctx, float sx, float sz, float gx, float gz, float radius,
                         uint32_t exempt_building, float* out_xz, int32_t cap);
} GamePathfinder;

// Construct a new NavContext, to be used as GamePathfinder.ctx. Never NULL in
// practice (aborts the process only if the allocator itself fails). Free with
// nav_context_free.
void* nav_context_new(void);

// Free a NavContext previously returned by nav_context_new. Safe to call with
// NULL.
void nav_context_free(void* ctx);

// GamePathfinder.add_obstacle: register/replace building `building_id`'s
// footprint (a closed polygon, `n_verts` XZ vertex pairs in `poly_xz`,
// n_verts >= 3) as a path obstacle. Every live clearance-radius graph is
// updated incrementally. No-op if ctx/poly_xz are NULL or n_verts < 3.
void nav_add_obstacle(void* ctx, uint32_t building_id, const float* poly_xz, int32_t n_verts);

// GamePathfinder.remove_obstacle: drop building `building_id` as a path
// obstacle from every live clearance-radius graph. No-op if ctx is NULL or
// the building was never added.
void nav_remove_obstacle(void* ctx, uint32_t building_id);

// GamePathfinder.find_path: shortest clearance-respecting path from
// (sx,sz) to (gx,gz) for an agent of the given `radius`, optionally exempting
// `exempt_building` (UINT32_MAX for none) from clearance. Writes up to `cap`
// XZ waypoint pairs to `out_xz` and returns the total waypoint count (0 means
// no path; a return > cap means truncated — call again with a bigger buffer,
// per the game_state snapshot idiom). Returns 0 if ctx is NULL.
int32_t nav_find_path(void* ctx, float sx, float sz, float gx, float gz, float radius,
                      uint32_t exempt_building, float* out_xz, int32_t cap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BADLANDS_NAV_H
