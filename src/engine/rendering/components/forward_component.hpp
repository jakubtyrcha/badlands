#pragma once

// Ported from sampo's src/rendering/components/forward_component.hpp,
// namespace sampo -> badlands.
//
// Deviation: also carries `MapRenderable`, sampo's tag for entities routed
// to the deferred/G-buffer pass (`SceneGraph::ProcessAttachment`'s default
// case for `MeshAttachment` with `MaterialPassType::kDeferred`). In sampo
// that tag lives in `rendering/components/material_components.hpp` alongside
// `MaterialComponent` (which carries a `sampo::RenderPhase` field). Neither
// `MaterialComponent` nor `RenderPhase` is ported here — D1 already excluded
// that whole legacy RenderPhase-keyed material-registration system as
// orthogonal to the (geometry, pass)-based `MaterialInstanceFactory` path
// (see task-D1-report.md). Since `MapRenderable` itself is a zero-field tag
// with no dependency on that system, and it's exactly the third
// pass-routing tag alongside `ForwardOpaqueRenderable`/
// `ForwardTransparentRenderable` below, it's folded into this file instead
// of porting a near-empty extra header just to carry one unused-here struct.

namespace badlands {

// Tag for entities rendered in forward opaque pass (after deferred lighting).
// Forward materials bypass the G-buffer and write color directly.
// The material is responsible for any lighting computation.
struct ForwardOpaqueRenderable {};

// Tag for entities rendered in forward transparent pass.
// Uses alpha blending with depth read-only (no depth write).
struct ForwardTransparentRenderable {};

// Tag for entities rendered in the deferred/G-buffer pass (default
// MeshAttachment routing). See deviation note above.
struct MapRenderable {};

}  // namespace badlands
