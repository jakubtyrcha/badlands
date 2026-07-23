// Hero recruitment, residency, class, entering/hiding, and destruction cascade
// (v0.3). All town game-logic lives here behind the dispatch_into handlers and
// is tested in C++ (heroes_tests.cpp), never through bespoke C API functions.

#pragma once

#include "badlands_sim.hpp"  // badlands::CharacterDesc

#include "mapgen/biomes.hpp"  // mapgen::Biome (pure enum, no engine deps)

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

struct InsideBuilding;  // components.h; used only by reference below

// HeroClassId (HERO_MERCENARY ...) is declared in badlands_sim.hpp -- it is the
// public "class type id" the recruit UI reads off BuildingDef::recruits.

// Guild kind -> class, or -1 for a non-guild building. Derived from the kind's
// BuildingDef::recruits (the recruit-set table is the single source of truth).
int32_t guild_hero_class(int kind);

// Shared baseline hero descriptor; color is the only class-distinguishing field.
CharacterDesc hero_desc(int32_t hero_class, float x, float z);

// --- world <-> map queries --------------------------------------------------
// MapData is indexed in MAP-LOCAL coordinates (origin at a corner); the sim
// works in world XZ centred on the origin. These convert once, so no caller
// re-derives the offset (the bug GameView's `world + half_extent` hand-rolling
// invites). An empty map reports Plains / height 0.
mapgen::Biome biome_at(const BadlandsGame& game, glm::vec2 world_xz);

// Razes a building through the full destruction cascade (expel/tombstone/nav/
// rehome) with NO policy check -- combat uses this to destroy any building.
// destroy_building_impl is the player-facing wrapper (user-destructible only).
int64_t raze_building(BadlandsGame& game, uint32_t building_id);
float height_at(const BadlandsGame& game, glm::vec2 world_xz);

// Spawn an entity with the full component set (incl. HeroCharacter/
// HeroSimulationState/HeroDisplayState).
// Shared by Sim::Spawn (home = -1) and recruit. Returns the public slot id.
uint32_t spawn_entity(BadlandsGame& game, const CharacterDesc& desc, int32_t home);

// Count of heroes whose Home is `building_id`.
uint32_t roster_count(const BadlandsGame& game, uint32_t building_id);

// Recruit at a guild: validates alive guild + roster room + a free approach tile,
// spawns a class-tinted hero on that tile with Home = building_id. UINT32_MAX on
// failure.
uint32_t recruit(BadlandsGame& game, uint32_t building_id);

// Destroy a user-destructible building: expel inside heroes, reassign residents
// to another same-class guild with room (else homeless), free the footprint and
// nav obstacle. Returns 0 on success, <0 on rejection (matches Sim::Dispatch).
int64_t destroy_building_impl(BadlandsGame& game, uint32_t building_id);

// Errand mechanics (invoked by the brain host calls in Phase 6; engine
// validates). Each returns whether the action applied.
bool hero_enter(BadlandsGame& game, entt::entity e, int kind);  // nearest-of-kind
bool hero_enter_home(BadlandsGame& game, entt::entity e);       // resting empties inventory
bool hero_buy(BadlandsGame& game, entt::entity e);              // fill inventory at an apothecary

// Should this hero stop being inside its building this tick?
//
// THE extension point for "what pulls a hero back out". Today the only answer
// is "the need it went in for is full" -- there is no duration anywhere, so how
// long a stay lasts falls out of how depleted the hero was and how fast the
// reserve refills (both HeroFactors, both live).
//
// Future reasons belong here and nowhere else: the home under attack, a threat
// outside the door, being summoned, dawn. It is system-side rather than a brain
// decision because a hidden hero is excluded from perception and movement, so
// it cannot see any of that for itself.
bool should_leave_building(const BadlandsGame& game, entt::entity e,
                           const InsideBuilding& inside);

// Tick sub-pass: evaluate should_leave_building for everyone inside; those that
// should reappear at the approach tile and drop the component.
void advance_inside(BadlandsGame& game);

// Tick sub-pass: run conversations and dissolve the ones that are over.
//
// A session is created ONLY by the Chat command, but it ends by SYSTEM RULE --
// expiry, the partner dying or being hidden, the pair drifting apart, or a
// threat arriving. That asymmetry is deliberate and mirrors MeleeLock: starting
// a conversation is a decision worth recording, ending one is a consequence of
// the world, and running it as a rule means a replay dissolves it identically
// without the log having to carry an event for it. Both sides always leave
// together, so a half-session cannot exist.
void advance_chats(BadlandsGame& game, float dt);

}  // namespace badlands
