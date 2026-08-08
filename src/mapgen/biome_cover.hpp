#pragma once

// THE ONE PLACE THE TWO CLASSIFICATION VOCABULARIES MEET.
//
// mapgen::Biome is the simulation's (walkability, movement cost, habitat,
// animal spawning); mapgen::Cover is the render path's (what grows here). They
// are kept apart deliberately -- see game/map/terrain_lattice.hpp -- but a map
// authored in one and rendered through the other has to cross once, and this is
// the crossing. Confining it to a header nothing else includes is what keeps it
// from spreading.
//
// ONE DIRECTION ONLY. Biome -> Cover is lossy and honest about it; Cover ->
// Biome is not defined and should not be, because a render classification
// cannot answer a movement-cost question.

#include "mapgen/biomes.hpp"
#include "mapgen/cover.hpp"

namespace badlands::mapgen {

// Hills and Plains both become Grass. That distinction is elevation-derived,
// which is exactly what the patch contract stopped carrying -- `height` already
// says it. Mountain becomes Bare because thin soil over bedrock is what put it
// in that class in the first place.
inline constexpr Cover CoverForBiome(Biome b) {
  switch (b) {
    case Biome::Lake:
      return Cover::Water;
    case Biome::Swamp:
      return Cover::Wetland;
    case Biome::Forest:
      return Cover::Tree;
    case Biome::Plains:
    case Biome::Hills:
      return Cover::Grass;
    case Biome::Mountain:
      return Cover::Bare;
  }
  return Cover::Unknown;
}

}  // namespace badlands::mapgen
