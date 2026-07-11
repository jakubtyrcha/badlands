// Shared frame uniforms - imported by all shaders
// Everything is public by default in WESL

struct FrameUniforms {
    viewMatrix: mat4x4<f32>,      // Camera at origin, world offset applied
    projMatrix: mat4x4<f32>,
    viewMatrixPrev: mat4x4<f32>,  // Previous frame view (for motion vectors/TAA)
    projMatrixPrev: mat4x4<f32>,  // Previous frame projection
    lightViewProj: mat4x4<f32>,   // Shadow map: light view-projection matrix
    cameraWorldPos: vec4<f32>,    // xyz = position, w unused (vec4 for alignment)
    sunDirection: vec4<f32>,      // xyz = direction, w unused
    sunColor: vec4<f32>,          // xyz = color, w unused
    ambientSH: array<vec4<f32>, 9>, // SH L2 (9 coefficients) ambient, RGB in xyz
    sphereOffset: vec4<f32>,      // xyz = terrain sphere center offset, w unused
    jitter: vec2<f32>,            // TAA jitter offset (0,0 until TAA enabled)
    jitterPrev: vec2<f32>,        // Previous frame jitter
    nearPlane: f32,               // Camera near plane distance
    farPlane: f32,                // Camera far plane distance
    screenSize: vec2<f32>,        // Screen width, height in pixels
    // Rendering option flags
    enableGtao: u32,              // 1 = AO enabled, 0 = disabled (use 1.0 for AO)
    tonemapMode: u32,             // TonemapMode enum (0 = kClamp)
    outputIsLinear: u32,          // 1 = RGBA16Float (linear output), 0 = sRGB
    _padding0: f32,               // was dpiScale, now unused — keep for struct alignment
}

// Get near and far plane distances
fn getNearFarPlanes() -> vec2<f32> {
    return vec2<f32>(frame.nearPlane, frame.farPlane);
}

@group(0) @binding(0) var<uniform> frame: FrameUniforms;

// ============================================================================
// Fullscreen Triangle Helpers
// ============================================================================
// Generates a fullscreen triangle from vertex index (0, 1, 2)
// Returns clip position and screen UV with consistent conventions:
// - Clip space: X=[-1,1], Y=[-1,1] (Y=-1 bottom, Y=+1 top)
// - Screen UV: X=[0,1], Y=[0,1] (Y=0 top, Y=1 bottom - standard screen convention)

struct FullscreenVertexOutput {
    clipPosition: vec4<f32>,
    screenUV: vec2<f32>,
}

fn getFullscreenTriangleVertex(vertexIndex: u32) -> FullscreenVertexOutput {
    // Generate triangle covering screen: vertices at (-1,-1), (3,-1), (-1,3)
    let uv = vec2<f32>(f32((vertexIndex << 1u) & 2u), f32(vertexIndex & 2u));
    let clipPos = vec2<f32>(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0);

    var output: FullscreenVertexOutput;
    output.clipPosition = vec4<f32>(clipPos, 0.0, 1.0);
    // Convert to screen UV convention: Y=0 at top, Y=1 at bottom
    // In clip space: uv.y=0 -> clipPos.y=-1 (bottom), uv.y=1 -> clipPos.y=+1 (top)
    // So flip Y: screenUV.y=0 should be top (clipPos.y=+1), screenUV.y=1 should be bottom
    output.screenUV = vec2<f32>(uv.x, 1.0 - uv.y);
    return output;
}

// ============================================================================
// Coordinate Space Helpers
// ============================================================================
// WorldCameraOffsetedSpace: WorldSpace with camera position subtracted (for float precision)
// WorldSpace: Original world coordinates (large values)
// ViewSpace: Camera at origin, looking down -Z
// ClipSpace: After projection matrix, ready for rasterization
// ============================================================================

// Convert from WorldSpace to WorldCameraOffsetedSpace
fn worldSpaceToWorldCameraOffsetedSpace(posInWorldSpace: vec3<f32>) -> vec3<f32> {
    return posInWorldSpace - frame.cameraWorldPos.xyz;
}

// Convert from WorldCameraOffsetedSpace to WorldSpace
fn worldCameraOffsetedSpaceToWorldSpace(posInWorldCameraOffsetedSpace: vec3<f32>) -> vec3<f32> {
    return posInWorldCameraOffsetedSpace + frame.cameraWorldPos.xyz;
}

// Transform WorldCameraOffsetedSpace position to ClipSpace
fn worldCameraOffsetedSpaceToClipSpace(posInWorldCameraOffsetedSpace: vec3<f32>) -> vec4<f32> {
    return frame.projMatrix * frame.viewMatrix * vec4<f32>(posInWorldCameraOffsetedSpace, 1.0);
}

// Transform WorldCameraOffsetedSpace position to Light ClipSpace (for shadow map rendering)
fn worldCameraOffsetedSpaceToLightClipSpace(posInWorldCameraOffsetedSpace: vec3<f32>) -> vec4<f32> {
    let posInWorldSpace = posInWorldCameraOffsetedSpace + frame.cameraWorldPos.xyz;
    return frame.lightViewProj * vec4<f32>(posInWorldSpace, 1.0);
}

// Transform WorldSpace position directly to ClipSpace (convenience function)
fn worldSpaceToClipSpace(posInWorldSpace: vec3<f32>) -> vec4<f32> {
    return worldCameraOffsetedSpaceToClipSpace(worldSpaceToWorldCameraOffsetedSpace(posInWorldSpace));
}

// Get view direction basis vectors (for skybox/fullscreen effects)
fn getViewRight() -> vec3<f32> {
    return vec3<f32>(frame.viewMatrix[0].x, frame.viewMatrix[1].x, frame.viewMatrix[2].x);
}

fn getViewUp() -> vec3<f32> {
    return vec3<f32>(frame.viewMatrix[0].y, frame.viewMatrix[1].y, frame.viewMatrix[2].y);
}

fn getViewForward() -> vec3<f32> {
    return vec3<f32>(frame.viewMatrix[0].z, frame.viewMatrix[1].z, frame.viewMatrix[2].z);
}

// Transform direction from ViewSpace to WorldSpace
fn viewSpaceToWorldSpaceDirection(dirInViewSpace: vec3<f32>) -> vec3<f32> {
    // View matrix rotation transforms world→view
    // Inverse (= transpose for orthonormal) transforms view→world
    let rotation = mat3x3<f32>(
        frame.viewMatrix[0].xyz,
        frame.viewMatrix[1].xyz,
        frame.viewMatrix[2].xyz
    );
    return transpose(rotation) * dirInViewSpace;
}

// Extract FOV and aspect from projection matrix
fn getTanHalfFov() -> f32 {
    return 1.0 / frame.projMatrix[1].y;
}

fn getAspect() -> f32 {
    return frame.projMatrix[1].y / frame.projMatrix[0].x;
}

// Get ray direction in ViewSpace from screen UV (0-1)
fn getRayDirectionInViewSpace(screenUV: vec2<f32>) -> vec3<f32> {
    let ndc = vec2<f32>(screenUV.x * 2.0 - 1.0, (1.0 - screenUV.y) * 2.0 - 1.0);
    let tanHalfFov = getTanHalfFov();
    let aspect = getAspect();
    return vec3<f32>(ndc.x * aspect * tanHalfFov, ndc.y * tanHalfFov, -1.0);
}

// Get normalized ray direction in WorldSpace from screen UV
fn getRayDirectionInWorldSpace(screenUV: vec2<f32>) -> vec3<f32> {
    let rayInViewSpace = getRayDirectionInViewSpace(screenUV);
    return normalize(viewSpaceToWorldSpaceDirection(rayInViewSpace));
}

// ============================================================================
// Depth Reconstruction Helpers
// ============================================================================

// Check if depth value represents sky/far plane (no geometry)
// In reversed-Z: depth=0 at far plane, depth=1 at near plane
fn isSkyDepth(depth: f32) -> bool {
    return depth == 0.0;
}

// Check if depth value represents valid geometry (not sky)
fn isValidDepth(depth: f32) -> bool {
    return depth > 0.0;
}

// Reconstruct linear view-space distance from reversed-Z depth buffer value
// depth=1 at near plane, depth=0 at far plane (reversed-Z)
// Returns positive distance from camera
fn reconstructLinearZ(depth: f32) -> f32 {
    let near = frame.nearPlane;
    let far = frame.farPlane;
    // Derived from reversed-Z projection: z = 2*near*far / (depth * (far - near) + 2*near)
    return 2.0 * near * far / (depth * (far - near) + 2.0 * near);
}

// Reconstruct view-space position from screen UV and depth buffer value
// Uses unnormalized ray (z=-1) scaled by linear depth
fn reconstructViewSpacePosition(screenUV: vec2<f32>, depth: f32) -> vec3<f32> {
    let linearZ = reconstructLinearZ(depth);
    // Get unnormalized ray direction (z = -1)
    let rayInViewSpace = getRayDirectionInViewSpace(screenUV);
    // Scale ray so that result.z = -linearZ
    return rayInViewSpace * linearZ;
}

// Convert view-space position to world-space position
// View matrix is pure rotation (camera at origin), so direction transform works for positions
fn viewSpaceToWorldSpacePosition(posInViewSpace: vec3<f32>) -> vec3<f32> {
    let posInWorldCameraOffseted = viewSpaceToWorldSpaceDirection(posInViewSpace);
    return worldCameraOffsetedSpaceToWorldSpace(posInWorldCameraOffseted);
}

fn viewSpaceToWorldOffsetedSpacePosition(posInViewSpace: vec3<f32>) -> vec3<f32> {
    return viewSpaceToWorldSpaceDirection(posInViewSpace);
}

// Convenience: reconstruct world position from screen UV and depth
fn reconstructWorldSpacePosition(screenUV: vec2<f32>, depth: f32) -> vec3<f32> {
    let posInViewSpace = reconstructViewSpacePosition(screenUV, depth);
    return viewSpaceToWorldSpacePosition(posInViewSpace);
}

// ============================================================================
// Projection Helpers (ViewSpace <-> Screen)
// ============================================================================

// Project view-space position to screen UV (0-1)
fn viewSpaceToScreenUV(posInViewSpace: vec3<f32>) -> vec2<f32> {
    let clip = frame.projMatrix * vec4<f32>(posInViewSpace, 1.0);
    let ndc = clip.xyz / clip.w;
    // NDC to screen UV: x: [-1,1] -> [0,1], y: [-1,1] -> [1,0] (Y flipped)
    return vec2<f32>((ndc.x + 1.0) * 0.5, (1.0 - ndc.y) * 0.5);
}

// Get depth buffer value from view-space position (for comparison during ray march)
fn viewSpaceToDepth(posInViewSpace: vec3<f32>) -> f32 {
    let clip = frame.projMatrix * vec4<f32>(posInViewSpace, 1.0);
    return clip.z / clip.w;
}

// Transform direction from WorldSpace to ViewSpace
fn worldSpaceToViewSpaceDirection(dirInWorldSpace: vec3<f32>) -> vec3<f32> {
    // View matrix rotation transforms world→view directly
    let rotation = mat3x3<f32>(
        frame.viewMatrix[0].xyz,
        frame.viewMatrix[1].xyz,
        frame.viewMatrix[2].xyz
    );
    return rotation * dirInWorldSpace;
}

// ============================================================================
// Terrain Sphere Helpers
// ============================================================================

// Compute sphere normal for a world position on the terrain sphere
// The sphere is centered at sphereOffset, so we normalize (pos - offset)
fn getSphereNormal(posInWorldSpace: vec3<f32>) -> vec3<f32> {
    return normalize(posInWorldSpace - frame.sphereOffset.xyz);
}

// ============================================================================
// Normal Encoding/Decoding Helpers
// ============================================================================

// Decode octahedron-encoded normal back to unit vector
// Input: 2D encoded value in [-1, 1] (from RG16Float texture)
// Output: normalized 3D direction in world space
fn decodeOctahedron(oct: vec2<f32>) -> vec3<f32> {
    var n = vec3<f32>(oct.x, oct.y, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) {
        n = vec3<f32>(
            (1.0 - abs(n.y)) * select(-1.0, 1.0, n.x >= 0.0),
            (1.0 - abs(n.x)) * select(-1.0, 1.0, n.y >= 0.0),
            n.z
        );
    }
    return normalize(n);
}
