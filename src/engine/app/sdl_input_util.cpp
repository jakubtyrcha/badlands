#include "engine/app/sdl_input_util.hpp"

namespace badlands {

bool EventWindowLogicalSize(SDL_WindowID window_id, glm::vec2& out_size) {
  SDL_Window* window = SDL_GetWindowFromID(window_id);
  if (window == nullptr) return false;
  int w = 0, h = 0;
  if (!SDL_GetWindowSize(window, &w, &h)) return false;
  if (w <= 0 || h <= 0) return false;
  out_size = glm::vec2(static_cast<float>(w), static_cast<float>(h));
  return true;
}

float NormalizedWheelY(const SDL_MouseWheelEvent& wheel) {
  return wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -wheel.y : wheel.y;
}

}  // namespace badlands
