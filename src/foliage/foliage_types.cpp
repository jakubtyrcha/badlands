#include "foliage/foliage_types.hpp"

#include <algorithm>
#include <cmath>

namespace badlands::foliage {

glm::ivec2 FoliageField::CellCoordAt(float x, float z) const {
  if (empty()) return glm::ivec2(0);
  const glm::vec2 local = (glm::vec2(x, z) - origin_m) / kFoliageCellSizeM;
  // floor, not truncation: a position just left of the origin must land in
  // cell -1 (then clamp to 0), not in cell 0 alongside a position just right
  // of it.
  const int cx = static_cast<int>(std::floor(local.x));
  const int cz = static_cast<int>(std::floor(local.y));
  return glm::ivec2(std::clamp(cx, 0, cells_x - 1),
                    std::clamp(cz, 0, cells_z - 1));
}

int FoliageField::CellIndexAt(float x, float z) const {
  if (empty()) return -1;
  const glm::ivec2 c = CellCoordAt(x, z);
  return CellIndex(c.x, c.y);
}

size_t FoliageField::InstanceCount() const {
  size_t n = 0;
  for (const std::vector<FoliageInstance>& c : cells) n += c.size();
  return n;
}

}  // namespace badlands::foliage
