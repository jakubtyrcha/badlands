#include "executables/viewer/character_viewer_view.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "engine/animation/attachment_lines.hpp"
#include "engine/animation/skeleton_lines.hpp"
#include "engine/app/sdl_input_util.hpp"  // NormalizedWheelY
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"

namespace badlands {
namespace {

// Same mid-gray debug floor the mesh viewer uses, at a size that suits a
// human-scale rig rather than a tree.
constexpr glm::vec3 kFloorGray{0.5f};
constexpr float kFloorRoughness = 1.0f;
constexpr float kFloorSize = 8.0f;
constexpr float kFloorUvRepeatSpacing = 1.0f;

// Bone colour and thickness. Yellow on mid-gray reads at any orbit distance,
// and 2px keeps thin finger bones visible without fattening the torso.
constexpr glm::vec3 kBoneColor{1.0f, 0.85f, 0.1f};
constexpr float kBoneThickness = 2.0f;

// Margin on the framing sphere so limbs at full extension stay inside the view.
constexpr float kFrameMargin = 1.15f;

// Axis-aligned bounds of a posed skeleton's joint origins. Derived from the rig
// rather than hardcoded, so a different skeleton (or one authored at another
// scale) frames itself correctly.
struct JointBounds {
  glm::vec3 lo{0.0f};
  glm::vec3 hi{0.0f};

  glm::vec3 center() const { return 0.5f * (lo + hi); }
  // Framing radius: half the diagonal, with margin for limbs at extension.
  float radius() const {
    const float r = 0.5f * glm::length(hi - lo) * kFrameMargin;
    return r > 0.01f ? r : 1.0f;  // a degenerate rig still gets a camera
  }
};

// Case-insensitive substring match, with an empty filter matching everything.
// 0 A.D.'s names are all lower_snake, but a user typing "Swordsman" should not
// have to know that.
bool MatchesFilter(const std::string& name, const char* filter) {
  if (filter == nullptr || filter[0] == '\0') return true;
  const auto lower = [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  };
  std::string haystack = name;
  std::string needle = filter;
  std::transform(haystack.begin(), haystack.end(), haystack.begin(), lower);
  std::transform(needle.begin(), needle.end(), needle.begin(), lower);
  return haystack.find(needle) != std::string::npos;
}

JointBounds ComputeJointBounds(const Pose& pose) {
  const ozz::span<const ozz::math::Float4x4> models = pose.models();
  if (models.empty()) return JointBounds{};

  JointBounds bounds{glm::vec3(std::numeric_limits<float>::max()),
                     glm::vec3(std::numeric_limits<float>::lowest())};
  for (const ozz::math::Float4x4& m : models) {
    const glm::vec3 p(ToMat4(m)[3]);
    bounds.lo = glm::min(bounds.lo, p);
    bounds.hi = glm::max(bounds.hi, p);
  }
  return bounds;
}

}  // namespace

bool CharacterViewerView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;
  pipeline_gen_ = ctx.pipeline_gen;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("CharacterViewerView::Initialize: MaterialLibrary init failed");
    return false;
  }

  animation_ = AnimationSet::Load(manifest_path_);
  if (!animation_) {
    // AnimationSet::Load already logged the specific failure. Without a
    // skeleton this view has nothing to show, so refuse rather than open an
    // empty window that looks like a rendering bug.
    spdlog::error("CharacterViewerView::Initialize: no character assets at {}",
                  manifest_path_);
    return false;
  }

  if (!initial_clip_name_.empty()) {
    const int found = animation_->FindClip(initial_clip_name_);
    if (found < 0) {
      spdlog::warn(
          "CharacterViewerView: no clip named \"{}\"; showing \"{}\" instead",
          initial_clip_name_, animation_->clip_name(0));
    } else {
      clip_index_ = found;
    }
  }

  pose_.emplace(animation_->skeleton());
  sampler_.Reset(animation_->skeleton());

  // No volumetric fog around a debug rig -- same reasoning as the mesh viewer.
  scene_renderer_->MutableFogConfig().enabled = false;
  env_.sun_intensity = 2.0f;
  env_.sky_intensity = 0.5f;

  ApplyEnvironment();
  BuildScene();
  UpdateSkeleton();

  // Frame the rig as it actually poses, not as we guessed it would.
  const JointBounds bounds = ComputeJointBounds(*pose_);
  orbit_.FrameBounds(bounds.center(), bounds.radius());
  orbit_.UpdateCamera(camera_);

  // Where the rig sits relative to y=0 decides whether a character placed at a
  // terrain height stands on it or sinks into it, so report it rather than
  // leaving it to be discovered as a floating character later. NB these are
  // JOINT origins: ankle joints sit above the sole, so a small positive
  // minimum is expected, not a gap.
  spdlog::info(
      "CharacterViewerView: clip \"{}\" -- joint origins span y=[{:.3f}, {:.3f}]",
      animation_->clip_name(clip_index_), bounds.lo.y, bounds.hi.y);

  if (!matlib_.ok()) {
    spdlog::error("CharacterViewerView::Initialize: material load failed");
    return false;
  }
  return true;
}

void CharacterViewerView::ApplyEnvironment() {
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void CharacterViewerView::BuildScene() {
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  AddFloor(scene_, kFloorSize, matlib_.SolidColor(kFloorGray, kFloorRoughness),
           kFloorSize / kFloorUvRepeatSpacing);
}

void CharacterViewerView::UpdateSkeleton() {
  skeleton_lines_.Clear();
  if (!animation_ || !pose_) return;

  const AnimationClip& clip = animation_->clip(clip_index_);
  // Fixed time pins a deterministic frame for a headless screenshot; otherwise
  // the clip LOOPS. LoopRatioAt, not RatioAt: the clamping form would hold the
  // final frame after one cycle instead of starting the next.
  const float ratio = fixed_ratio_ ? std::clamp(*fixed_ratio_, 0.0f, 1.0f)
                                   : LoopRatioAt(clip, anim_seconds_);

  if (!sampler_.Sample(clip, ratio, *pose_)) return;
  if (!LocalToModel(animation_->skeleton(), *pose_)) return;

  // Turn the rig onto +Z, the same correction the in-world overlay applies. A
  // rig authored facing -Z would otherwise be inspected from behind, and every
  // pose would read reversed here while looking correct in the game.
  const glm::mat4 world = glm::rotate(glm::mat4(1.0f),
                                      animation_->yaw_offset_radians(),
                                      glm::vec3(0.0f, 1.0f, 0.0f));
  EmitSkeletonLines(animation_->skeleton(), *pose_, world,
                    skeleton_lines_, kBoneColor, kBoneThickness);
  // Into the SAME buffer: a frame has one debug-line buffer, and an emitter that
  // owned its own would silently erase the bones (src/engine/CLAUDE.md).
  // Always on -- that is also what keeps a --screenshot run deterministic.
  EmitAttachmentAxes(*animation_, *pose_, world, skeleton_lines_);
}

void CharacterViewerView::HandleEvent(const SDL_Event& event, int /*width*/,
                                      int /*height*/) {
  if (ImGui::GetIO().WantCaptureMouse) return;

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = true;
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button == SDL_BUTTON_LEFT) left_mouse_down_ = false;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (left_mouse_down_) {
        orbit_.HandleMouseDrag(event.motion.xrel, event.motion.yrel);
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      orbit_.HandleMouseWheel(NormalizedWheelY(event.wheel));
      break;
    default:
      break;
  }
}

void CharacterViewerView::Update(float dt, const bool* /*keyboard_state*/) {
  dt_ = dt;
  // Presentation time: this is a view, so the clip advances on anim time. A
  // fixed ratio freezes it entirely so a screenshot is reproducible.
  if (playing_ && !fixed_ratio_) {
    anim_seconds_ += dt * playback_scale_;
  }

  orbit_.UpdateCamera(camera_);
  scene_.SyncToRegistry(registry_, scene_context_);

  UpdateSkeleton();
  // Pointed at AFTER SyncToRegistry: the sync does not touch debug_lines, but
  // keeping the assignment last makes the per-frame ownership obvious.
  scene_context_.debug_lines =
      skeleton_lines_.empty() ? nullptr : &skeleton_lines_;
}

void CharacterViewerView::DrawUI() {
  if (!scene_renderer_ || !animation_) return;

  ImGui::SetNextWindowSize(ImVec2(340.0f, 620.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 300.0f),
                                      ImVec2(4096.0f, 4096.0f));
  ImGui::Begin("Character");

  if (!animation_->family().empty()) {
    ImGui::Text("%s", animation_->family().c_str());
  }
  ImGui::Text("%d joints, %d attachments", animation_->skeleton().num_joints(),
              animation_->attachment_count());

  // The clip list is the rig's whole vocabulary, and for an imported 0 A.D.
  // family that is 651 entries of `biped__infantry__swordsman__attack_...`.
  // Scrolling past hundreds of near-identical names is not browsing, hence the
  // filter -- the one control here, and the reason the list is usable at all.
  ImGui::SeparatorText("Clips");
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##clip_filter", "filter", clip_filter_,
                           sizeof(clip_filter_));

  int shown = 0;
  if (ImGui::BeginChild("clips", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders)) {
    for (int i = 0; i < animation_->clip_count(); ++i) {
      const std::string& name = animation_->clip_name(i);
      if (!MatchesFilter(name, clip_filter_)) continue;
      ++shown;
      if (ImGui::Selectable(name.c_str(), i == clip_index_)) {
        clip_index_ = i;
        anim_seconds_ = 0.0f;  // a new clip starts at its beginning
      }
    }
  }
  ImGui::EndChild();
  // Say how many of how many, so an empty list reads as "no match" rather than
  // as a rig that failed to load.
  if (clip_filter_[0] != '\0') {
    ImGui::Text("%d of %d", shown, animation_->clip_count());
  } else {
    ImGui::Text("%d", animation_->clip_count());
  }

  ImGui::Separator();
  // The selected clip stays selected through a filter that hides it, so
  // filtering never silently changes what is playing.
  ImGui::TextWrapped("%s", animation_->clip_name(clip_index_).c_str());
  const AnimationClip& clip = animation_->clip(clip_index_);
  ImGui::Text("%.2fs", clip.duration_seconds());
  ImGui::Checkbox("Play", &playing_);
  ImGui::SliderFloat("Speed", &playback_scale_, 0.0f, 2.0f, "%.2fx");

  // Scrub: writing the slider sets absolute time, which also pauses nothing --
  // dragging while playing simply reseeds where playback continues from.
  float ratio = fixed_ratio_ ? *fixed_ratio_
                             : LoopRatioAt(clip, anim_seconds_);
  if (ImGui::SliderFloat("Time", &ratio, 0.0f, 1.0f, "%.2f")) {
    anim_seconds_ = ratio * clip.duration_seconds();
    fixed_ratio_.reset();  // scrubbing releases a --anim-time pin
  }

  // What the RIG DECLARES, read-only. The viewer reads whatever names the asset
  // carries and shows them; it never interprets what a marker or an attachment
  // means -- that stays a decision for the layer above.
  if (const int markers = animation_->clip_marker_count(clip_index_); markers > 0) {
    ImGui::SeparatorText("Markers");
    for (int i = 0; i < markers; ++i) {
      ImGui::Text("%s  %.2f", animation_->clip_marker_name(clip_index_, i).c_str(),
                  animation_->clip_marker_value(clip_index_, i));
    }
  }

  ImGui::SeparatorText("Attachments");
  // Scrolls: a 0 A.D. biped declares ~74, which would otherwise push the clip
  // list off the panel entirely.
  if (ImGui::BeginChild("attachments", ImVec2(0.0f, 0.0f),
                        ImGuiChildFlags_Borders)) {
    for (int i = 0; i < animation_->attachment_count(); ++i) {
      ImGui::TextUnformatted(animation_->attachment_name(i).c_str());
    }
  }
  ImGui::EndChild();

  ImGui::End();

  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }
}

void CharacterViewerView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
