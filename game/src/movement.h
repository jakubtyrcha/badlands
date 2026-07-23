// Movement + collision pipeline (v0.3). Consumes the durable MoveTarget/NavPath
// components and the injected GamePathfinder contract (game_state.h). Path
// geometry is delegated to the provider; with none registered these systems
// fall back to straight-line paths and skip obstacle re-projection, so the
// headless sim/tests still run.

#pragma once

#include "mapgen/biomes.hpp"  // mapgen::Biome

struct BadlandsGame;

namespace badlands {

// Can a character stand here? A STAND-IN for real terrain navigation: the nav
// provider (src/crates/nav) is a visibility graph over building footprints and
// knows nothing about terrain, so until it does, this one predicate is the
// whole of "the world says no".
//
// It exists so the AI's half of the contract can be built and tested now: a
// character that is told to walk somewhere it cannot reach raises a MoveBlocked
// event, and the brain reacts by abandoning the goal. Replacing this with a
// real navmesh changes where the event comes FROM, and nothing about how any
// brain responds to it.
bool is_walkable(mapgen::Biome biome);

// Resolve each MoveTarget's goal and (re)plan its NavPath via the pathfinder,
// throttled by a repath cooldown and invalidated by nav_epoch / goal drift.
void plan_paths(BadlandsGame& game, float dt);

// Step each unit along its NavPath at move_speed*dt (skips MeleeLock'd units).
void follow_paths(BadlandsGame& game, float dt);

// Add/remove MeleeLock based on proximity to the nearest enemy (hysteresis).
void update_melee_locks(BadlandsGame& game);

// Soft disc push-apart via a per-tick spatial hash; locked units are immovable
// colliders. With a pathfinder present, re-project units out of footprints.
void separate_units(BadlandsGame& game);

}  // namespace badlands
