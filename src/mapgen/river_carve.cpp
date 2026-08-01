#include "mapgen/river_carve.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace badlands::mapgen {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Spacing of the centreline stations. Fine enough that the running-minimum bed
// tracks the 1 m base lattice rather than stepping across it, and a power of
// two so a station index is an exact float division.
constexpr float kStationStepM = 0.25f;

// Chord-vs-arc sag budget for the mask rasterization. Half of it would buy
// nothing: the margin is added back into the coverage radius, so a chord is
// conservative at ANY budget and this only trades chord count for mask width.
constexpr float kSagittaM = 0.0625f;

// Bounds on the chord step, so a hairpin arc does not explode the chord count
// and a near-straight one does not walk the whole reach in one AABB.
constexpr float kMinChordM = 0.25f;
constexpr float kMaxChordM = 2.0f;

// The cavity model, from the design doc section 1. Both terms are derived:
// 1.390 is d(Q_bf)/d(Q) at Q_bf = 3*Q with d ~ Q^0.3, and 0.6 is 0.3/0.5, the
// ratio of the depth and width exponents of downstream hydraulic geometry.
constexpr float kBankfullFactor = 1.390f;
constexpr float kBankExponent = 0.6f;

// Corridor half-width from the centreline: max(3w, 2 m) total, per side.
constexpr float kCorridorWidthFactor = 1.5f;
constexpr float kMinCorridorHalfM = 1.0f;

}  // namespace

// --- evaluation -------------------------------------------------------------

float RiverCarve::base_at(float wx, float wz) const {
  const int w = base_.width, h = base_.height;
  if (w <= 0 || h <= 0 || !(texel_m_ > 0.0f)) return 0.0f;
  const float fx = std::clamp(wx / texel_m_, 0.0f, static_cast<float>(w));
  const float fz = std::clamp(wz / texel_m_, 0.0f, static_cast<float>(h));
  const int i0 = std::clamp(static_cast<int>(std::floor(fx)), 0, w);
  const int j0 = std::clamp(static_cast<int>(std::floor(fz)), 0, h);
  const int i1 = std::min(i0 + 1, w);
  const int j1 = std::min(j0 + 1, h);
  const float tx = fx - static_cast<float>(i0);
  const float tz = fz - static_cast<float>(j0);
  // The mesh's node lattice is (w+1) x (h+1) and its edge nodes REPEAT the last
  // texel; reproducing that here is what keeps the carve on the rendered
  // surface rather than half a texel off it at the map border.
  auto node = [&](int i, int j) {
    return base_.at(std::min(i, w - 1), std::min(j, h - 1));
  };
  const float a = node(i0, j0) + (node(i1, j0) - node(i0, j0)) * tx;
  const float b = node(i0, j1) + (node(i1, j1) - node(i0, j1)) * tx;
  return a + (b - a) * tz;
}

RiverCarve::Station RiverCarve::station_at(const ArcRec& a,
                                           float chain_s_m) const {
  if (a.station_count == 0) return Station{};
  const float f = std::max(0.0f, chain_s_m) / kStationStepM;
  const uint32_t last = a.station_count - 1;
  const uint32_t i0 =
      std::min(static_cast<uint32_t>(f), last);  // f >= 0, so truncation floors
  const uint32_t i1 = std::min(i0 + 1, last);
  const float t = std::clamp(f - static_cast<float>(i0), 0.0f, 1.0f);
  const Station& s0 = stations_[a.station_begin + i0];
  const Station& s1 = stations_[a.station_begin + i1];
  Station out;
  out.half_m = s0.half_m + (s1.half_m - s0.half_m) * t;
  out.wetted_half_m = s0.wetted_half_m + (s1.wetted_half_m - s0.wetted_half_m) * t;
  out.cavity_m = s0.cavity_m + (s1.cavity_m - s0.cavity_m) * t;
  out.bed_m = s0.bed_m + (s1.bed_m - s0.bed_m) * t;
  return out;
}

float RiverCarve::HeightAt(float wx, float wz) const {
  const float base = base_at(wx, wz);
  if (cand_offset_.empty()) return base;
  // Which texel's SQUARE holds the query: texel (x, y) is CENTRED at
  // (x*texel, y*texel), matching rasterize_rivers.
  const int tx = static_cast<int>(std::floor(wx / texel_m_ + 0.5f));
  const int tz = static_cast<int>(std::floor(wz / texel_m_ + 0.5f));
  if (!mask.in_bounds(tx, tz)) return base;
  const size_t ti = static_cast<size_t>(tz) * static_cast<size_t>(mask.width) +
                    static_cast<size_t>(tx);

  const glm::vec2 p(wx, wz);
  float out = base;  // untouched unless some channel actually governs here
  for (uint32_t k = cand_offset_[ti]; k < cand_offset_[ti + 1]; ++k) {
    const ArcRec& rec = arcs_[cand_arc_[k]];
    const float r = arc_distance_m(rec.arc, p);
    const Station st =
        station_at(rec, rec.chain_s0_m + arc_closest_param_m(rec.arc, p));
    // COMPACT SUPPORT, and it is a hard edge on purpose: at or beyond the
    // corridor half-width this channel contributes nothing at all, so `out`
    // keeps the exact float base_at() produced.
    if (!(r < st.half_m)) continue;
    float profile = 1.0f;
    if (r > st.wetted_half_m) {
      const float span = st.half_m - st.wetted_half_m;
      if (!(span > 0.0f)) continue;
      // Raised cosine over the bank zone: 1 at the wetted edge, exactly 0 at
      // the corridor edge, with zero slope at both ends so the carve meets the
      // untouched terrain smoothly.
      const float t = std::clamp((r - st.wetted_half_m) / span, 0.0f, 1.0f);
      profile = 0.5f + 0.5f * std::cos(kPi * t);
    }
    // DEEPEST WINS. Two channels overlapping at a confluence must not carve
    // twice as deep as either -- the cavity is a shape, not a quantity.
    out = std::min(out, base + profile * (st.bed_m - base));
  }
  return out;
}

// --- build ------------------------------------------------------------------

RiverCarve build_river_carve(const RiverGraph& g,
                             const std::vector<RiverArcChain>& chains,
                             const Field2D<float>& base_height,
                             float world_size_m, float k_bank) {
  RiverCarve out;
  const int w = base_height.width, h = base_height.height;
  if (w <= 0 || h <= 0 || !(world_size_m > 0.0f)) return out;
  out.base_ = base_height;
  out.texel_m_ = world_size_m / static_cast<float>(w);
  out.mask = Field2D<uint8_t>(w, h, 0);
  out.cand_offset_.assign(out.mask.size() + 1, 0);

  // --- pass 1: stations, cross-section and the per-chain running minimum ----

  // Everything a chain contributes to the seeding pass. `own_min_m` is the
  // running minimum of the base surface over the chain ALONE; the cross-node
  // seed folds in later.
  struct ChainRec {
    uint32_t station_begin = 0, station_count = 0;
    int32_t from = -1, to = -1;
    float own_min_m = 0.0f;
  };
  std::vector<ChainRec> recs;
  std::vector<float> prefix_min;  // parallel to out.stations_

  for (const RiverArcChain& c : chains) {
    // A chain naming an edge outside the graph is SKIPPED, not dereferenced.
    // build_river_arcs drops reaches it could not fit, so `chain.edge` -- not
    // the chain's position -- is the only link back to the graph.
    if (c.edge < 0 || static_cast<size_t>(c.edge) >= g.edges.size()) continue;
    const RiverEdge& e = g.edges[static_cast<size_t>(c.edge)];
    // Through-lake edges carry no geometry on purpose -- and no per-point
    // width or depth to carve with.
    if (e.points_m.size() < 2 || c.arcs.empty()) continue;

    float total_m = 0.0f;
    for (const RiverArc& a : c.arcs) total_m += a.length_m;
    if (!(total_m > 0.0f)) continue;

    // The reach's normalized arc length, so per-point width/depth reach the arc
    // through its param0..param1 rather than through a copy on the arc.
    const std::vector<float> params = polyline_params(e.points_m);

    ChainRec rec;
    rec.from = e.from;
    rec.to = e.to;
    rec.station_begin = static_cast<uint32_t>(out.stations_.size());
    rec.station_count =
        static_cast<uint32_t>(std::ceil(total_m / kStationStepM)) + 1;

    size_t ai = 0;
    float arc_s0 = 0.0f;
    float run_min = std::numeric_limits<float>::max();
    for (uint32_t i = 0; i < rec.station_count; ++i) {
      const float t =
          std::min(static_cast<float>(i) * kStationStepM, total_m);
      while (ai + 1 < c.arcs.size() && t > arc_s0 + c.arcs[ai].length_m) {
        arc_s0 += c.arcs[ai].length_m;
        ++ai;
      }
      const RiverArc& a = c.arcs[ai];
      const float s = std::clamp(t - arc_s0, 0.0f, a.length_m);
      const float f = (a.length_m > 0.0f) ? s / a.length_m : 0.0f;
      const float u = a.param0 + (a.param1 - a.param0) * f;
      const float ch_w = std::max(0.0f, sample_at_param(params, e.width_m, u));
      const float ch_d = std::max(0.0f, sample_at_param(params, e.depth_m, u));

      RiverCarve::Station st;
      st.wetted_half_m = 0.5f * ch_w;
      st.half_m = std::max(kCorridorWidthFactor * ch_w, kMinCorridorHalfM);
      st.cavity_m =
          kBankfullFactor * ch_d + k_bank * std::pow(ch_w, kBankExponent);
      out.stations_.push_back(st);

      const glm::vec2 p = arc_point(a, s);
      run_min = std::min(run_min, out.base_at(p.x, p.y));
      prefix_min.push_back(run_min);
    }
    rec.own_min_m = run_min;

    float chain_s0 = 0.0f;
    for (const RiverArc& a : c.arcs) {
      RiverCarve::ArcRec ar;
      ar.arc = a;
      ar.chain_s0_m = chain_s0;
      ar.station_begin = rec.station_begin;
      ar.station_count = rec.station_count;
      out.arcs_.push_back(ar);
      chain_s0 += a.length_m;
    }
    recs.push_back(rec);
  }

  // --- pass 2: seed each chain's running minimum from its upstream chains ---
  //
  // The graph is a directed forest, so a single topological sweep over the
  // chains suffices. Without this a tributary would restart its running minimum
  // at the confluence and could hand the trunk a bed ABOVE the one it arrived
  // with -- a step in the channel floor at every junction.
  {
    const int32_t nn = static_cast<int32_t>(g.nodes.size());
    auto valid = [&](int32_t n) { return n >= 0 && n < nn; };
    std::vector<std::vector<uint32_t>> out_of(static_cast<size_t>(std::max(nn, 0)));
    std::vector<std::vector<uint32_t>> in_of(static_cast<size_t>(std::max(nn, 0)));
    for (uint32_t i = 0; i < recs.size(); ++i) {
      if (valid(recs[i].from)) out_of[static_cast<size_t>(recs[i].from)].push_back(i);
      if (valid(recs[i].to)) in_of[static_cast<size_t>(recs[i].to)].push_back(i);
    }
    std::vector<uint32_t> pending(recs.size(), 0);
    std::vector<float> seed(recs.size(), std::numeric_limits<float>::max());
    std::vector<uint32_t> ready;
    for (uint32_t i = 0; i < recs.size(); ++i) {
      if (valid(recs[i].from))
        pending[i] = static_cast<uint32_t>(in_of[static_cast<size_t>(recs[i].from)].size());
      if (pending[i] == 0) ready.push_back(i);
    }
    size_t head = 0;
    while (head < ready.size()) {
      const uint32_t i = ready[head++];
      const float end_m = std::min(seed[i], recs[i].own_min_m);
      if (!valid(recs[i].to)) continue;
      for (const uint32_t j : out_of[static_cast<size_t>(recs[i].to)]) {
        seed[j] = std::min(seed[j], end_m);
        if (--pending[j] == 0) ready.push_back(j);
      }
    }
    for (uint32_t i = 0; i < recs.size(); ++i) {
      const ChainRec& rec = recs[i];
      for (uint32_t k = 0; k < rec.station_count; ++k) {
        const uint32_t s = rec.station_begin + k;
        // BED FROM THE CENTRELINE: the running minimum of the base surface,
        // never the local height, so the channel floor can only ever descend.
        out.stations_[s].bed_m =
            std::min(seed[i], prefix_min[s]) - out.stations_[s].cavity_m;
      }
    }
  }

  // --- pass 3: conservative rasterization of the corridor -------------------

  std::vector<std::pair<uint32_t, uint32_t>> hits;  // (texel index, arc index)
  const float half_texel = 0.5f * out.texel_m_;
  for (uint32_t ai = 0; ai < out.arcs_.size(); ++ai) {
    const RiverCarve::ArcRec& rec = out.arcs_[ai];
    const RiverArc& a = rec.arc;
    if (!(a.length_m > 0.0f)) continue;
    const bool straight = (a.curvature_1_m == 0.0f);
    const float radius = straight ? 0.0f : arc_radius_m(a);

    // Chords short enough that the arc never sags more than kSagittaM off them:
    // for a circle of radius r a step s sags by at most s^2/(8r). A straight
    // arc IS its chord and needs one.
    const float step = straight
                           ? a.length_m
                           : std::clamp(std::sqrt(8.0f * kSagittaM * radius),
                                        kMinChordM, kMaxChordM);
    const int n = std::max(1, static_cast<int>(std::ceil(a.length_m / step)));
    const float chord_s = a.length_m / static_cast<float>(n);
    // The sag of the step ACTUALLY used, so the margin stays honest even where
    // the clamps above bit. Capped at the radius, which bounds it outright.
    const float sag =
        straight ? 0.0f
                 : std::min(chord_s * chord_s / (8.0f * radius), radius);

    for (int k = 0; k < n; ++k) {
      const float s0 = chord_s * static_cast<float>(k);
      const float s1 = (k + 1 == n) ? a.length_m : chord_s * static_cast<float>(k + 1);
      const glm::vec2 pa = arc_point(a, s0), pb = arc_point(a, s1);

      // Widest corridor anywhere under this chord. Taking the endpoints alone
      // would under-cover where a polyline knot inside the chord carries a
      // larger width.
      float half_max = kMinCorridorHalfM;
      if (rec.station_count > 0) {
        const uint32_t last = rec.station_count - 1;
        const uint32_t k0 = std::min(
            static_cast<uint32_t>(std::max(0.0f, (rec.chain_s0_m + s0) / kStationStepM)),
            last);
        const uint32_t k1 = std::min(
            static_cast<uint32_t>(std::ceil(std::max(0.0f, (rec.chain_s0_m + s1) / kStationStepM))),
            last);
        for (uint32_t q = k0; q <= k1; ++q)
          half_max = std::max(half_max, out.stations_[rec.station_begin + q].half_m);
      }
      // dist(p, arc) <= dist(p, chord) + sag, so testing the chord at
      // half + sag can only OVER-cover. Under-covering is the failure that
      // matters: HeightAt trusts the mask to have listed every arc that can
      // govern a texel.
      const float radius_m = half_max + sag;

      const int x0 = std::max(0, static_cast<int>(std::floor((std::min(pa.x, pb.x) - radius_m) / out.texel_m_)) - 1);
      const int x1 = std::min(w - 1, static_cast<int>(std::ceil((std::max(pa.x, pb.x) + radius_m) / out.texel_m_)) + 1);
      const int y0 = std::max(0, static_cast<int>(std::floor((std::min(pa.y, pb.y) - radius_m) / out.texel_m_)) - 1);
      const int y1 = std::min(h - 1, static_cast<int>(std::ceil((std::max(pa.y, pb.y) + radius_m) / out.texel_m_)) + 1);

      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          const glm::vec2 c(static_cast<float>(x) * out.texel_m_,
                            static_cast<float>(y) * out.texel_m_);
          // CONSERVATIVE: the texel's SQUARE must come within the radius, not
          // its centre. A sub-metre channel passes straight through texels no
          // centre test would ever pick up.
          if (segment_aabb_distance(pa, pb, c - half_texel, c + half_texel) > radius_m)
            continue;
          hits.emplace_back(
              static_cast<uint32_t>(static_cast<size_t>(y) * static_cast<size_t>(w) +
                                    static_cast<size_t>(x)),
              ai);
        }
      }
    }
  }

  // --- pass 4: mask + the per-texel candidate lists (CSR) -------------------

  std::sort(hits.begin(), hits.end());
  hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
  for (const auto& hit : hits) ++out.cand_offset_[hit.first + 1];
  for (size_t i = 1; i < out.cand_offset_.size(); ++i)
    out.cand_offset_[i] += out.cand_offset_[i - 1];
  out.cand_arc_.resize(hits.size());
  for (size_t i = 0; i < hits.size(); ++i) out.cand_arc_[i] = hits[i].second;
  for (const auto& hit : hits) out.mask.data[hit.first] = 1;
  return out;
}

}  // namespace badlands::mapgen
