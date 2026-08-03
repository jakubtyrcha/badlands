// The animation projection: one pass that summarises what every character is
// DOING into a CharacterAnim (badlands_sim.hpp), for the render layer to map to
// clips.
//
// This is the sim's only concession to animation, and it is deliberately a
// PROJECTION rather than a model: every mechanic keeps owning its own state
// (StrikeInProgress owns a swing, SkillFocus owns a cast, Statuses owns a stun)
// and this pass only translates them into one value. Adding a mechanic that
// should look different means one case here, not a new writer scattered into
// the mechanic itself.
//
// It runs LAST in step_world, and that placement is load-bearing -- see the
// note on moved_by_path_scratch in the .cpp.
//
// Nothing under game/ reads CharacterAnim back. It is write-only from the sim's
// side, which is what keeps the renderer's use of the shared registry from
// affecting determinism.

#pragma once

struct BadlandsGame;

namespace badlands {

// Writes (emplacing where absent) a CharacterAnim on every live character slot.
void project_anim_state(BadlandsGame& game);

}  // namespace badlands
