#include "engine/rendering/debug_line_expand.hpp"

#include "engine/rendering/debug_line_buffer.hpp"

namespace badlands {

std::vector<float> ExpandDebugLines(const DebugLineBuffer& lines,
                                    const glm::mat4& view, const glm::mat4& proj,
                                    glm::vec2 screen_size,
                                    glm::vec3 camera_world_pos) {
  std::vector<float> data;
  const size_t line_count = lines.lines.size();
  if (line_count == 0) return data;

  const glm::mat4 vp = proj * view;
  const glm::mat4 inv_vp = glm::inverse(vp);
  data.reserve(line_count * 6 * 8);

  auto write_vertex = [&](glm::vec3 pos, glm::vec3 color, float dist,
                          float half_thickness) {
    data.insert(data.end(), {pos.x, pos.y, pos.z, color.x, color.y, color.z,
                             dist, half_thickness});
  };
  auto degenerate = [&](const DebugLine& l) {
    for (int i = 0; i < 6; ++i) write_vertex(l.start, l.color, 0.0f, 0.0f);
  };

  // Keep the perspective divide stable: clip endpoints to w >= kWEps.
  constexpr float kWEps = 1e-4f;

  for (const DebugLine& line : lines.lines) {
    glm::vec4 clip_start = vp * glm::vec4(line.start - camera_world_pos, 1.0f);
    glm::vec4 clip_end = vp * glm::vec4(line.end - camera_world_pos, 1.0f);

    const bool start_behind = clip_start.w <= kWEps;
    const bool end_behind = clip_end.w <= kWEps;
    if (start_behind && end_behind) {
      degenerate(line);
      continue;
    }
    // Near-plane clip: move the behind-camera endpoint to the point on the
    // segment where w == kWEps (linear in clip space).
    if (start_behind) {
      clip_start = glm::mix(clip_start, clip_end,
                            (kWEps - clip_start.w) / (clip_end.w - clip_start.w));
    }
    if (end_behind) {
      clip_end = glm::mix(clip_end, clip_start,
                          (kWEps - clip_end.w) / (clip_start.w - clip_end.w));
    }

    const glm::vec2 ndc_start = glm::vec2(clip_start) / clip_start.w;
    const glm::vec2 ndc_end = glm::vec2(clip_end) / clip_end.w;
    const glm::vec2 screen_start = (ndc_start * 0.5f + 0.5f) * screen_size;
    const glm::vec2 screen_end = (ndc_end * 0.5f + 0.5f) * screen_size;
    glm::vec2 screen_dir = screen_end - screen_start;
    const float screen_len = glm::length(screen_dir);
    if (screen_len < 0.001f) {
      degenerate(line);
      continue;
    }
    screen_dir /= screen_len;
    const glm::vec2 perp(-screen_dir.y, screen_dir.x);
    const float half_thickness = line.thickness * 0.5f;
    const float half_extent = half_thickness + 1.0f;  // +1px AA fringe
    const glm::vec2 offset_ndc = perp / screen_size * 2.0f;

    // Perspective-correct perpendicular offset (scale by w), in clip space.
    auto corner = [&](const glm::vec4& clip, float sign) {
      const glm::vec2 o = offset_ndc * (half_extent * sign * clip.w);
      return clip + glm::vec4(o.x, o.y, 0.0f, 0.0f);
    };
    auto unproject = [&](const glm::vec4& clip) {
      const glm::vec4 w = inv_vp * clip;
      return glm::vec3(w) / w.w + camera_world_pos;  // back to world space
    };
    const glm::vec3 w0 = unproject(corner(clip_start, +1.0f));
    const glm::vec3 w1 = unproject(corner(clip_start, -1.0f));
    const glm::vec3 w2 = unproject(corner(clip_end, +1.0f));
    const glm::vec3 w3 = unproject(corner(clip_end, -1.0f));

    write_vertex(w0, line.color, +half_extent, half_thickness);
    write_vertex(w1, line.color, -half_extent, half_thickness);
    write_vertex(w2, line.color, +half_extent, half_thickness);
    write_vertex(w2, line.color, +half_extent, half_thickness);
    write_vertex(w1, line.color, -half_extent, half_thickness);
    write_vertex(w3, line.color, -half_extent, half_thickness);
  }

  return data;
}

}  // namespace badlands
