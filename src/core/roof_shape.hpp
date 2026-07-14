#pragma once

// Shared between the engine's building-part assembler
// (engine/rendering/geometry/building_parts_builder.hpp — shape only, no
// materials) and the game's building visual catalog
// (game/building_catalog.h — kind -> shape + material mapping). Ported from
// the reference RoofStyle enum (src/game/catalog.rs @ 8ee93cc), minus its
// payload (the reference embeds a MaterialId in Gable/CornerTowers; that
// split — RoofShape here is shape-only, materials live in game data — keeps
// game::MaterialId out of src/core and src/engine).

namespace badlands {

enum class RoofShape { Gable, CornerTowers, None };

}  // namespace badlands
