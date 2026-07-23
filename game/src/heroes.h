// Hero recruitment, residency, class, entering/hiding, and destruction cascade
// (v0.3). All town game-logic lives here behind the game_dispatch handlers and
// is tested in C++ (heroes_tests.cpp), never through bespoke C API functions.

#pragma once

#include "badlands_sim.hpp"  // badlands::CharacterDesc

#include <entt/entt.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// HeroClassId (HERO_MERCENARY ...) is declared in badlands_sim.hpp -- it is the
// public "class type id" the recruit UI reads off BuildingDef::recruits.

// Guild kind -> class, or -1 for a non-guild building. Derived from the kind's
// BuildingDef::recruits (the recruit-set table is the single source of truth).
int32_t guild_hero_class(int kind);

// Shared baseline hero descriptor; color is the only class-distinguishing field.
CharacterDesc hero_desc(int32_t hero_class, float x, float z);

// Spawn an entity with the full component set (incl. HeroClass/Home/Inventory).
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

// Tick sub-pass: decrement InsideBuilding timers; on expiry reappear at the
// approach tile and drop the component.
void advance_inside_timers(BadlandsGame& game, float dt);

}  // namespace badlands
