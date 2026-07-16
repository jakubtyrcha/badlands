#pragma once

// Small SDL input helpers shared by AppViews that do mouse picking.

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

namespace badlands {

// Logical (point) size of the window an event came from.
//
// Use THIS — not the width/height handed to AppView::HandleEvent — to turn a
// mouse position into NDC. SDL reports mouse coordinates (SDL_MouseMotionEvent
// x/y, SDL_MouseWheelEvent mouse_x/mouse_y) in logical points, whereas
// HandleEvent's width/height come from SDL_GetWindowSizeInPixels and are
// PHYSICAL pixels. On a HiDPI display those differ by the window's pixel
// density, so dividing a point coordinate by a pixel extent silently scales
// every picking ray away from the cursor.
//
// Returns false if the window can't be resolved or reports a degenerate size.
bool EventWindowLogicalSize(SDL_WindowID window_id, glm::vec2& out_size);

// Wheel delta with SDL's FLIPPED convention normalized away.
//
// macOS "natural scrolling" makes SDL set direction == SDL_MOUSEWHEEL_FLIPPED
// and negate x/y; without undoing that, zoom is inverted for those users (and
// correct for everyone else, which is why it is easy to ship). Returns y in a
// consistent sense: positive == scrolled away from the user.
//
// Values are fractional for precise devices (trackpads emit many small deltas
// rather than ±1 notches), so callers should scale by the delta rather than
// treating each event as one step.
float NormalizedWheelY(const SDL_MouseWheelEvent& wheel);

}  // namespace badlands
