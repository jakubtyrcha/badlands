# Bit-exact mirror of game/src/behaviours/rng.h -- the sim's ONLY source of
# "randomness" (see that header's own comment for the "why": no global RNG
# state, every varied value is a pure function of (who, when, what for), so a
# replayed command log lands on the same decisions). This module must draw
# EXACTLY the same numbers the C++ side draws for the same (slot, when) --
# that is the whole of what the twin-brain parity test checks.
#
# uint64 arithmetic wraps on overflow in Nim exactly as it does in C++ (both
# are well-defined modulo-2^64 for unsigned types), so every operation below
# is a literal transcription: same operators, same order, same literals.

# xorshift64: advances `s` in place and returns the new value. `s` must be
# non-zero (seed_of guarantees that).
proc xorshift64*(s: var uint64): uint64 =
  s = s xor (s shl 13)
  s = s xor (s shr 7)
  s = s xor (s shl 17)
  result = s

# Mixes an entity slot and a time (or epoch) into a non-zero seed. The
# splitmix64 finalizer is load-bearing (see rng.h) -- avalanche first, then
# draw. `whenMillis` mirrors the C++ int64_t `when` parameter; the
# static_cast<uint64_t> reinterpretation of a (possibly negative) int64 is
# exactly what Nim's `uint64(x)` conversion between same-width signed/unsigned
# integers does too (two's-complement bit pattern, no range check).
proc seedOf*(slot: uint32, whenMillis: int64): uint64 =
  var s: uint64 = uint64(slot) * 0x9E3779B97F4A7C15'u64
  s = s xor (uint64(whenMillis) + 0x9E3779B97F4A7C15'u64)
  s = s xor (s shr 30)
  s = s * 0xBF58476D1CE4E5B9'u64
  s = s xor (s shr 27)
  s = s * 0x94D049BB133111EB'u64
  s = s xor (s shr 31)
  result = if s == 0'u64: 1'u64 else: s

# Uniform in [0,1). Takes the high bits (the well-mixed ones): the `shr 40`
# draw and the 1/16777216 scale are literal, per rng.h's own comment on why
# that matters.
proc unitFloat*(s: var uint64): float32 =
  result = float32(xorshift64(s) shr 40) * (1.0'f32 / 16777216.0'f32)

# Uniform integer in [lo, hi]. Returns lo when the range is empty or inverted.
proc rangeI64*(s: var uint64, lo, hi: int64): int64 =
  if hi <= lo:
    return lo
  let span: uint64 = uint64(hi - lo) + 1'u64
  result = lo + int64(xorshift64(s) mod span)
