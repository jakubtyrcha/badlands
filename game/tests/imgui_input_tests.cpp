// Reliability tests for src/engine/app/imgui_mouse_input.{hpp,cpp}.
//
// Layers:
//  1. Pure logic: ImGuiButtonIndex + ApplyButton (button bitmask + capture edges).
//  2. Headless ImGui: ImGuiMouseInput feeds discrete SDL events to a real ImGui
//     context and we assert io/click behaviour -- covering every failure mode the
//     earlier poll-based version had:
//       - a press+release that share ONE frame still registers a click (a poll
//         that samples only end-of-frame state dropped this);
//       - a BUTTON_UP with a foreign windowID still releases (never wedges);
//       - a drag that leaves the window keeps a valid position (capture);
//       - leaving the window idle marks the mouse unavailable.
//  3. A RAII-guarded stock-backend repro documenting the windowID-drop wedge this
//     tracker exists to sidestep.
//
// All SDL/ImGui lifetimes are RAII so a failing assertion never leaks context.

#include <catch_amalgamated.hpp>

#include <cfloat>
#include <initializer_list>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include "engine/app/imgui_mouse_input.hpp"

using namespace badlands;

namespace {

// SDL event builders. ImGuiMouseInput reads only the fields set here.
SDL_Event MotionEvent(SDL_WindowID id, float x, float y) {
  SDL_Event e{};
  e.type = SDL_EVENT_MOUSE_MOTION;
  e.motion.windowID = id;
  e.motion.which = 1;
  e.motion.x = x;
  e.motion.y = y;
  return e;
}

SDL_Event ButtonEvent(SDL_WindowID id, bool down, float x, float y,
                      Uint8 button) {
  SDL_Event e{};
  e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  e.button.windowID = id;
  e.button.which = 1;
  e.button.button = button;
  e.button.x = x;
  e.button.y = y;
  return e;
}

SDL_Event WindowEvent(SDL_EventType type, SDL_WindowID id) {
  SDL_Event e{};
  e.type = type;
  e.window.windowID = id;
  return e;
}

// RAII: SDL video + an ImGui context with a built atlas, no renderer backend.
struct HeadlessImGui {
  HeadlessImGui() {
    SDL_Init(SDL_INIT_VIDEO);  // makes SDL_CaptureMouse a safe no-op in-test
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(200.0f, 200.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
  }
  ~HeadlessImGui() {
    ImGui::DestroyContext();
    SDL_Quit();
  }
};

void NewFrameRender() {
  ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
  ImGui::Render();
}

// Renders a window-filling button and drives it through ImGuiMouseInput.
struct UiHarness {
  ImGuiMouseInput mouse;
  ImVec2 button_center{0.0f, 0.0f};

  bool Frame(std::initializer_list<SDL_Event> events) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    for (const SDL_Event& e : events) mouse.ProcessEvent(e, io);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(200, 200));
    ImGui::Begin("w", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);
    const bool clicked = ImGui::Button("B", ImVec2(180, 50));
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    button_center = ImVec2(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y));
    ImGui::End();
    ImGui::Render();
    return clicked;
  }
};

constexpr SDL_WindowID kWin = 7;

}  // namespace

// ---------------------------------------------------------------------------
// Pure logic.
// ---------------------------------------------------------------------------

TEST_CASE("ImGuiButtonIndex maps SDL buttons to ImGui indices") {
  CHECK(ImGuiButtonIndex(SDL_BUTTON_LEFT) == 0);
  CHECK(ImGuiButtonIndex(SDL_BUTTON_RIGHT) == 1);
  CHECK(ImGuiButtonIndex(SDL_BUTTON_MIDDLE) == 2);
  CHECK(ImGuiButtonIndex(SDL_BUTTON_X1) == 3);
  CHECK(ImGuiButtonIndex(SDL_BUTTON_X2) == 4);
  CHECK(ImGuiButtonIndex(99) == -1);
}

TEST_CASE("ApplyButton toggles capture only on the 0<->held edges") {
  uint32_t b = 0;
  CHECK(ApplyButton(b, 0, true, false) == CaptureAction::kEnable);  // 0 -> held
  CHECK(b == 0b1u);
  CHECK(ApplyButton(b, 1, true, true) == CaptureAction::kNone);  // already held
  CHECK(b == 0b11u);
  CHECK(ApplyButton(b, 0, false, true) == CaptureAction::kNone);  // still held
  CHECK(b == 0b10u);
  CHECK(ApplyButton(b, 1, false, true) == CaptureAction::kDisable);  // -> none
  CHECK(b == 0u);
}

// ---------------------------------------------------------------------------
// Headless ImGui behaviour.
// ---------------------------------------------------------------------------

TEST_CASE("A normal press-release over a button registers a click") {
  HeadlessImGui ctx;
  UiHarness h;
  h.Frame({});  // warm-up: lay out, capture centre
  const ImVec2 c = h.button_center;
  h.Frame({MotionEvent(kWin, c.x, c.y)});  // hover
  h.Frame({ButtonEvent(kWin, true, c.x, c.y, SDL_BUTTON_LEFT)});   // press
  const bool clicked = h.Frame({ButtonEvent(kWin, false, c.x, c.y, SDL_BUTTON_LEFT)});
  CHECK(clicked == true);
}

TEST_CASE("A press+release in ONE frame still clicks (poll dropped this)") {
  HeadlessImGui ctx;
  const ImVec2 c = [] { UiHarness w; w.Frame({}); return w.button_center; }();

  // GREEN: discrete down+up in a single frame -> ImGui's trickle applies the
  // down this frame and the up next frame, so the click registers. A per-frame
  // poll that only sampled the end-of-frame button state (up) never saw the
  // down and would drop this entirely -- the failure this design removes.
  {
    UiHarness h;
    h.Frame({});
    h.Frame({MotionEvent(kWin, c.x, c.y)});  // hover
    const bool f1 = h.Frame({ButtonEvent(kWin, true, c.x, c.y, SDL_BUTTON_LEFT),
                             ButtonEvent(kWin, false, c.x, c.y, SDL_BUTTON_LEFT)});
    const bool f2 = h.Frame({});  // deferred up applies here
    CHECK(f1 == false);
    CHECK(f2 == true);
  }

  // RED model: feed only the net end-of-frame state (button up, no discrete
  // down) -- what a poll observes for a click that began+ended within a frame.
  {
    UiHarness h;
    h.Frame({});
    h.Frame({MotionEvent(kWin, c.x, c.y)});
    ImGui::GetIO().AddMouseButtonEvent(0, false);  // net state only, no down
    const bool clicked = h.Frame({}) || h.Frame({});
    CHECK(clicked == false);
  }
}

TEST_CASE("A release is honoured regardless of windowID (never wedges)") {
  HeadlessImGui ctx;
  ImGuiMouseInput mouse;
  ImGuiIO& io = ImGui::GetIO();

  SDL_Event down = ButtonEvent(kWin, true, 10, 10, SDL_BUTTON_LEFT);
  mouse.ProcessEvent(down, io);
  NewFrameRender();
  CHECK(io.MouseDown[0] == true);
  CHECK(mouse.buttons() == 1u);

  // The matching UP arrives with a FOREIGN windowID (0) -- the exact event the
  // stock backend drops. Our tracker never inspects windowID, so it releases.
  SDL_Event up = ButtonEvent(/*windowID*/ 0, false, 10, 10, SDL_BUTTON_LEFT);
  mouse.ProcessEvent(up, io);
  NewFrameRender();
  CHECK(io.MouseDown[0] == false);
  CHECK(mouse.buttons() == 0u);
}

TEST_CASE("A drag that leaves the window keeps tracking (capture)") {
  HeadlessImGui ctx;
  ImGuiMouseInput mouse;
  ImGuiIO& io = ImGui::GetIO();

  SDL_Event m0 = MotionEvent(kWin, 50, 50);
  mouse.ProcessEvent(m0, io);
  SDL_Event down = ButtonEvent(kWin, true, 50, 50, SDL_BUTTON_LEFT);
  mouse.ProcessEvent(down, io);
  NewFrameRender();
  CHECK(io.MouseDown[0] == true);
  CHECK(mouse.captured() == true);  // capture engaged while a button is held

  // Cursor drags outside; capture keeps motion flowing, and a spurious LEAVE must
  // NOT invalidate the position.
  SDL_Event m1 = MotionEvent(kWin, 500, -30);
  mouse.ProcessEvent(m1, io);
  SDL_Event leave = WindowEvent(SDL_EVENT_WINDOW_MOUSE_LEAVE, kWin);
  mouse.ProcessEvent(leave, io);
  NewFrameRender();
  CHECK(io.MouseDown[0] == true);
  CHECK(ImGui::IsMousePosValid(&io.MousePos));
  CHECK(io.MousePos.x == Catch::Approx(500.0f));

  SDL_Event up = ButtonEvent(kWin, false, 500, -30, SDL_BUTTON_LEFT);
  mouse.ProcessEvent(up, io);
  NewFrameRender();
  CHECK(mouse.captured() == false);  // released on button up
}

TEST_CASE("Leaving the window idle marks the mouse unavailable") {
  HeadlessImGui ctx;
  ImGuiMouseInput mouse;
  ImGuiIO& io = ImGui::GetIO();

  SDL_Event m = MotionEvent(kWin, 50, 50);
  mouse.ProcessEvent(m, io);
  NewFrameRender();
  CHECK(ImGui::IsMousePosValid(&io.MousePos));

  SDL_Event leave = WindowEvent(SDL_EVENT_WINDOW_MOUSE_LEAVE, kWin);
  mouse.ProcessEvent(leave, io);  // no button held
  NewFrameRender();
  CHECK_FALSE(ImGui::IsMousePosValid(&io.MousePos));
}

// ---------------------------------------------------------------------------
// Documentation repro: the stock backend really does wedge on a foreign-windowID
// BUTTON_UP -- which is why the tracker above exists.
// ---------------------------------------------------------------------------

namespace {
// RAII SDL window + ImGui context + SDL3 backend; exception-safe teardown.
struct BackendCtx {
  SDL_Window* win = nullptr;
  bool ok = false;
  BackendCtx() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return;
    win = SDL_CreateWindow("input_test", 200, 200, SDL_WINDOW_HIDDEN);
    if (!win) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(200.0f, 200.0f);
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!ImGui_ImplSDL3_InitForOther(win)) return;
    // Don't grab the real cursor while reproducing a stuck button.
    ImGui_ImplSDL3_SetMouseCaptureMode(ImGui_ImplSDL3_MouseCaptureMode_Disabled);
    ok = true;
  }
  ~BackendCtx() {
    if (ImGui::GetCurrentContext()) {
      if (ok) ImGui_ImplSDL3_Shutdown();
      ImGui::DestroyContext();
    }
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
  }
};
}  // namespace

TEST_CASE("Stock ImGui SDL3 backend wedges on a foreign-windowID BUTTON_UP") {
  BackendCtx ctx;
  if (!ctx.ok) {
    SUCCEED("SDL video unavailable (headless CI); skipping backend repro");
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  const SDL_WindowID id = SDL_GetWindowID(ctx.win);

  auto backend_frame = [] {
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::Render();
  };

  SDL_Event enter = WindowEvent(SDL_EVENT_WINDOW_MOUSE_ENTER, id);
  ImGui_ImplSDL3_ProcessEvent(&enter);
  SDL_Event down = ButtonEvent(id, true, 50, 50, SDL_BUTTON_LEFT);
  ImGui_ImplSDL3_ProcessEvent(&down);
  backend_frame();
  REQUIRE(io.MouseDown[0] == true);

  SDL_Event leave = WindowEvent(SDL_EVENT_WINDOW_MOUSE_LEAVE, id);
  ImGui_ImplSDL3_ProcessEvent(&leave);
  SDL_Event up = ButtonEvent(/*windowID*/ 0, false, 50, 50, SDL_BUTTON_LEFT);
  ImGui_ImplSDL3_ProcessEvent(&up);
  backend_frame();
  // The backend DROPS the foreign-windowID UP -> stuck. This is the bug our
  // ImGuiMouseInput avoids by never routing buttons through the backend.
  CHECK(io.MouseDown[0] == true);
}
