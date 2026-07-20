#pragma once

// Reliable mouse input for the ImGui viewer apps, driven from the SDL event
// stream -- NOT the stock ImGui SDL3 backend, and NOT a per-frame poll.
//
// The stock backend DROPS a mouse-button event whose windowID it does not own
// (imgui_impl_sdl3.cpp), so on macOS a BUTTON_UP arriving with a foreign windowID
// during focus flicker leaves io.MouseDown stuck -> WantCapture* stuck -> dead
// input. A per-frame poll of SDL_GetGlobalMouseState avoids that but (a) misses a
// press+release that share one (possibly hitching) frame -> dropped clicks,
// (b) reads the DESKTOP-global button state, so another app's button bleeds in,
// and (c) must re-inject after the backend's NewFrame, fighting event trickling.
//
// This tracker consumes OUR window's mouse events directly and forwards them to
// ImGui as discrete events, in temporal order, exactly as a backend should: so
// ImGui's trickle queue registers even a same-frame click, no foreign app can
// inject buttons, and a release is ALWAYS honoured (a foreign-windowID UP can
// never leave a button stuck). It also OWNS the OS mouse capture: while any
// button is held it captures the mouse so a drag that leaves the window keeps
// delivering motion (position stays valid outside the window without trusting
// global state). Capture is toggled on the button events themselves, so it is
// independent of the render loop (never left engaged by a skipped frame).

#include <cstdint>

union SDL_Event;
struct ImGuiIO;

namespace badlands {

// Which ImGui mouse button an SDL button maps to (0=left,1=right,2=middle,
// 3=X1,4=X2), or -1 if none. Pure.
int ImGuiButtonIndex(unsigned char sdl_button);

// The OS-capture side effect a button transition implies. Pure.
enum class CaptureAction { kNone, kEnable, kDisable };

// Apply a button transition to `buttons` (bitmask over ImGui button indices) and
// report whether OS mouse capture must change. Capture is on iff any button is
// held; `captured` is the current state. Pure -- no SDL/ImGui calls.
CaptureAction ApplyButton(uint32_t& buttons, int imgui_button, bool down,
                          bool captured);

// Event-driven mouse feeder. One instance per window/loop.
class ImGuiMouseInput {
 public:
  // Feed one SDL event to ImGui. Returns true if it was a mouse event this
  // CONSUMED -- the caller must then NOT forward it to the ImGui SDL3 backend
  // (which would double-handle it or wedge on it). Wheel and non-mouse events
  // return false; forward those to the backend as usual.
  bool ProcessEvent(const SDL_Event& e, ImGuiIO& io);

  uint32_t buttons() const { return buttons_; }
  bool captured() const { return captured_; }

 private:
  uint32_t buttons_ = 0;   // bit b set == ImGui button b currently held
  bool captured_ = false;  // OS mouse capture engaged (during a drag)
};

}  // namespace badlands
