// Shared visibility-buffer types and packing. NO RESOURCE DECLARATIONS.
//
// Split out of visbuffer.slang because a module's globals come across with an
// import: with `StructuredBuffer<MeshVertex> vertices` living beside these
// types, the resolve's own `vertices` binding became an ambiguous reference to
// two different buffers. Types and packing are shared; a binding belongs to
// whoever binds it.
//
// The same shape as shaders/slang/rhi_lab/lab_common.slang, and for the same
// reason.

module vis_common;

// Mirrors LineFrameUniform's sibling in plane_mesh.hpp / visbuffer_pass.hpp.
public struct VisFrame {
    public float4x4 view_proj;
    public float4 camera_pos;   // xyz world, w unused
}

// Three float4s, 48 bytes. A float3 pads to 16 in MSL, so the obvious layout is
// 40 bytes on the CPU and 64 here -- every vertex after the first read from the
// wrong offset, which shears the mesh rather than failing.
//
// Mirrors MeshVertex in mesh_types.hpp. Named for the thing rather than for its
// first caller, now that the sphere grid uses it too.
public struct MeshVertex {
    public float4 pos_nx;    // xyz position, w normal.x
    public float4 nyz_uv;    // xy normal.yz, zw uv
    public float4 tangent;   // xyz tangent, w bitangent sign

    public property float3 position { get { return pos_nx.xyz; } }
    public property float3 normal {
        get { return float3(pos_nx.w, nyz_uv.x, nyz_uv.y); }
    }
    public property float2 uv { get { return nyz_uv.zw; } }
}

// One INSTANCE, indexed by the draw slot the visibility buffer packs. Mirrors
// DrawInfo in mesh_types.hpp -- 48 bytes, three 16-byte rows.
public struct DrawInfo {
    public uint first_index;
    public uint first_vertex;
    public uint material_slot;
    public uint override_mask;
    public float4 offset;    // xyz world offset for this instance
    public float4 material;  // x roughness, y metallic
}

// Which fields of DrawInfo::material the resolve should believe.
//
// AN EXPLICIT MASK, NOT A SENTINEL: "negative roughness means use the map"
// would make one value mean two things, and would reinterpret a legitimately
// zero roughness. The plane clears the mask and reads the pack instead, so
// neither field is accepted-and-ignored.
public static const uint kOverrideRoughness = 1u;
public static const uint kOverrideMetallic = 2u;

// --- The packing ------------------------------------------------------------
//
// (draw_slot << 24) | (primitive + 1). Eight bits of draw, twenty-four of
// primitive, and 0 reserved for "nothing here" -- which is why the primitive is
// stored one-based.
public static const uint kVisPrimBits = 24u;
public static const uint kVisPrimMask = (1u << kVisPrimBits) - 1u;
public static const uint kVisEmpty = 0u;

// MASKED, and the CPU refuses a mesh that would need the mask (see
// ValidatePrimitiveCount). Unmasked, a primitive past 2^24-1 overflows into the
// draw bits and the resolve fetches a wildly out-of-bounds DrawInfo -- so the
// mask is the memory-safety backstop. It is not the error report: a mask alone
// would contain the damage silently, and the CPU is the only side that knows
// the triangle count.
public uint PackVis(uint draw_slot, uint primitive) {
    return (draw_slot << kVisPrimBits) | ((primitive + 1u) & kVisPrimMask);
}
public uint VisDraw(uint packed) { return packed >> kVisPrimBits; }
// One-based on the way in, so zero-based on the way out. A caller that forgets
// this indexes every triangle off by one, which looks like a subtly wrong
// surface rather than like a bug.
public uint VisPrimitive(uint packed) { return (packed & kVisPrimMask) - 1u; }
