#pragma once

// Floating WORLD LABELS: text/bars drawn to the color buffer but anchored to 3D
// scene positions -- character & building names, health bars, and floating
// damage numbers. Two flavors share one screen-space projector:
//
//  * Persistent labels (names, health bars) are stateless: the caller rebuilds
//    them from the sim snapshot every frame, projects each anchor, and emits
//    quads. Nothing is stored here for those.
//  * Timed labels (damage numbers, and anything else with a lifetime) live in a
//    generic WorldLabelPool. The TIMER is the only state; visualization
//    (opacity, vertical rise) is DERIVED from it each frame by an animation --
//    nothing here is "damage"- or "opacity"-specific. Damage is just one
//    producer that spawns a rise+fade label; the pool would carry any timed
//    text the same way.
//
// This is pure CPU math over glm + the sim's entity-slot ids; it never touches
// the GPU or the `ui` crate. The caller turns ResolvedLabel world positions into
// screen quads via ProjectLabel + ui_text_run. Mirrors the decal-math test
// pattern (a CPU oracle exercised without a device).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace badlands {

// --- screen-space projection (3D awareness) --------------------------------

// One anchor projected to the screen. `visible` is false when the anchor is
// behind the camera or off-screen (the caller then draws nothing). `x`/`y` are
// PHYSICAL pixels (origin top-left); `depth` is the view-forward distance (the
// clip w), and `scale` is the dampened depth scale to apply to the label.
struct LabelProjection {
  bool visible = false;
  float x = 0.0f;
  float y = 0.0f;
  float depth = 0.0f;
  float scale = 1.0f;
};

// Projects `world` through `view_proj` (= camera.GetProj() * camera.GetView())
// into a physical-pixel screen point, culling anything behind the camera or
// outside the viewport (with a small margin so a label straddling the edge is
// kept). `scale` is LabelDepthScale(depth).
LabelProjection ProjectLabel(const glm::mat4& view_proj, const glm::vec3& world,
                             float viewport_w_px, float viewport_h_px);

// Fixed depth-scale constants (no runtime knobs). Public so the CPU-math tests
// can anchor to kRefDepth; the game never varies these.
namespace label_scale {
inline constexpr float kRefDepth = 60.0f;     // depth at which scale == 1.0
inline constexpr float kDampExponent = 0.5f;  // < 1 => dampened vs true 1/depth
inline constexpr float kMinScale = 0.55f;     // clamp: never smaller than this
inline constexpr float kMaxScale = 1.25f;     // clamp: never larger than this
}  // namespace label_scale

// DAMPENED depth scale: farther anchors get smaller labels, but slower than a
// true 1/depth perspective (an exponent < 1), clamped to a readable band. 1.0
// at the reference depth. Fixed constants -- no runtime knobs.
float LabelDepthScale(float depth);

// --- generic timed-label pool ----------------------------------------------

// How a timed label's visualization derives from its normalized life `t01`
// (1.0 at spawn -> 0.0 at expiry). More animations can be added; the pool and
// producers stay agnostic.
enum class LabelAnimation : int32_t {
  RiseFade = 0,  // rises a little and fades its alpha out over its life
};

// The derived visualization for one instant of a label's life.
struct LabelVisual {
  float opacity;  // 0..1 alpha multiplier
  float rise;     // extra world-up offset (units) above the anchor
};

// Samples an animation at normalized life `t01` (clamped to [0,1]).
LabelVisual SampleLabelAnimation(LabelAnimation anim, float t01);

// A timed label resolved for this frame: where to draw it and how opaque.
struct ResolvedLabel {
  glm::vec3 world_pos;  // anchor (live or frozen) + up*(base + rise + stacking)
  std::string text;
  uint32_t color;   // base 0xRRGGBBAA (alpha is the label's max; scale by opacity)
  float opacity;    // 0..1, from the animation
};

// Resolves a live anchor position for an entity slot, or nullopt if the entity
// is gone (the label then freezes at its spawn position -- so a lethal hit's
// number still floats where the victim died).
using AnchorLookup = std::function<std::optional<glm::vec3>(uint32_t slot)>;

// Owns the live timed labels. Producers Spawn; the view Advances by the real
// presentation dt each frame (independent of sim speed/pause) and Resolves for
// drawing. Labels on the same anchor STACK vertically and are removed when their
// timer reaches 0.
class WorldLabelPool {
 public:
  // Spawns a timed label anchored to `anchor_slot` at `anchor_pos` (the
  // entity's world position when it fired), starting `base_offset` world units
  // above it. `color` is the base 0xRRGGBBAA; `lifetime` seconds; `anim` drives
  // opacity/rise.
  void Spawn(uint32_t anchor_slot, const glm::vec3& anchor_pos, float base_offset,
             std::string text, uint32_t color, float lifetime, LabelAnimation anim);

  // Advances every label's timer by `dt` and drops the expired ones.
  void Advance(float dt);

  // Resolves every live label to its draw params. `live_pos` follows a moving
  // anchor that is still alive; a nullopt freezes the label at its spawn pos.
  std::vector<ResolvedLabel> Resolve(const AnchorLookup& live_pos) const;

  size_t size() const { return labels_.size(); }
  bool empty() const { return labels_.empty(); }

 private:
  struct TimedLabel {
    uint32_t anchor_slot;
    glm::vec3 anchor_pos;  // frozen anchor (entity pos at spawn)
    float base_offset;     // world-up offset to sit above the head
    std::string text;
    uint32_t color;
    float timer;     // seconds remaining
    float lifetime;  // seconds total (for t01)
    LabelAnimation anim;
  };
  std::vector<TimedLabel> labels_;  // spawn order (oldest first)
};

}  // namespace badlands
