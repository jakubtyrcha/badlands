#pragma once

// The sim's ONLY source of "randomness", and the reason a run is reproducible.
//
// There is no global RNG state anywhere in badlands. Every varied value is a
// pure function of (who, when, what for) -- so two runs of the same inputs draw
// the same numbers, a replayed command log lands on the same decisions, and no
// system can accidentally couple itself to how many times another system
// happened to draw. Anything that wants variety seeds from an entity slot and a
// time quantum and takes what it gets.

#include <cstdint>

namespace badlands {

// xorshift64. The state must be non-zero; seed_of guarantees that.
inline uint64_t xorshift64(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

// Mixes an entity slot and a time (or epoch, or any second axis) into a
// non-zero seed. Knuth's multiplicative constant spreads adjacent slots apart
// so neighbouring entities do not draw correlated sequences.
inline uint64_t seed_of(uint32_t slot, int64_t when) {
    uint64_t s = (static_cast<uint64_t>(slot) * 2654435761ull) ^
                 (static_cast<uint64_t>(when) + 1ull);
    return s == 0 ? 1ull : s;
}

// Uniform in [0,1). Takes the high bits, which are the well-mixed ones.
inline float unit_float(uint64_t& s) {
    return static_cast<float>(xorshift64(s) >> 40) * (1.0f / 16777216.0f);
}

// Uniform integer in [lo, hi]. Returns lo when the range is empty or inverted.
inline int64_t range_i64(uint64_t& s, int64_t lo, int64_t hi) {
    if (hi <= lo) {
        return lo;
    }
    const uint64_t span = static_cast<uint64_t>(hi - lo) + 1ull;
    return lo + static_cast<int64_t>(xorshift64(s) % span);
}

}  // namespace badlands
