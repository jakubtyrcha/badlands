#include "foliage/forest_type.hpp"

namespace badlands::foliage {

float DepthCurve::Evaluate(float depth_m) const {
  if (depth_m <= rise_start) return 0.0f;
  if (depth_m >= fall_end) return 0.0f;

  float v = 1.0f;
  if (depth_m < rise_end) {
    // Degenerate ramp = an instantaneous step at rise_start. Guarding here
    // rather than dividing by zero keeps a "hard edge" curve expressible.
    const float span = rise_end - rise_start;
    v = span > 0.0f ? (depth_m - rise_start) / span : 1.0f;
  }
  if (depth_m > fall_start) {
    const float span = fall_end - fall_start;
    const float falloff = span > 0.0f ? (fall_end - depth_m) / span : 0.0f;
    v = v < falloff ? v : falloff;
  }
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

bool ForestType::Valid() const {
  if (models.empty() || layers.empty()) return false;
  for (const FoliageLayer& l : layers) {
    if (l.model_count == 0) return false;
    const size_t end = static_cast<size_t>(l.first_model) + l.model_count;
    if (end > models.size()) return false;
    if (l.grid_m <= 0.0f) return false;
  }
  return true;
}

}  // namespace badlands::foliage
