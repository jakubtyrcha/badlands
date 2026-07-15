#pragma once

// Shared, game-agnostic ImGui debug/editor helpers (Task S2.B4). Free
// functions (not a stateful class — nothing here needs to persist state
// across frames beyond what LightEnvironment/SceneRenderer already own), so
// every AppView (badlands_viewer/game/ai_sandbox) can reuse the same
// LightEnvironment editor + G-buffer debug selector + FPS readout without
// duplicating ImGui code. Knows LightEnvironment + SceneRenderer, nothing
// game-specific — lives in src/engine/ per the rendering/engine layer
// boundary (see CLAUDE.md).
//
// Adapted (not verbatim-ported) from sampo's src/ui/editor_ui.{hpp,cpp}:
// sampo's EditorUI is a stateful class coupled to its own renderer/
// EditorState/weather-sim/planet-map types (DrawPerformanceWindow/
// DrawRenderingDebugWindow/DrawPlanetWindow/DrawSimulationWindow, an
// orientation gizmo, docking setup) — none of which exist here. This keeps
// only the two patterns this task needs: the G-buffer-debug radio-button
// group (DrawGBufferDebugSelector) and the perf/FPS line (DrawStats).

namespace badlands {

struct LightEnvironment;
class SceneRenderer;

namespace EditorUI {

// Sliders for sun direction (azimuth/elevation), sun color + intensity, sky/
// horizon/ground colors, sky intensity, and sun disc size. Mutates `env` in
// place. Returns true if any value changed this frame — callers should
// re-run ApplyLightEnvironment(env, ...) when it does (rebuilds the sky cube
// + SH ambient + sun, see light_environment.hpp).
bool DrawLightEnvironmentEditor(LightEnvironment& env);

// Radio-button group selecting `renderer`'s GBufferDebugMode (None/Depth/
// Normals/Albedo/Roughness/Hdr). Reads the current mode from
// `renderer.GetDebugMode()` and calls `renderer.SetDebugMode(...)` on
// selection.
void DrawGBufferDebugSelector(SceneRenderer& renderer);

// Radio-button group selecting `renderer`'s ShadowDebugMode (Off/Combined/
// ShadowMapOnly/ContactOnly, Task T3). Reads the current mode from
// `renderer.GetShadowDebugMode()` and calls
// `renderer.SetShadowDebugMode(...)` on selection.
void DrawShadowDebugSelector(SceneRenderer& renderer);

// FPS + frame-time text line (e.g. "60.0 FPS (16.67 ms)").
void DrawStats(float dt_seconds);

// Convenience: draws all three inside one ImGui::Begin("Debug")/End().
// Returns DrawLightEnvironmentEditor's result (true if the light environment
// changed this frame).
bool DrawDebugPanel(LightEnvironment& env, SceneRenderer& renderer,
                    float dt_seconds);

}  // namespace EditorUI

}  // namespace badlands
