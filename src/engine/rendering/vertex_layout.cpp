// Ported verbatim from sampo's src/rendering/vertex_layout.cpp, namespace
// sampo -> badlands.
#include "engine/rendering/vertex_layout.hpp"

namespace badlands {

VertexLayoutInfo GetVertexLayoutInfo(VertexLayout layout) {
  VertexLayoutInfo info;
  info.step_mode = wgpu::VertexStepMode::Vertex;

  switch (layout) {
    case VertexLayout::kStaticMesh: {
      // pos(vec3) + color(vec3) + normal(vec3) = 9 floats = 36 bytes
      info.attributes.resize(3);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x3;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x3;
      info.attributes[2].offset = sizeof(float) * 6;
      info.attributes[2].shaderLocation = 2;

      info.stride = sizeof(float) * 9;
      break;
    }

    case VertexLayout::kTerrainMesh: {
      // pos(vec3) + color(vec3) + normal(vec3) + cell_index(u32) = 10 elements
      // = 40 bytes
      info.attributes.resize(4);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x3;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x3;
      info.attributes[2].offset = sizeof(float) * 6;
      info.attributes[2].shaderLocation = 2;

      info.attributes[3].format = wgpu::VertexFormat::Uint32;
      info.attributes[3].offset = sizeof(float) * 9;
      info.attributes[3].shaderLocation = 3;

      info.stride = sizeof(float) * 9 + sizeof(uint32_t);
      break;
    }

    case VertexLayout::kTerrainBlend: {
      // pos(vec3) + normal(vec3) + layer_indices(Uint8x4) + blend_weights
      // (Unorm8x4) = 32 bytes. The two u8x4 attributes are packed (one u32
      // each) into the last 2 float slots of the flat vertex buffer. The vertex
      // shader scatters the (layer, weight) pairs into per-layer weights, so the
      // mesh is indexable. layer_indices arrive as vec4<u32> (0..255),
      // blend_weights as vec4<f32> (0..1).
      info.attributes.resize(4);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x3;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Uint8x4;
      info.attributes[2].offset = sizeof(float) * 6;
      info.attributes[2].shaderLocation = 2;

      info.attributes[3].format = wgpu::VertexFormat::Unorm8x4;
      info.attributes[3].offset = sizeof(float) * 7;
      info.attributes[3].shaderLocation = 3;

      info.stride = sizeof(float) * 8;
      break;
    }

    case VertexLayout::kTerrainCluster: {
      // pos(vec3) + normal(vec3) + color(Unorm8x4) + meta(Uint8x4) = 32 bytes.
      // pos/normal occupy the first 6 float slots; color and meta are each a
      // u32 (one packed float slot) at offsets 24 and 28. color arrives as
      // vec4<f32> (0..1), meta as vec4<u32> (0..255, sampled @interpolate(flat)).
      info.attributes.resize(4);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x3;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Unorm8x4;
      info.attributes[2].offset = sizeof(float) * 6;
      info.attributes[2].shaderLocation = 2;

      info.attributes[3].format = wgpu::VertexFormat::Uint8x4;
      info.attributes[3].offset = sizeof(float) * 7;
      info.attributes[3].shaderLocation = 3;

      info.stride = sizeof(float) * 8;
      break;
    }

    case VertexLayout::kTexturedMesh: {
      // pos(vec3) + uv(vec2) + normal(vec3) + tangent(vec3) = 11 floats = 44
      // bytes
      info.attributes.resize(4);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x2;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x3;
      info.attributes[2].offset = sizeof(float) * 5;
      info.attributes[2].shaderLocation = 2;

      info.attributes[3].format = wgpu::VertexFormat::Float32x3;
      info.attributes[3].offset = sizeof(float) * 8;
      info.attributes[3].shaderLocation = 3;

      info.stride = sizeof(float) * 11;
      break;
    }

    case VertexLayout::kCubeMapMesh: {
      // pos(vec3) + normal(vec3) + tangent(vec3) = 9 floats = 36 bytes (no UV)
      info.attributes.resize(3);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x3;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x3;
      info.attributes[2].offset = sizeof(float) * 6;
      info.attributes[2].shaderLocation = 2;

      info.stride = sizeof(float) * 9;
      break;
    }

    case VertexLayout::kLine: {
      // pos(vec3) + color(vec3) = 6 floats = 24 bytes
      info.attributes.resize(2);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x3;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.stride = sizeof(float) * 6;
      break;
    }

    case VertexLayout::kThickLine: {
      // pos(vec3) + color(vec3) + dist_and_half_thickness(vec2) = 8 floats =
      // 32 bytes
      info.attributes.resize(3);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x3;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x2;
      info.attributes[2].offset = sizeof(float) * 6;
      info.attributes[2].shaderLocation = 2;

      info.stride = sizeof(float) * 8;
      break;
    }

    case VertexLayout::kPolygon: {
      // pos(vec3) + color(vec4) = 7 floats = 28 bytes
      info.attributes.resize(2);

      info.attributes[0].format = wgpu::VertexFormat::Float32x3;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x4;
      info.attributes[1].offset = sizeof(float) * 3;
      info.attributes[1].shaderLocation = 1;

      info.stride = sizeof(float) * 7;
      break;
    }

    case VertexLayout::kText: {
      // pos(vec2) + glyph_index(u32) = 12 bytes
      info.attributes.resize(2);

      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Uint32;
      info.attributes[1].offset = sizeof(float) * 2;
      info.attributes[1].shaderLocation = 1;

      info.stride = sizeof(float) * 2 + sizeof(uint32_t);
      break;
    }

    case VertexLayout::kTextEffect: {
      // pos(vec2) + glyph_index(u32) + bbox_min(vec2) + bbox_max(vec2) = 7
      // elements = 28 bytes
      info.attributes.resize(4);

      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Uint32;
      info.attributes[1].offset = sizeof(float) * 2;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x2;
      info.attributes[2].offset = sizeof(float) * 2 + sizeof(uint32_t);
      info.attributes[2].shaderLocation = 2;

      info.attributes[3].format = wgpu::VertexFormat::Float32x2;
      info.attributes[3].offset =
          sizeof(float) * 2 + sizeof(uint32_t) + sizeof(float) * 2;
      info.attributes[3].shaderLocation = 3;

      info.stride = sizeof(float) * 2 + sizeof(uint32_t) + sizeof(float) * 4;
      break;
    }

    case VertexLayout::kFullscreen: {
      // No vertex buffer - vertices generated in shader
      info.stride = 0;
      break;
    }

    case VertexLayout::kSplatQuad: {
      // pos_ndc(vec2) + local_uv(vec2) + src_uv(vec2) = 6 floats = 24 bytes
      info.attributes.resize(3);

      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x2;
      info.attributes[1].offset = sizeof(float) * 2;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x2;
      info.attributes[2].offset = sizeof(float) * 4;
      info.attributes[2].shaderLocation = 2;

      info.stride = sizeof(float) * 6;
      break;
    }

    case VertexLayout::kDelaunayTriangle: {
      // pos_ndc(vec2) + edge_dists(vec3) + src_uv(vec2) = 7 floats = 28 bytes
      info.attributes.resize(3);

      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x3;
      info.attributes[1].offset = sizeof(float) * 2;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x2;
      info.attributes[2].offset = sizeof(float) * 5;
      info.attributes[2].shaderLocation = 2;

      info.stride = sizeof(float) * 7;
      break;
    }

    case VertexLayout::kSplatCapsule: {
      // pos_ndc(vec2) + local_pos(vec2) + src_uv(vec2) + half_len_radius(vec2)
      // = 8 floats = 32 bytes
      info.attributes.resize(4);

      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x2;
      info.attributes[1].offset = sizeof(float) * 2;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x2;
      info.attributes[2].offset = sizeof(float) * 4;
      info.attributes[2].shaderLocation = 2;

      info.attributes[3].format = wgpu::VertexFormat::Float32x2;
      info.attributes[3].offset = sizeof(float) * 6;
      info.attributes[3].shaderLocation = 3;

      info.stride = sizeof(float) * 8;
      break;
    }

    case VertexLayout::kTextBg: {
      // pos(vec2) + color(vec4) = 6 floats = 24 bytes
      info.attributes.resize(2);

      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x4;
      info.attributes[1].offset = sizeof(float) * 2;
      info.attributes[1].shaderLocation = 1;

      info.stride = sizeof(float) * 6;
      break;
    }

    case VertexLayout::kUiVertex: {
      // pos(vec2) + uv(vec2) + color(vec4) = 8 floats = 32 bytes.
      // Screen-space game-UI quads (shaders/ui/ui.wesl): position in physical
      // pixels, uv into the R8 glyph atlas (solid rects sample its reserved
      // white texel), color in sRGB with straight alpha.
      info.attributes.resize(3);

      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Float32x2;
      info.attributes[1].offset = sizeof(float) * 2;
      info.attributes[1].shaderLocation = 1;

      info.attributes[2].format = wgpu::VertexFormat::Float32x4;
      info.attributes[2].offset = sizeof(float) * 4;
      info.attributes[2].shaderLocation = 2;

      info.stride = sizeof(float) * 8;
      break;
    }

    case VertexLayout::kPos2d: {
      // pos(vec2) = 2 floats = 8 bytes.
      // Screen-space quads for fragment-path demos (e.g. glyph_morph_demo).
      info.attributes.resize(1);
      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.stride = sizeof(float) * 2;
      break;
    }

    case VertexLayout::kPos2dSlot: {
      // pos(vec2) + slot_index(u32) = 12 bytes.
      // Per-vertex slot index steers the cross-font glyph-morph demo shader
      // into the correct (from, to) SDF pair for the tight slot quad the
      // vertex belongs to. The slot index is integer; the fragment shader
      // samples it with `@interpolate(flat)` (u32 varyings must be flat).
      info.attributes.resize(2);
      info.attributes[0].format = wgpu::VertexFormat::Float32x2;
      info.attributes[0].offset = 0;
      info.attributes[0].shaderLocation = 0;

      info.attributes[1].format = wgpu::VertexFormat::Uint32;
      info.attributes[1].offset = sizeof(float) * 2;
      info.attributes[1].shaderLocation = 1;

      info.stride = sizeof(float) * 2 + sizeof(uint32_t);
      break;
    }
  }

  return info;
}

}  // namespace badlands
