#pragma once

// Structs shared between MSL and C++. Layout rule: only float, float4 and
// float4x4 fields allowed. float3 is banned: MSL gives it 16-byte
// size/alignment, which silently desyncs from the host layout.

#ifdef __METAL_VERSION__
#include <metal_stdlib>
typedef metal::float4   sq_float4;
typedef metal::float4x4 sq_float4x4;
#else
#include <simd/simd.h>
typedef simd_float4     sq_float4;
typedef simd_float4x4   sq_float4x4;
#endif

typedef struct {
    sq_float4 pos;   // xyz used
    sq_float4 color;
} LineVertex;

typedef struct {
    sq_float4x4 view_proj;
} LineUniforms;
