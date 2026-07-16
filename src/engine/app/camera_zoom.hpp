#pragma once

// One zoom step, shared by the camera controllers.

#include <algorithm>
#include <cmath>

namespace badlands {

// Applies `notches` of zoom to a distance-like scalar (a camera height or an
// orbit distance), clamped to [min_value, max_value]. Positive notches shrink
// the value (move closer).
//
// Multiplicative, deliberately: a zoom step has to be proportional to where you
// already are, or one notch is imperceptible far out and slams into the subject
// up close. Two properties fall out of that which the additive/linear form
// `v *= (1 - notches * speed)` does NOT have:
//   - exactly reversible: zoom in N then out N returns the original value,
//     rather than drifting (x0.9 then x1.1 = 0.99);
//   - can never reach 0 or invert, whereas the linear form multiplies by zero
//     at exactly 1/speed notches and goes negative beyond it (a clamp hides the
//     negative but not the collapse).
//
// `speed` is octaves per notch: 1/speed notches halve the value.
inline float ZoomScalar(float value, float notches, float speed, float min_value,
                        float max_value) {
  return std::clamp(value * std::exp2(-notches * speed), min_value, max_value);
}

}  // namespace badlands
