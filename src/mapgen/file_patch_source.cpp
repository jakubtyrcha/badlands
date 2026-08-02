#include "mapgen/file_patch_source.hpp"

#include "mapgen/erosion.hpp"
#include "mapgen/generator.hpp"
#include "mapgen/patch_io.hpp"
#include "mapgen/window_rivers.hpp"

namespace badlands::mapgen {

namespace {

// Narrowest channel that counts as a river. Below this the network is a haze of
// hairlines that tells you nothing; w = k_w*sqrt(Q) makes this Q >= 0.0036 m3/s.
constexpr float kMinRiverWidthM = 0.3f;

// Shortest headwater branch worth drawing. Leaf-only, so the network cannot be
// severed; applied repeatedly, since removing one leaf exposes the next.
constexpr float kMinRiverBranchM = 32.0f;

}  // namespace

PatchData FilePatchSource::Fetch(const PatchRequest& /*req*/) const {
  return patch_;
}

std::unique_ptr<FilePatchSource> LoadFilePatchSource(const std::string& dir,
                                                     std::string* error) {
  std::optional<PatchData> loaded = load_patch(dir, error);
  if (!loaded) return nullptr;

  const std::optional<PatchManifest> man = load_patch_manifest(dir, error);
  if (!man) return nullptr;

  auto src = std::unique_ptr<FilePatchSource>(new FilePatchSource());
  src->patch_ = std::move(*loaded);
  src->native_.origin_m = man->origin_m;
  src->native_.world_size_m = man->world_size_m;
  src->native_.resolution = man->resolution;
  src->source_ = man->source;

  // --- the temporary river derivation (see the header) -----------------------
  // Rebuilds the bridge the map view used to hold inline: route the loaded bed,
  // accumulate drainage with any boundary inflow seeded as upstream area,
  // extract the graph, then cull it.
  float runoff_m_per_yr = 1.0f;
  const std::vector<RiverInflow> inflows = load_inflows(dir, &runoff_m_per_yr);
  ErosionParams ep;
  ep.runoff_m_per_s = runoff_m_per_yr / 31557600.0f;

  // build_window_rivers still speaks MapArtifacts. It reads exactly four fields,
  // so the adapter is four assignments -- and it disappears entirely once the
  // graph arrives on disk.
  MapArtifacts art;
  art.heightmap = src->patch_.height;
  art.water_depth = src->patch_.water_depth;
  art.lake_id = src->patch_.lake_id;
  art.lakes = src->patch_.lakes;

  WindowRivers rivers = build_window_rivers(art, man->world_size_m, inflows, ep,
                                            kMinRiverWidthM, kMinRiverBranchM);
  src->patch_.rivers = std::move(rivers.graph);
  return src;
}

}  // namespace badlands::mapgen
