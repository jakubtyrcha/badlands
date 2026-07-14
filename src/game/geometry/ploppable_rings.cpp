#include "game/geometry/ploppable_rings.h"

#include <cmath>

namespace badlands {

namespace {

constexpr float kPi = 3.14159265358979f;

std::vector<glm::vec2> Octagon(float r) {
  std::vector<glm::vec2> v;
  v.reserve(8);
  for (int i = 0; i < 8; ++i) {
    float a = static_cast<float>(i) / 8.0f * 2.0f * kPi;
    v.push_back({r * std::cos(a), r * std::sin(a)});
  }
  return v;  // CCW
}

std::vector<glm::vec2> Rect(float hx, float hz) {
  return {{-hx, -hz}, {hx, -hz}, {hx, hz}, {-hx, hz}};  // CCW
}

// The un-rotated, origin-centered footprint per kind.
std::vector<glm::vec2> BaseRing(GamePloppableKind kind) {
  switch (kind) {
    case GamePloppableKind::RockA:
      return Octagon(2.0f);
    case GamePloppableKind::RockB:
      return Octagon(3.2f);
    case GamePloppableKind::RockC:
      return Rect(3.5f, 1.8f);
    case GamePloppableKind::Sinkhole:
      return Octagon(9.0f);
    case GamePloppableKind::Tree:
    default:
      return {};  // footprint-less
  }
}

}  // namespace

std::vector<glm::vec2> ploppable_local_ring(GamePloppableKind kind, int rot) {
  std::vector<glm::vec2> ring = BaseRing(kind);
  float angle = static_cast<float>(((rot % 8) + 8) % 8) * (kPi / 4.0f);
  float c = std::cos(angle), s = std::sin(angle);
  for (glm::vec2& v : ring) {
    v = {c * v.x - s * v.y, s * v.x + c * v.y};
  }
  return ring;
}

}  // namespace badlands
