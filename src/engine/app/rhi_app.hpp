#pragma once

// The app layer for the RHI era: what SdlViewerApp is to Dawn.
//
// SdlViewerApp cannot serve, and not for want of trying -- it is Dawn all the
// way through, handing views a wgpu::Device via RenderContext. So this is a
// second app layer by necessity, built on rhi_app_shell (which keeps the window
// and the loop) rather than beside it.
//
// WHAT IT OWNS, so an app does not: SDL init, device creation, the Slang
// compiler, the window and swapchain, ImGui in full, argument parsing, frame
// timing, screenshots, and shutdown in the right order. object_viewer's
// RunWindowed was ~350 lines of exactly that, and rhi_lab duplicated most of
// it.
//
// WHAT IT DOES NOT OWN: the scene. There is no scene target here and no
// tonemap -- an app renders whatever it likes into the backbuffer, and the
// layer composites debug UI on top of what it presented. The one exception is
// ImGui's own overlay, which the layer must own because it cannot be correct
// otherwise (see rhi_app.cpp).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "engine/app/fixed_timestep.hpp"
#include "engine/app/rhi_app_shell.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

namespace badlands::rhi_app {

// The back-channel a view needs for things only the app can do.
//
// FOUND BY PORTING THE SECOND APP. rhi_lab's windowed self-test drives a
// scripted resize through the window manager -- the same path a user drag takes,
// so the test exercises the coalescing rather than bypassing it -- and it reads
// the window in POINTS to compute the request. A view with no way to reach
// either could not be written at all, which the first port never revealed
// because object_viewer never asks the window for anything.
//
// NARROW ON PURPOSE: three methods, not the whole AppShell. A view calling
// Run() again, or reaching the swapchain behind the layer's back, are not
// things this seam should make expressible.
class RhiAppHost {
 public:
  virtual ~RhiAppHost() = default;

  // Asks the window manager for a new size, in POINTS (what SDL takes). The new
  // PIXEL size arrives as an event and goes through the same coalescing a user
  // drag uses -- which is the part worth testing.
  virtual void RequestResizePoints(uint32_t width, uint32_t height) = 0;

  // Ends the loop after the current frame.
  virtual void Stop() = 0;

  // What the SWAPCHAIN is currently sized for, in pixels. The sizes, not the
  // swapchain: an app asserting that its targets, the window and the swapchain
  // all agree needs these three numbers, and handing out the object so it can
  // read two of them would let it acquire and present behind the loop's back.
  virtual uint32_t SwapchainWidth() const = 0;
  virtual uint32_t SwapchainHeight() const = 0;

  // The window, for the POINT-space queries SDL answers and the shell does not.
  // Everything the shell reports is in PIXELS, and on a HiDPI display those are
  // different numbers -- comparing one against the other is a test that fails on
  // a correct implementation.
  virtual SDL_Window* Window() = 0;
};

// What a view is built against. Everything here exists only after the app has
// started, which is why views arrive through a factory.
struct RhiAppContext {
  RhiAppHost* host = nullptr;
  rhi::IRhiDevice* device = nullptr;
  slang::SlangCompiler* compiler = nullptr;
  // READ BACK from the swapchain, never assumed: the shell may present
  // RGBA16Float on an HDR display, and may drop off it again if the layer
  // refuses to tag. A pipeline built against a guess is a pipeline built for an
  // attachment that does not exist.
  rhi::Format surface_format = rhi::Format::Undefined;
  rhi::ColorSpace surface_color_space = rhi::ColorSpace::Srgb;
  uint32_t width = 0;   // PIXELS
  uint32_t height = 0;  // PIXELS
};

// How far time has moved, and what drove it.
//
// THE LAYER DEALS ONLY IN REAL AND PRESENTATION TIME. It never produces a tick:
// `fixed_steps` is a COUNT of elapsed fixed steps and the view maps those onto
// its own clock. Producing ticks here would put the simulation's time base in
// the app shell, which the four-time-bases contract forbids.
struct FrameTime {
  float real_dt = 0.0f;         // wall seconds, or exactly the fixed step
  double elapsed = 0.0;         // since the first frame
  uint32_t fixed_steps = 0;     // whole fixed steps elapsed this frame
  uint64_t index = 0;           // frames begun, from 0
  // SDL_GetKeyboardState, valid for this call only.
  //
  // NULL WHENEVER THE DEBUG UI OWNS THE KEYBOARD. The layer gates this the same
  // way it gates events, because a filter on events alone does not cover POLLED
  // state -- and typing into an ImGui field used to fly the camera one letter
  // at a time. A view must handle null rather than assume it.
  const bool* keys = nullptr;
};

// Turns a stream of real frame deltas into FrameTime.
//
// EXTRACTED SO IT CAN BE TESTED. This was six lines inside a lambda in
// RunParsed, which meant the only way to check that --fixed-dt is exact, that
// the step count is right, or that a stall cannot detonate the accumulator was
// to open a window and look. None of those need a GPU.
struct FrameClock {
  // 0 means wall clock; anything else replaces the measured dt entirely.
  float fixed_dt = 0.0f;
  double elapsed = 0.0;
  double accumulator = 0.0;

  // `real_dt` is what the loop measured; the return is what the view is told.
  FrameTime Advance(float real_dt, uint64_t index, const bool* keys) {
    const float dt = fixed_dt > 0.0f ? fixed_dt : real_dt;
    elapsed += double(dt);
    accumulator += double(dt);
    uint32_t steps = 0;
    // CAPPED. A stall -- a breakpoint, a slow first frame -- otherwise asks for
    // hundreds of steps at once, and an app that simulates each of them spends
    // longer than the stall did, which is the spiral kMaxSimTicksPerFrame
    // exists to stop. The remainder is DROPPED rather than carried: catching up
    // on time nobody was watching is worse than losing it.
    while (accumulator >= kTickDt && steps < uint32_t(kMaxSimTicksPerFrame)) {
      accumulator -= kTickDt;
      ++steps;
    }
    // DROPPED ONLY WHEN THE CAP IS WHAT STOPPED THE LOOP. `steps == kMax` on
    // its own cannot tell a clamped stall from a frame that happened to land
    // exactly on the cap with ordinary carried time left over -- and zeroing
    // the latter throws away sub-tick time the accumulator exists to keep.
    // A remainder still worth a whole tick is the backlog; anything less is
    // the fraction every frame leaves behind.
    if (steps >= uint32_t(kMaxSimTicksPerFrame) && accumulator >= kTickDt) {
      accumulator = 0.0;
    }
    return {.real_dt = dt,
            .elapsed = elapsed,
            .fixed_steps = steps,
            .index = index,
            .keys = keys};
  }
};

// One application view. The RHI-era counterpart of AppView.
//
// Deliberately carries NO entt::registry, no SceneContext and no Dawn type.
// AppView carries all three; the registry belongs to the simulation and
// SceneContext is the Dawn renderer's. A view that needs a registry owns one.
class RhiAppView {
 public:
  virtual ~RhiAppView() = default;

  // Build pipelines and targets here. Returns false, after logging, to abort
  // the run -- rendering an empty scene instead would be a silent failure.
  virtual bool Initialize(const RhiAppContext& ctx) = 0;

  // One SDL event, AFTER ImGui has had first refusal. Return true to consume.
  virtual bool OnEvent(const SDL_Event& event) { (void)event; return false; }

  // The surface changed size; the swapchain has already been resized. Return
  // false to stop the loop: a target rebuild that failed leaves nothing sane.
  virtual bool OnResize(uint32_t width, uint32_t height) {
    (void)width; (void)height; return true;
  }

  // After IRhiDevice::BeginFrame. Recycle per-frame allocators HERE, not in
  // Render -- a SKIPPED frame still consumes its slot.
  virtual void OnFrameBegin(uint64_t frame_index) { (void)frame_index; }

  // Camera and simulation.
  virtual void Update(const FrameTime& time) { (void)time; }

  // Per-frame ImGui windows. THE DEBUG UI SURFACE, distinct from any in-world
  // game UI. The layer runs the context and the pass; this only declares
  // windows.
  virtual void DrawUI() {}

  // Record into the acquired backbuffer. Return false to skip presenting this
  // frame; the frame still ends, so pacing stays sound.
  virtual bool Render(rhi::ITextureView* target, const FrameInfo& info) = 0;
};

struct RhiAppConfig {
  std::string title = "badlands";
  uint32_t width = 1280;   // POINTS, as SDL takes
  uint32_t height = 720;
  rhi::ColorSpace color_space = rhi::ColorSpace::Srgb;
  bool prefer_hdr = false;
  bool vsync = true;
  // Search paths for the Slang compiler, in order.
  std::vector<std::string> shader_paths;

  // An EXISTING device to run on, borrowed. The layer creates its own when this
  // is null.
  //
  // FOUND BY PORTING rhi_lab. An app with both a headless and a windowed path
  // creates its device before it knows which path it is on -- and its scene,
  // targets and pipelines with it. If the layer then made a SECOND device, the
  // app would render with one into the other's drawable: two devices, resources
  // that belong to neither consistently, and Metal forgiving enough that it
  // draws anyway. Lending the device is the fix; owning it is only the default.
  rhi::IRhiDevice* device = nullptr;
};

// Flags the layer parses out of argv, so every app spells them the same way.
struct RhiAppOptions {
  uint64_t max_frames = 0;       // --frames N; 0 = until quit
  float fixed_dt = 0.0f;         // --fixed-dt D; 0 = wall clock
  std::string screenshot_path;   // --screenshot PATH
  bool valid = true;
};

// Parses the layer's own flags and REMOVES them from argv, leaving the rest for
// the app. Exposed for its own test: a flag that is parsed but not removed
// reaches the app's parser as an unknown argument and gets refused there, which
// reads as the app rejecting a flag the layer advertises.
RhiAppOptions ParseAppArgs(int& argc, char** argv);

// Should this frame be the one read back for --screenshot?
//
// THE LAST FRAME, NOT THE FIRST. It used to be "the first frame that renders,
// and never again", which makes --frames N --fixed-dt D useless for the thing
// those flags exist for: 60 deterministic frames were run and frame 0 was
// written. `frame_cap` of 0 means the loop is uncapped, and there is no last
// frame to wait for, so the first one is the only answer available.
//
// Pure, and exposed, so the rule can be checked without a window -- neither app
// has an animating scene, so an end-to-end check of it would pass vacuously.
constexpr bool ShouldCapture(uint64_t frame_index, uint64_t frame_cap,
                             bool already_captured, bool requested) {
  if (!requested || already_captured) return false;
  if (frame_cap == 0) return true;
  return frame_index + 1 >= frame_cap;
}

// The polled keyboard a view is allowed to see this frame.
//
// FILTERING EVENTS IS NOT ENOUGH. WASD is read from SDL_GetKeyboardState, which
// no event filter touches, so a view kept flying the camera while the user
// typed an exact value into a debug slider -- one letter at a time. Gated in
// the layer rather than per view, because a view that forgets has the same bug.
constexpr const bool* KeysForFrame(bool ui_wants_keyboard, const bool* polled) {
  return ui_wants_keyboard ? nullptr : polled;
}

// What the process should exit with, given what the run actually did.
//
// EXTRACTED SO THE SILENT SUCCESSES ARE TESTABLE. Two of them shipped: a
// --screenshot run that never recorded a readback exited 0 having written no
// file, and a run whose every Acquire was Skipped exited 0 having presented
// nothing -- the second of which made object_viewer's windowed ctests
// unfailable against a permanently black window.
struct RunOutcome {
  bool aborted = false;
  uint64_t frames_begun = 0;
  uint64_t frames_presented = 0;
  bool screenshot_requested = false;
  bool screenshot_captured = false;
  bool screenshot_written = false;
};
int ExitCodeFor(const RunOutcome& outcome);

class RhiApp {
 public:
  using ViewFactory = std::function<std::unique_ptr<RhiAppView>(
      const RhiAppContext&)>;

  explicit RhiApp(RhiAppConfig config) : config_(std::move(config)) {}

  // Runs to completion. Returns a process exit code. Parses and STRIPS the
  // layer's own flags from argv first.
  int Run(int argc, char** argv, const ViewFactory& factory,
          RunStats* out_stats = nullptr);

  // The same, with the flags already parsed.
  //
  // EXISTS BECAUSE ORDER MATTERS. An app with its own parser must strip the
  // layer's flags BEFORE running it, or --frames reaches the app as an unknown
  // argument and gets refused -- the app rejecting a flag the layer documents.
  // So the app calls ParseAppArgs itself, up front, and hands the result here
  // rather than having argv parsed twice.
  // `out_stats` receives what the loop actually did. Exposed because an app's
  // own assertions need it: "the window really did resize" and "some frame was
  // presented" are claims about the LOOP, and a view cannot see either.
  int RunParsed(const RhiAppOptions& options, const ViewFactory& factory,
                RunStats* out_stats = nullptr);

 private:
  RhiAppConfig config_;
};

}  // namespace badlands::rhi_app
