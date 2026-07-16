#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/biomes.hpp"
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// One 10 m grid block: a single representative height (world meters), its
// dominant biome, and the section it belongs to (assigned by extract_sections).
struct Block {
  float height = 0.0f;
  uint8_t biome = 0;
  int section_id = -1;
};

// A section: a connected run of blocks of gently-varying height (a "terrace").
struct Section {
  int id = 0;
  Biome biome = Biome::Plains;  // dominant biome among the section's blocks
  int block_count = 0;
  float mean_height = 0.0f;
  glm::vec2 centroid_m{0.0f};  // area centroid in world meters
};

// Adjacency between two sections (a ledge), with the average vertical step and
// how many block pairs share the border.
struct SectionEdge {
  int a = 0;
  int b = 0;
  float height_step = 0.0f;  // mean |Δheight| across the shared border
  int border_len = 0;        // number of adjacent block pairs on the border
};

struct SectionGraph {
  std::vector<Section> nodes;
  std::vector<SectionEdge> edges;
};

// Reduce a WxH heightmap (+ per-pixel biome) to a
// (W/kSamplesPerBlock) x (H/kSamplesPerBlock) block grid. Each block's height
// is the median (or mean if !median) of its footprint; its biome is the
// footprint majority. Remainder samples that don't fill a whole block are
// dropped.
Field2D<Block> reduce_to_blocks(const Field2D<float>& height,
                                const Field2D<uint8_t>& biome, bool median);

// Flood-fill blocks into sections: 4-connected neighbors join when their
// |Δheight| <= section_step. Sections smaller than min_section_blocks merge
// into their nearest-height neighbor. Mutates blocks (section_id compacted to
// 0..N-1) and returns the section graph (nodes + ledge edges).
SectionGraph extract_sections(Field2D<Block>& blocks, float section_step,
                              int min_section_blocks);

}  // namespace badlands::mapgen
