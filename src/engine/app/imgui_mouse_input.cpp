#include "engine/app/imgui_mouse_input.hpp"

#include <cfloat>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace badlands {

int ImGuiButtonIndex(unsigned char sdl_button) {
  switch (sdl_button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
  }
}

CaptureAction ApplyButton(uint32_t& buttons, int imgui_button, bool down,
                          bool captured) {
  if (down) {
    buttons |= (1u << imgui_button);
  } else {
    buttons &= ~(1u << imgui_button);
  }
  const bool any = buttons != 0;
  if (any && !captured) return CaptureAction::kEnable;
  if (!any && captured) return CaptureAction::kDisable;
  return CaptureAction::kNone;
}

bool ImGuiMouseInput::ProcessEvent(const SDL_Event& e, ImGuiIO& io) {
  switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
      // Window-relative logical coords (ImGui's space). During a drag the OS
      // capture keeps these arriving with out-of-bounds values -- exactly what a
      // slider/window drag needs to keep tracking past the window edge.
      io.AddMousePosEvent(e.motion.x, e.motion.y);
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      const int b = ImGuiButtonIndex(e.button.button);
      if (b < 0) return true;  // consumed but not tracked
      const bool down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
      // Feed the discrete event so ImGui's trickle queue registers even a
      // press+release that share a frame. The release is honoured regardless of
      // windowID, so a foreign-windowID UP can never leave a button stuck.
      io.AddMouseButtonEvent(b, down);
      switch (ApplyButton(buttons_, b, down, captured_)) {
        case CaptureAction::kEnable:
          SDL_CaptureMouse(true);
          captured_ = true;
          break;
        case CaptureAction::kDisable:
          SDL_CaptureMouse(false);
          captured_ = false;
          break;
        case CaptureAction::kNone:
          break;
      }
      return true;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      // Cursor left the window. If nothing is held, mark the mouse unavailable so
      // no widget stays hovered. During a drag (a button held) the captured
      // motion events keep the position valid, so ignore the leave.
      if (buttons_ == 0) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
      return true;  // do NOT let the backend also invalidate our position
    default:
      return false;  // wheel / keyboard / text / focus / resize -> the backend
  }
}

}  // namespace badlands
