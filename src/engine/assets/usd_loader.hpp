#pragma once

// Reads a USD file into the neutral UsdSceneData PODs. This is the ONLY header
// in the engine that pulls in tinyusdz, and it does not expose a single
// tinyusdz type -- consumers include usd_scene.hpp and never learn what parsed
// the file.
//
// Layering: this and usd_scene.hpp build into `badlands_usd_lib`, which links
// tinyusdz/glm/spdlog and deliberately NOT SDL3, Dawn or badlands_engine. That
// is what lets badlands_usd_tests exercise the parser without a GPU.

#include <string>

#include "engine/assets/usd_scene.hpp"

namespace badlands {

// Loads the .usdc at `path` and converts it to render-ready meshes.
//
// The heavy lifting is tinyusdz's Tydra layer, configured to triangulate
// (earcut), split face-varying attributes into single-indexable vertices,
// compute smooth normals when the file has none, and compute tangents from
// the primary UV set. Tangents are requested unconditionally rather than only
// for meshes with a bound normal map, since the material a prop actually
// renders with comes from its material.json and is invisible here.
//
// Returns a UsdSceneData whose `ok` is false (after logging via spdlog) if the
// file is missing, unparseable, or converts to no meshes at all. Meshes that
// individually fail to convert are skipped and logged; they do not fail the
// whole load.
UsdSceneData LoadUsdScene(const std::string& path);

}  // namespace badlands
