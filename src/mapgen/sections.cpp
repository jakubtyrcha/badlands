#include "mapgen/sections.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mapgen/mapgen_constants.hpp"
#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

namespace {

constexpr int kBlockTile = 32;

// 4-connected neighbor offsets.
constexpr int kDx[4] = {1, -1, 0, 0};
constexpr int kDy[4] = {0, 0, 1, -1};

}  // namespace

Field2D<Block> reduce_to_blocks(const Field2D<float>& height,
                                const Field2D<uint8_t>& biome, bool median) {
  const int bx = height.width / kSamplesPerBlock;
  const int by = height.height / kSamplesPerBlock;
  Field2D<Block> blocks(bx, by);

  parallel_tiles(
      bx, by, kBlockTile, [] { return std::vector<float>(); },
      [&](std::vector<float>& foot, int x0, int y0, int x1, int y1) {
        for (int b_y = y0; b_y < y1; ++b_y) {
          for (int b_x = x0; b_x < x1; ++b_x) {
            foot.clear();
            int biome_count[kBiomeCount] = {0};
            const int px0 = b_x * kSamplesPerBlock;
            const int py0 = b_y * kSamplesPerBlock;
            for (int sy = 0; sy < kSamplesPerBlock; ++sy) {
              for (int sx = 0; sx < kSamplesPerBlock; ++sx) {
                const int px = px0 + sx;
                const int py = py0 + sy;
                foot.push_back(height.at(px, py));
                uint8_t bi = biome.at(px, py);
                if (bi < kBiomeCount) ++biome_count[bi];
              }
            }

            float h;
            if (median) {
              const size_t mid = foot.size() / 2;
              std::nth_element(foot.begin(), foot.begin() + mid, foot.end());
              h = foot[mid];
            } else {
              double sum = 0.0;
              for (float v : foot) sum += v;
              h = static_cast<float>(sum / foot.size());
            }

            int best_biome = 0;
            for (int i = 1; i < kBiomeCount; ++i) {
              if (biome_count[i] > biome_count[best_biome]) best_biome = i;
            }

            Block blk;
            blk.height = h;
            blk.biome = static_cast<uint8_t>(best_biome);
            blk.section_id = -1;
            blocks.at(b_x, b_y) = blk;
          }
        }
      });

  return blocks;
}

SectionGraph extract_sections(Field2D<Block>& blocks, float section_step,
                              int min_section_blocks) {
  const int C = blocks.width;
  const int R = blocks.height;

  for (Block& b : blocks.data) b.section_id = -1;

  // --- Flood-fill: join 4-connected neighbors within section_step. ---
  std::vector<int> stack;
  int next_id = 0;
  for (int y = 0; y < R; ++y) {
    for (int x = 0; x < C; ++x) {
      if (blocks.at(x, y).section_id != -1) continue;
      const int id = next_id++;
      blocks.at(x, y).section_id = id;
      stack.clear();
      stack.push_back(y * C + x);
      while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        const int px = p % C;
        const int py = p / C;
        const float ph = blocks.at(px, py).height;
        for (int d = 0; d < 4; ++d) {
          const int qx = px + kDx[d];
          const int qy = py + kDy[d];
          if (qx < 0 || qy < 0 || qx >= C || qy >= R) continue;
          Block& q = blocks.at(qx, qy);
          if (q.section_id != -1) continue;
          if (std::abs(ph - q.height) > section_step) continue;
          q.section_id = id;
          stack.push_back(qy * C + qx);
        }
      }
    }
  }

  // --- Merge sections smaller than min_section_blocks into their nearest-
  // height neighbor. Recompute stats + adjacency each pass (section counts are
  // modest); stop when no mergeable small section remains. ---
  auto section_count = [&]() {
    int mx = -1;
    for (const Block& b : blocks.data) mx = std::max(mx, b.section_id);
    return mx + 1;
  };

  for (;;) {
    const int n = section_count();
    std::vector<int> count(n, 0);
    std::vector<double> sum_h(n, 0.0);
    for (const Block& b : blocks.data) {
      ++count[b.section_id];
      sum_h[b.section_id] += b.height;
    }
    // Adjacency: for each section, the neighbor with the closest mean height.
    std::vector<double> mean(n, 0.0);
    for (int i = 0; i < n; ++i)
      mean[i] = count[i] ? sum_h[i] / count[i] : 0.0;

    // best_neighbor[i] = section id to merge i into (or -1).
    std::vector<int> best(n, -1);
    std::vector<double> best_diff(n, 1e30);
    for (int y = 0; y < R; ++y) {
      for (int x = 0; x < C; ++x) {
        const int a = blocks.at(x, y).section_id;
        for (int d = 0; d < 4; ++d) {
          const int qx = x + kDx[d];
          const int qy = y + kDy[d];
          if (qx < 0 || qy < 0 || qx >= C || qy >= R) continue;
          const int b = blocks.at(qx, qy).section_id;
          if (a == b) continue;
          const double diff = std::abs(mean[a] - mean[b]);
          if (diff < best_diff[a] ||
              (diff == best_diff[a] && b < best[a])) {
            best_diff[a] = diff;
            best[a] = b;
          }
        }
      }
    }

    // Pick the smallest under-min section that has a neighbor (ties: lowest id).
    int victim = -1;
    for (int i = 0; i < n; ++i) {
      if (count[i] == 0 || count[i] >= min_section_blocks) continue;
      if (best[i] < 0) continue;
      if (victim < 0 || count[i] < count[victim]) victim = i;
    }
    if (victim < 0) break;

    const int target = best[victim];
    for (Block& b : blocks.data)
      if (b.section_id == victim) b.section_id = target;
  }

  // --- Compact section ids to 0..N-1 (stable by first appearance order). ---
  std::unordered_map<int, int> remap;
  int compact = 0;
  for (Block& b : blocks.data) {
    auto it = remap.find(b.section_id);
    if (it == remap.end()) {
      remap.emplace(b.section_id, compact);
      b.section_id = compact;
      ++compact;
    } else {
      b.section_id = it->second;
    }
  }
  const int num = compact;

  // --- Build nodes. ---
  SectionGraph graph;
  graph.nodes.resize(num);
  std::vector<std::array<int, kBiomeCount>> biome_hist(num);
  for (auto& h : biome_hist) h.fill(0);
  std::vector<double> sum_h(num, 0.0);
  std::vector<double> sum_cx(num, 0.0);
  std::vector<double> sum_cy(num, 0.0);
  for (int y = 0; y < R; ++y) {
    for (int x = 0; x < C; ++x) {
      const Block& b = blocks.at(x, y);
      const int s = b.section_id;
      auto& node = graph.nodes[s];
      ++node.block_count;
      sum_h[s] += b.height;
      sum_cx[s] += (x + 0.5) * kBlockSizeM;
      sum_cy[s] += (y + 0.5) * kBlockSizeM;
      if (b.biome < kBiomeCount) ++biome_hist[s][b.biome];
    }
  }
  for (int s = 0; s < num; ++s) {
    auto& node = graph.nodes[s];
    node.id = s;
    const int c = std::max(1, node.block_count);
    node.mean_height = static_cast<float>(sum_h[s] / c);
    node.centroid_m = glm::vec2(static_cast<float>(sum_cx[s] / c),
                                static_cast<float>(sum_cy[s] / c));
    int bb = 0;
    for (int i = 1; i < kBiomeCount; ++i)
      if (biome_hist[s][i] > biome_hist[s][bb]) bb = i;
    node.biome = static_cast<Biome>(bb);
  }

  // --- Build edges. Scan the right + down neighbor of each block; accumulate
  // per section-pair the border length and summed |Δheight|. ---
  std::map<std::pair<int, int>, std::pair<double, int>> edge_acc;
  auto consider = [&](int ax, int ay, int bx, int by) {
    const Block& ba = blocks.at(ax, ay);
    const Block& bb = blocks.at(bx, by);
    if (ba.section_id == bb.section_id) return;
    const int lo = std::min(ba.section_id, bb.section_id);
    const int hi = std::max(ba.section_id, bb.section_id);
    auto& acc = edge_acc[{lo, hi}];
    acc.first += std::abs(ba.height - bb.height);
    acc.second += 1;
  };
  for (int y = 0; y < R; ++y) {
    for (int x = 0; x < C; ++x) {
      if (x + 1 < C) consider(x, y, x + 1, y);
      if (y + 1 < R) consider(x, y, x, y + 1);
    }
  }
  graph.edges.reserve(edge_acc.size());
  for (const auto& [key, acc] : edge_acc) {
    SectionEdge e;
    e.a = key.first;
    e.b = key.second;
    e.border_len = acc.second;
    e.height_step =
        acc.second ? static_cast<float>(acc.first / acc.second) : 0.0f;
    graph.edges.push_back(e);
  }

  return graph;
}

}  // namespace badlands::mapgen
