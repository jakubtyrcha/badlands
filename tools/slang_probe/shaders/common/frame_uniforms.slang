// Slang port of shaders/common/frame.wesl, for the RHI/Slang probe.
//
// Faithful in STRUCTURE and SIZE, not a production shader: the probe measures
// compile time and reflection output, so the module is ported whole (including
// helpers terrain_cluster never calls) to keep the input comparable to what the
// WESL compiler sees with stripping enabled.
//
// FrameUniforms must stay 592 bytes to match the C++ UniformData
// (static_assert(sizeof(UniformData)==592)) -- probe B checks whether Slang
// reflection reports the same member offsets naga does.
//
// Named frame_uniforms, not frame: a `module frame;` declaration collides with
// this module's own `frame` global (Slang reports an ambiguous reference). The
// WESL original has no module-name concept, so this is a real porting gotcha
// rather than a stylistic choice -- recorded in probe A's findings.
module frame_uniforms;

public struct FrameUniforms
{
    public float4x4 viewMatrix;       // Camera at origin, world offset applied
    public float4x4 projMatrix;
    public float4x4 viewMatrixPrev;   // Previous frame view (motion vectors/TAA)
    public float4x4 projMatrixPrev;
    public float4x4 lightViewProj;    // Shadow map: light view-projection
    public float4   cameraWorldPos;   // xyz = position, w unused
    public float4   sunDirection;
    public float4   sunColor;
    public float4   ambientSH[9];     // SH L2 ambient, RGB in xyz
    public float4   sphereOffset;
    public float2   jitter;
    public float2   jitterPrev;
    public float    nearPlane;
    public float    farPlane;
    public float2   screenSize;
    public uint     enableGtao;
    public uint     tonemapMode;
    public uint     outputIsLinear;
    public uint     debugFlags;
    public float4   shadowParams;
};

public ConstantBuffer<FrameUniforms> frame;

public float2 getNearFarPlanes() { return float2(frame.nearPlane, frame.farPlane); }

// ============================================================================
// Fullscreen triangle helpers
// ============================================================================

public struct FullscreenVertexOutput
{
    public float4 clipPosition;
    public float2 screenUV;
};

public FullscreenVertexOutput getFullscreenTriangleVertex(uint vertexIndex)
{
    let uv = float2(float((vertexIndex << 1u) & 2u), float(vertexIndex & 2u));
    let clipPos = float2(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0);

    FullscreenVertexOutput output;
    output.clipPosition = float4(clipPos, 0.0, 1.0);
    output.screenUV = float2(uv.x, 1.0 - uv.y);
    return output;
}

// ============================================================================
// Coordinate space helpers
// ============================================================================

public float3 worldSpaceToWorldCameraOffsetedSpace(float3 p) { return p - frame.cameraWorldPos.xyz; }
public float3 worldCameraOffsetedSpaceToWorldSpace(float3 p) { return p + frame.cameraWorldPos.xyz; }

public float4 worldCameraOffsetedSpaceToClipSpace(float3 p)
{
    return mul(frame.projMatrix, mul(frame.viewMatrix, float4(p, 1.0)));
}

public float4 worldCameraOffsetedSpaceToLightClipSpace(float3 p)
{
    let posInWorldSpace = p + frame.cameraWorldPos.xyz;
    return mul(frame.lightViewProj, float4(posInWorldSpace, 1.0));
}

public float4 worldSpaceToClipSpace(float3 p)
{
    return worldCameraOffsetedSpaceToClipSpace(worldSpaceToWorldCameraOffsetedSpace(p));
}

public float3 getViewRight()   { return float3(frame.viewMatrix[0].x, frame.viewMatrix[1].x, frame.viewMatrix[2].x); }
public float3 getViewUp()      { return float3(frame.viewMatrix[0].y, frame.viewMatrix[1].y, frame.viewMatrix[2].y); }
public float3 getViewForward() { return float3(frame.viewMatrix[0].z, frame.viewMatrix[1].z, frame.viewMatrix[2].z); }

public float3 viewSpaceToWorldSpaceDirection(float3 dirInViewSpace)
{
    let rotation = float3x3(frame.viewMatrix[0].xyz, frame.viewMatrix[1].xyz, frame.viewMatrix[2].xyz);
    return mul(transpose(rotation), dirInViewSpace);
}

public float getTanHalfFov() { return 1.0 / frame.projMatrix[1].y; }
public float getAspect()     { return frame.projMatrix[1].y / frame.projMatrix[0].x; }

public float3 getRayDirectionInViewSpace(float2 screenUV)
{
    let ndc = float2(screenUV.x * 2.0 - 1.0, (1.0 - screenUV.y) * 2.0 - 1.0);
    let tanHalfFov = getTanHalfFov();
    let aspect = getAspect();
    return float3(ndc.x * aspect * tanHalfFov, ndc.y * tanHalfFov, -1.0);
}

public float3 getRayDirectionInWorldSpace(float2 screenUV)
{
    return normalize(viewSpaceToWorldSpaceDirection(getRayDirectionInViewSpace(screenUV)));
}

// ============================================================================
// Depth reconstruction (reversed-Z: depth=1 near, depth=0 far)
// ============================================================================

public bool isSkyDepth(float depth)   { return depth == 0.0; }
public bool isValidDepth(float depth) { return depth > 0.0; }

public float reconstructLinearZ(float depth)
{
    let near = frame.nearPlane;
    let far = frame.farPlane;
    return near * far / (depth * (far - near) + near);
}

public float3 reconstructViewSpacePosition(float2 screenUV, float depth)
{
    return getRayDirectionInViewSpace(screenUV) * reconstructLinearZ(depth);
}

public float3 viewSpaceToWorldSpacePosition(float3 posInViewSpace)
{
    return worldCameraOffsetedSpaceToWorldSpace(viewSpaceToWorldSpaceDirection(posInViewSpace));
}

public float3 viewSpaceToWorldOffsetedSpacePosition(float3 posInViewSpace)
{
    return viewSpaceToWorldSpaceDirection(posInViewSpace);
}

public float3 reconstructWorldSpacePosition(float2 screenUV, float depth)
{
    return viewSpaceToWorldSpacePosition(reconstructViewSpacePosition(screenUV, depth));
}

// ============================================================================
// Projection helpers
// ============================================================================

public float2 viewSpaceToScreenUV(float3 posInViewSpace)
{
    let clip = mul(frame.projMatrix, float4(posInViewSpace, 1.0));
    let ndc = clip.xyz / clip.w;
    return float2((ndc.x + 1.0) * 0.5, (1.0 - ndc.y) * 0.5);
}

public float viewSpaceToDepth(float3 posInViewSpace)
{
    let clip = mul(frame.projMatrix, float4(posInViewSpace, 1.0));
    return clip.z / clip.w;
}

public float3 worldSpaceToViewSpaceDirection(float3 dirInWorldSpace)
{
    let rotation = float3x3(frame.viewMatrix[0].xyz, frame.viewMatrix[1].xyz, frame.viewMatrix[2].xyz);
    return mul(rotation, dirInWorldSpace);
}

// ============================================================================
// Terrain sphere / normal decode / shadow helpers
// ============================================================================

public float3 getSphereNormal(float3 posInWorldSpace)
{
    return normalize(posInWorldSpace - frame.sphereOffset.xyz);
}

public float3 decodeOctahedron(float2 oct)
{
    float3 n = float3(oct.x, oct.y, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0)
    {
        n = float3((1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0),
                   (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0),
                   n.z);
    }
    return normalize(n);
}

public float normalOffsetLength(float cosTerm, float cellSize, float cosClamp)
{
    return 1.5 * cellSize / max(cosTerm, cosClamp);
}

public float shadowNormalOffsetLength(float NdotL, float tSize)
{
    return normalOffsetLength(NdotL, tSize, 0.05);
}
