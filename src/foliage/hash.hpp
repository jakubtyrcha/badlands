#pragma once

// The generator's entire source of randomness.
//
// Integer hashing, not an RNG object, and that is the whole point: placement
// must be a pure function of (seed, layer, grid cell). No global state, no
// order dependence between grid cells, no engine RNG to keep in sync. The same
// seed reproduces the same forest byte for byte, on any machine, because every
// operation below is exact integer arithmetic.
//
// Header-only: it is a handful of multiplies, and the tests want to call it
// directly.

#include <cstdint>

namespace badlands::foliage {

// Chris Wellons' `triple32` -- a bijective 32-bit integer hash with (measured)
// near-perfect avalanche, i.e. flipping any input bit flips each output bit
// with probability ~0.5. That property is what lets one hash serve as BOTH the
// per-cell seed and, via the Weyl step below, a decorrelated stream: a cheaper
// mixer leaks structure into the jitter and the lattice starts showing.
inline constexpr uint32_t Triple32(uint32_t x) {
  x ^= x >> 17;
  x *= 0xed5ad4bbu;
  x ^= x >> 11;
  x *= 0xac4c1b51u;
  x ^= x >> 15;
  x *= 0x31848babu;
  x ^= x >> 14;
  return x;
}

// Seed for one (layer, grid cell). Coordinates are signed and may be negative
// (a field whose origin is not at the world origin); the conversion to uint32
// is two's-complement and well-defined, so negative cells hash as cleanly as
// positive ones.
//
// The inputs are folded through Triple32 one at a time rather than being packed
// into one word: packing would cap each field's range, and a 512 m map at 1 m
// grid already needs more than 8 bits of cell coordinate.
inline constexpr uint32_t FoliageHash(uint32_t seed, uint32_t layer, int32_t gx,
                                      int32_t gz) {
  uint32_t h = Triple32(seed);
  h = Triple32(h ^ (layer * 0x9e3779b9u));
  h = Triple32(h ^ static_cast<uint32_t>(gx));
  h = Triple32(h ^ static_cast<uint32_t>(gz));
  return h;
}

// A decorrelated stream of draws from one seed. Successive values come from a
// Weyl sequence (add the golden-ratio constant) pushed through Triple32, so
// consecutive draws are independent -- which matters because a single grid
// cell pulls its jitter, its density roll, its model pick, its yaw and its
// scale from the same stream, and any correlation between those shows up as
// visible structure (e.g. tall trees always facing one way).
class HashStream {
 public:
  explicit constexpr HashStream(uint32_t seed) : state_(seed) {}

  constexpr uint32_t NextUint() {
    state_ += 0x9e3779b9u;
    return Triple32(state_);
  }

  // Uniform on [0, 1). Takes the TOP 24 bits: the low bits of a multiplicative
  // mixer are the weakest, and 24 bits is exactly float's mantissa, so this is
  // both the best-quality and the lossless choice.
  constexpr float Next01() {
    return static_cast<float>(NextUint() >> 8) * (1.0f / 16777216.0f);
  }

  constexpr float NextRange(float lo, float hi) {
    return lo + (hi - lo) * Next01();
  }

 private:
  uint32_t state_;
};

}  // namespace badlands::foliage
