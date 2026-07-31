#include "renderer.h"
#include <CoreGraphics/CoreGraphics.h>
#include <cassert>
#include <cstdio>

#include "camera.h"
#include "dcsdd.h" // TriangleMesh; kept out of renderer.h, see the forward declaration there
#include "lines.h"
#include "scene.h"

namespace sq {

void Renderer::attach_layer(CA::MetalLayer* layer) {
    layer_ = layer;

    device_ = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    queue_ = NS::TransferPtr(device_->newCommandQueue());

    layer_->setDevice(device_.get());
    layer_->setPixelFormat(MTL::PixelFormatRGBA16Float);
    layer_->setFramebufferOnly(true);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    layer_->setColorspace(cs);
    CGColorSpaceRelease(cs); // the layer retains it

    library_ = NS::TransferPtr(device_->newDefaultLibrary());
    if (!library_) {
        fprintf(stderr, "failed to load default Metal library\n");
        assert(false && "failed to load default Metal library");
    }

    NS::SharedPtr<MTL::Function> vertex_fn = NS::TransferPtr(
        library_->newFunction(NS::String::string("debug_line_vertex", NS::UTF8StringEncoding)));
    NS::SharedPtr<MTL::Function> fragment_fn = NS::TransferPtr(
        library_->newFunction(NS::String::string("debug_line_fragment", NS::UTF8StringEncoding)));
    if (!vertex_fn) {
        fprintf(stderr, "failed to create debug_line_vertex: missing from default.metallib\n");
        assert(false && "debug_line_vertex missing from default.metallib");
    }
    if (!fragment_fn) {
        fprintf(stderr, "failed to create debug_line_fragment: missing from default.metallib\n");
        assert(false && "debug_line_fragment missing from default.metallib");
    }

    NS::SharedPtr<MTL::RenderPipelineDescriptor> pipelineDesc =
        NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    pipelineDesc->setVertexFunction(vertex_fn.get());
    pipelineDesc->setFragmentFunction(fragment_fn.get());
    pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);
    // The wireframe pass draws depth-ignored (depth_ignore_ below), but the
    // pass itself now always has a depth attachment (the mesh pass needs
    // one) -- Metal validation requires every PSO used in that pass to
    // declare a matching depth attachment format, even the ones that ignore
    // the depth test at draw time.
    pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

    NS::Error* error = nullptr;
    line_pso_ = NS::TransferPtr(device_->newRenderPipelineState(pipelineDesc.get(), &error));
    if (!line_pso_) {
        fprintf(stderr, "failed to create line_pso_: %s\n",
                error ? error->localizedDescription()->utf8String() : "unknown error");
        assert(false && "failed to create line render pipeline state");
    }

    // Second PSO for the modify-mode gizmo: identical functions/attachment
    // format (Metal validation requires the formats to match), plus alpha
    // blending. This is the only blended draw in the renderer.
    NS::SharedPtr<MTL::RenderPipelineDescriptor> blendPipelineDesc =
        NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    blendPipelineDesc->setVertexFunction(vertex_fn.get());
    blendPipelineDesc->setFragmentFunction(fragment_fn.get());
    MTL::RenderPipelineColorAttachmentDescriptor* blendColorAttachment =
        blendPipelineDesc->colorAttachments()->object(0);
    blendColorAttachment->setPixelFormat(MTL::PixelFormatRGBA16Float);
    blendColorAttachment->setBlendingEnabled(true);
    blendColorAttachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
    blendColorAttachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    blendColorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);
    blendColorAttachment->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
    blendColorAttachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    blendColorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
    // Same reasoning as line_pso_ above: depth-ignored at draw time, but the
    // format must still match the pass's depth attachment.
    blendPipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);

    NS::Error* blend_error = nullptr;
    line_blend_pso_ = NS::TransferPtr(device_->newRenderPipelineState(blendPipelineDesc.get(), &blend_error));
    if (!line_blend_pso_) {
        fprintf(stderr, "failed to create line_blend_pso_: %s\n",
                blend_error ? blend_error->localizedDescription()->utf8String() : "unknown error");
        assert(false && "failed to create line blend render pipeline state");
    }

    // Mesh PSO: depth-tested shaded triangles, opaque (no blending), drawn
    // under the line/gizmo passes -- see render()'s draw-order comment.
    NS::SharedPtr<MTL::Function> mesh_vertex_fn = NS::TransferPtr(
        library_->newFunction(NS::String::string("mesh_vertex", NS::UTF8StringEncoding)));
    NS::SharedPtr<MTL::Function> mesh_fragment_fn = NS::TransferPtr(
        library_->newFunction(NS::String::string("mesh_fragment", NS::UTF8StringEncoding)));
    if (!mesh_vertex_fn) {
        fprintf(stderr, "failed to create mesh_vertex: missing from default.metallib\n");
        assert(false && "mesh_vertex missing from default.metallib");
    }
    if (!mesh_fragment_fn) {
        fprintf(stderr, "failed to create mesh_fragment: missing from default.metallib\n");
        assert(false && "mesh_fragment missing from default.metallib");
    }

    NS::SharedPtr<MTL::RenderPipelineDescriptor> meshPipelineDesc =
        NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    meshPipelineDesc->setVertexFunction(mesh_vertex_fn.get());
    meshPipelineDesc->setFragmentFunction(mesh_fragment_fn.get());
    meshPipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA16Float);
    meshPipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    // No blending set: RenderPipelineColorAttachmentDescriptor defaults to
    // blending disabled, matching line_pso_'s opaque draw.

    NS::Error* mesh_error = nullptr;
    mesh_pso_ = NS::TransferPtr(device_->newRenderPipelineState(meshPipelineDesc.get(), &mesh_error));
    if (!mesh_pso_) {
        fprintf(stderr, "failed to create mesh_pso_: %s\n",
                mesh_error ? mesh_error->localizedDescription()->utf8String() : "unknown error");
        assert(false && "failed to create mesh render pipeline state");
    }

    // Two depth-stencil states, both created once here (stencil unused by
    // either -- default front/backFaceStencil is nil, i.e. disabled).
    // Depth convention: the pinned projection maps near->0/far->1 (see
    // camera.cpp), so "closer" compares Less.
    NS::SharedPtr<MTL::DepthStencilDescriptor> depthTestDesc =
        NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
    depthTestDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
    depthTestDesc->setDepthWriteEnabled(true);
    depth_test_ = NS::TransferPtr(device_->newDepthStencilState(depthTestDesc.get()));

    NS::SharedPtr<MTL::DepthStencilDescriptor> depthIgnoreDesc =
        NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
    depthIgnoreDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
    depthIgnoreDesc->setDepthWriteEnabled(false);
    depth_ignore_ = NS::TransferPtr(device_->newDepthStencilState(depthIgnoreDesc.get()));
}

void Renderer::set_viewport_size(float w_pts, float h_pts, float backing_scale) {
    if (!layer_) return;

    const float w = w_pts * backing_scale;
    const float h = h_pts * backing_scale;
    if (w <= 0.0f || h <= 0.0f) return; // guard against zero/negative sizes

    layer_->setDrawableSize(CGSizeMake(w, h));
}

void Renderer::set_scene_lines_dirty() {
    scene_lines_dirty_ = true;
}

void Renderer::set_mesh(const TriangleMesh& mesh) {
    // TriangleMesh's own invariant (dcsdd.h): positions/normals are always
    // the same length (3 of each per triangle, written in lockstep by
    // reconstruct()). Asserted here since this loop indexes both by the
    // same i.
    assert(mesh.normals.size() == mesh.positions.size());

    mesh_vertex_count_ = mesh.positions.size();
    if (mesh_vertex_count_ == 0) {
        mesh_vertices_.reset(); // drop the buffer -- Metal disallows zero-length ones
        return;
    }

    std::vector<MeshVertex> vertices;
    vertices.reserve(mesh_vertex_count_);
    for (size_t i = 0; i < mesh_vertex_count_; ++i) {
        MeshVertex v;
        v.pos = (simd_float4){mesh.positions[i].x, mesh.positions[i].y, mesh.positions[i].z, 1.0f};
        v.normal = (simd_float4){mesh.normals[i].x, mesh.normals[i].y, mesh.normals[i].z, 0.0f};
        vertices.push_back(v);
    }

    // Same replace-on-update reasoning as scene_lines_ in render(): an
    // in-flight command buffer retains the old MTL::Buffer via ARC, so
    // swapping mesh_vertices_ here is safe even mid-flight.
    mesh_vertices_ = NS::TransferPtr(device_->newBuffer(
        vertices.data(), vertices.size() * sizeof(MeshVertex), MTL::ResourceStorageModeShared));
}

void Renderer::ensure_depth_texture(uint32_t width, uint32_t height) {
    if (depth_texture_ && depth_texture_width_ == width && depth_texture_height_ == height) {
        return;
    }

    NS::SharedPtr<MTL::TextureDescriptor> desc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    desc->setTextureType(MTL::TextureType2D);
    desc->setPixelFormat(MTL::PixelFormatDepth32Float);
    desc->setWidth(width);
    desc->setHeight(height);
    desc->setUsage(MTL::TextureUsageRenderTarget);
    desc->setStorageMode(MTL::StorageModePrivate);
    depth_texture_ = NS::TransferPtr(device_->newTexture(desc.get()));
    depth_texture_width_ = width;
    depth_texture_height_ = height;
}

void Renderer::set_gizmo(simd_float3 origin, simd_float3 normal, float half_extent) {
    gizmo_visible_ = true;
    gizmo_verts_.clear();
    append_tangent_frame(gizmo_verts_, origin, normal, half_extent, 12);
}

void Renderer::hide_gizmo() {
    gizmo_visible_ = false;
    gizmo_verts_.clear();
}

void Renderer::render(CA::MetalDrawable* drawable, const SceneDocument& doc, int32_t selected_id,
                       const Camera& camera) {
    if (!device_ || !layer_ || !drawable) return;

    if (scene_lines_dirty_) {
        // mesh_vertex_count_ > 0 is the wireframe policy's mesh_present flag: once
        // there's a shaded mesh to draw, the wireframe narrows to a selection
        // annotation (see build_scene_lines's doc comment in lines.h).
        std::vector<LineVertex> vertices = build_scene_lines(doc, selected_id, camera.eye, mesh_vertex_count_ > 0);
        scene_line_vertex_count_ = vertices.size();
        if (!vertices.empty()) {
            // Metal disallows ever creating a zero-length buffer, hence the guard;
            // replacing scene_lines_ here is safe even mid-flight — the in-flight
            // command buffer retains the old MTL::Buffer via ARC.
            scene_lines_ = NS::TransferPtr(device_->newBuffer(
                vertices.data(), vertices.size() * sizeof(LineVertex), MTL::ResourceStorageModeShared));
        }
        scene_lines_dirty_ = false;
    }

    // Sized off the drawable's own texture, not the last set_viewport_size
    // call: this is the single source of truth for "what size does the pass
    // need to be this frame" and sidesteps any ordering hazard against
    // set_viewport_size (attach_layer runs before the first resize callback
    // fires, but render() could in principle run before that too).
    ensure_depth_texture(static_cast<uint32_t>(drawable->texture()->width()),
                          static_cast<uint32_t>(drawable->texture()->height()));

    MTL::CommandBuffer* commandBuffer = queue_->commandBuffer();

    // alloc()->init() returns a +1 owned pointer (not autoreleased), so it is
    // wrapped in a SharedPtr rather than left to leak once per frame.
    NS::SharedPtr<MTL::RenderPassDescriptor> passDescriptor =
        NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());

    MTL::RenderPassColorAttachmentDescriptor* colorAttachment =
        passDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(drawable->texture());
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    colorAttachment->setClearColor(MTL::ClearColor(0.02, 0.02, 0.025, 1.0));

    MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = passDescriptor->depthAttachment();
    depthAttachment->setTexture(depth_texture_.get());
    depthAttachment->setLoadAction(MTL::LoadActionClear);
    depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    depthAttachment->setClearDepth(1.0);

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(passDescriptor.get());

    LineUniforms uniforms;
    uniforms.view_proj = camera.view_proj();

    // Draw order: mesh (depth-tested) first, then scene lines, then the
    // gizmo — both depth-ignored so they stay in painter's order on top of
    // the mesh and of each other, exactly as before D7.
    if (mesh_vertex_count_ > 0) {
        encoder->setRenderPipelineState(mesh_pso_.get());
        encoder->setDepthStencilState(depth_test_.get());
        encoder->setVertexBuffer(mesh_vertices_.get(), 0, 0);
        encoder->setVertexBytes(&uniforms, sizeof(LineUniforms), 1);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), mesh_vertex_count_);
    }

    encoder->setDepthStencilState(depth_ignore_.get());

    if (scene_line_vertex_count_ > 0) {
        // Buffer indices 0/1 are hardcoded to match the shader's [[buffer(0)]]/[[buffer(1)]] — no reflection.
        encoder->setRenderPipelineState(line_pso_.get());
        encoder->setVertexBuffer(scene_lines_.get(), 0, 0);
        encoder->setVertexBytes(&uniforms, sizeof(LineUniforms), 1);
        // NS::UInteger(0), not the literal 0: a bare 0 is also a null-pointer constant, which
        // makes it ambiguous against the (PrimitiveType, const Buffer*, offset) overload.
        encoder->drawPrimitives(MTL::PrimitiveTypeLine, NS::UInteger(0), scene_line_vertex_count_);
    }

    // Gizmo pass: painter's order over the opaque scene lines, semi-transparent
    // blend PSO, verts always via setVertexBytes (54 verts, never a buffer).
    if (gizmo_visible_ && !gizmo_verts_.empty()) {
        encoder->setRenderPipelineState(line_blend_pso_.get());
        encoder->setVertexBytes(gizmo_verts_.data(), gizmo_verts_.size() * sizeof(LineVertex), 0);
        encoder->setVertexBytes(&uniforms, sizeof(LineUniforms), 1);
        encoder->drawPrimitives(MTL::PrimitiveTypeLine, NS::UInteger(0), gizmo_verts_.size());
    }

    encoder->endEncoding();

    commandBuffer->presentDrawable(drawable);
    commandBuffer->commit();
}

} // namespace sq
