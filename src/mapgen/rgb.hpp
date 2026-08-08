#pragma once

// The debug-palette colour, shared by every classification vocabulary here.
//
// Its own header so that biomes.hpp (gameplay) and cover.hpp (render) can each
// carry a palette without either including the other. They are deliberately
// independent -- see game/map/terrain_lattice.hpp -- and a shared struct is not
// a reason to couple them. It also means a palette can be handed to code that
// takes colours generically without a type pun.

#include <cstdint>

namespace badlands::mapgen {

struct Rgb {
  uint8_t r, g, b;
};

}  // namespace badlands::mapgen
