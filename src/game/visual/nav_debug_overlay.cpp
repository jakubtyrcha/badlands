#include "game/visual/nav_debug_overlay.hpp"

#include <imgui.h>

namespace badlands {

void NavDebugOverlay::Rebuild(Sim& sim, DebugLineBuffer& out,
                              const GroundHeightFn& ground_y) {
  // No Clear() and no SceneContext: the buffer is the host's and may already
  // hold another overlay's lines. See the header.
  DebugLineBuffer& lines = out;
  // Ride the terrain surface (+ a small lift) so the overlay hugs the ground.
  auto gy = [&](float x, float z, float lift) { return ground_y(x, z) + lift; };

  if (show_mesh_) {
    cells_ = sim.NavDebugCells();
    constexpr float lift = 0.12f;
    for (const NavDebugCell& c : cells_) {
      const glm::vec3 col =
          !c.passable
              ? glm::vec3(0.85f, 0.12f, 0.12f)  // obstacle / water / mountain: red
              // Passable terrain: green (cheap) -> yellow (dear), by cost.
              : glm::mix(glm::vec3(0.2f, 0.8f, 0.25f), glm::vec3(0.9f, 0.85f, 0.1f),
                         glm::clamp((c.cost - 1.0f) / 1.5f, 0.0f, 1.0f));
      const glm::vec3 a(c.min_x, gy(c.min_x, c.min_z, lift), c.min_z);
      const glm::vec3 b(c.max_x, gy(c.max_x, c.min_z, lift), c.min_z);
      const glm::vec3 d(c.max_x, gy(c.max_x, c.max_z, lift), c.max_z);
      const glm::vec3 e(c.min_x, gy(c.min_x, c.max_z, lift), c.max_z);
      if (c.tri_mask == 0 || c.tri_mask == 0xF) {
        lines.AddLine(a, b, col);
        lines.AddLine(b, d, col);
        lines.AddLine(d, e, col);
        lines.AddLine(e, a, col);
        continue;
      }
      // A PARTIAL cell: a diagonal footprint covers some corner triangles and
      // not others. Drawing the rect whole would show exactly the fat wall the
      // triangle mask exists to stop showing -- so draw the four quarters
      // individually, each in its own colour. Corner order is -Z, +X, +Z, -X
      // (game/src/navmesh/tri.h), and the shared apex is the cell centre.
      const float mid_x = 0.5f * (c.min_x + c.max_x);
      const float mid_z = 0.5f * (c.min_z + c.max_z);
      const glm::vec3 centre(mid_x, gy(mid_x, mid_z, lift), mid_z);
      const glm::vec3 corner_edge[4][2] = {{a, b}, {b, d}, {d, e}, {e, a}};
      for (int t = 0; t < 4; ++t) {
        const bool solid = (c.tri_mask & (1u << t)) != 0;
        const glm::vec3 tc = solid ? glm::vec3(0.85f, 0.12f, 0.12f) : col;
        lines.AddLine(corner_edge[t][0], corner_edge[t][1], tc);
        lines.AddLine(corner_edge[t][0], centre, tc);
        lines.AddLine(corner_edge[t][1], centre, tc);
      }
    }
  }

  if (a_ && b_) {
    path_ = sim.NavQuery(a_->x, a_->y, b_->x, b_->y);
    const glm::vec3 col =
        path_.reachable ? glm::vec3(0.15f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.2f, 0.2f);
    const std::vector<float>& w = path_.waypoints_xz;
    for (size_t i = 2; i < w.size(); i += 2) {
      lines.AddLine(glm::vec3(w[i - 2], gy(w[i - 2], w[i - 1], 0.25f), w[i - 1]),
                     glm::vec3(w[i], gy(w[i], w[i + 1], 0.25f), w[i + 1]), col, 3.0f);
    }
  }

  auto marker = [&](glm::vec2 p, glm::vec3 col) {
    const float y = gy(p.x, p.y, 0.3f);
    constexpr float s = 0.6f;
    lines.AddLine(glm::vec3(p.x - s, y, p.y), glm::vec3(p.x + s, y, p.y), col, 3.0f);
    lines.AddLine(glm::vec3(p.x, y, p.y - s), glm::vec3(p.x, y, p.y + s), col, 3.0f);
  };
  if (a_) marker(*a_, glm::vec3(0.2f, 0.6f, 1.0f));
  if (b_) marker(*b_, glm::vec3(1.0f, 0.9f, 0.2f));
}

void NavDebugOverlay::Pick(glm::vec2 world_xz) {
  if (!a_ || b_) {  // first click, or restart after a full pair
    a_ = world_xz;
    b_.reset();
  } else {
    b_ = world_xz;
  }
}

void NavDebugOverlay::DrawControls() {
  ImGui::Checkbox("Nav mesh", &show_mesh_);
  if (show_mesh_) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu cells; green=cheap, red=blocked)", cells_.size());
  }
  ImGui::Checkbox("Nav pick path (click 2 points)", &pick_mode_);
  if (a_ && b_) {
    if (path_.reachable) {
      ImGui::Text("path: cost %.1f  (%zu waypoints)", path_.cost,
                  path_.waypoints_xz.size() / 2);
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "unreachable");
    }
  }
  if ((a_ || b_) && ImGui::SmallButton("clear path")) {
    a_.reset();
    b_.reset();
  }
}

}  // namespace badlands
