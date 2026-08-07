#pragma once

// The editor's render pipelines, built once at attach.
//
// SEVEN, where the metal-cpp path had five PSOs. Metal let each draw pick its
// depth-stencil state and primitive type independently; the RHI folds both into
// the pipeline object, so the single blend PSO becomes three -- lines and
// triangles, depth-tested and depth-ignored -- and the opaque line PSO splits
// from the mesh one. The count went up because state that used to be implicit
// at the call site is now stated where it is decided.

#include <memory>

#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

namespace sq {

struct RhiPipelines {
    // `depth` is the format of the attachment the geometry and ground passes
    // render into. The three chrome pipelines declare NO depth format, because
    // their pass has no depth attachment -- and Metal validation requires a
    // pipeline's depth format to match the pass it is used in, so "no depth" has
    // to be stated on both sides rather than left to default agreement.
    static std::unique_ptr<RhiPipelines> Create(
        badlands::rhi::IRhiDevice& device,
        badlands::slang::SlangCompiler& compiler,
        badlands::rhi::Format color,
        badlands::rhi::Format depth);

    badlands::rhi::RenderPipelinePtr raymarch;     // depth write, opaque, tris
    badlands::rhi::RenderPipelinePtr mesh;         // depth write, opaque, tris
    badlands::rhi::RenderPipelinePtr ground;       // depth read, premultiplied
    badlands::rhi::RenderPipelinePtr origin;       // depth read, straight alpha
    badlands::rhi::RenderPipelinePtr lines;        // no depth, opaque, LINES
    badlands::rhi::RenderPipelinePtr blend_lines;  // no depth, alpha, LINES
    badlands::rhi::RenderPipelinePtr blend_tris;   // no depth, alpha, tris

private:
    // Held because a pipeline does not own its modules; dropping these would
    // leave the pipelines pointing at freed shaders.
    badlands::rhi::ShaderModulePtr modules_[8];
};

} // namespace sq
