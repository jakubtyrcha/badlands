#pragma once

// The CPU mirror of shaders/slang/common/output_transform.slang.
//
// EXISTS TO BE AN ORACLE, not to be called at runtime. Every headless assertion
// in object_viewer compares a read-back texel against a CPU evaluation of the
// same rule, which is the pattern that caught the real defects in the blend and
// splat work -- a test that hardcodes an expected byte only pins the value
// someone happened to observe.
//
// Header-only and dependency-free on purpose: it must be includable from an
// app, from a test, and from anything else that needs to predict a pixel,
// without dragging in a target.
//
// KEEP IN LOCKSTEP with the Slang module. The two-sink test is what notices if
// they drift: it asserts an 8-bit and a float sink agree, and they can only
// agree through a rule both sides implement identically.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace badlands::color {

// Mirrors badlands::rhi::ColorSpace and the shader's kOutput* constants. Not
// including the RHI header here is deliberate -- this is colour science, and it
// must not need a device to be true.
enum class OutputMode : uint8_t {
  Srgb = 0,
  DisplayP3 = 1,
  ExtendedLinearP3 = 2,
};

struct Rgb {
  float r = 0, g = 0, b = 0;
};

// IEC 61966-2-1, not the 2.2 power approximation. Display P3 reuses this exact
// curve, which is why one pair of functions serves both 8-bit modes.
inline float SrgbToLinear(float c) {
  return c <= 0.04045f ? c / 12.92f
                       : std::pow(std::max(c + 0.055f, 0.0f) / 1.055f, 2.4f);
}

inline float LinearToSrgb(float c) {
  const float v = std::max(c, 0.0f);
  return v <= 0.0031308f ? v * 12.92f
                         : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

inline Rgb SrgbToLinear(Rgb c) {
  return {SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b)};
}
inline Rgb LinearToSrgb(Rgb c) {
  return {LinearToSrgb(c.r), LinearToSrgb(c.g), LinearToSrgb(c.b)};
}

// Linear sRGB primaries -> linear Display P3 primaries, from
// shaders/common/colorspace.wesl (XYZ_TO_P3 * SRGB_TO_XYZ).
//
// EACH ROW SUMS TO 1, because both spaces are D65. That is load-bearing rather
// than trivia: it is why the greyscale debug views read back as their source
// bytes under P3, and it is what a transposed copy of this matrix would break
// (the transpose's rows sum to 0.87, 1.22 and 0.91). A neutral-in/neutral-out
// assertion is therefore a complete check that the matrix is not transposed.
inline Rgb LinearSrgbToLinearP3(Rgb c) {
  return {0.8224619688f * c.r + 0.1775380312f * c.g + 0.0000000000f * c.b,
          0.0331941989f * c.r + 0.9668058012f * c.g + 0.0000000000f * c.b,
          0.0170826307f * c.r + 0.0723974406f * c.g + 0.9105199285f * c.b};
}

// `linear_display` is display-referred and LINEAR, in sRGB primaries.
inline Rgb EncodeOutput(Rgb linear_display, OutputMode mode) {
  if (mode == OutputMode::ExtendedLinearP3) {
    const Rgb p3 = LinearSrgbToLinearP3(linear_display);
    // Unclamped above 1: values over SDR white are the point of an EDR surface.
    // Only negatives are cut -- out of gamut, not bright.
    return {std::max(p3.r, 0.0f), std::max(p3.g, 0.0f), std::max(p3.b, 0.0f)};
  }
  if (mode == OutputMode::DisplayP3) {
    Rgb p3 = LinearSrgbToLinearP3(linear_display);
    p3 = {std::clamp(p3.r, 0.0f, 1.0f), std::clamp(p3.g, 0.0f, 1.0f),
          std::clamp(p3.b, 0.0f, 1.0f)};
    return LinearToSrgb(p3);
  }
  const Rgb c = {std::clamp(linear_display.r, 0.0f, 1.0f),
                 std::clamp(linear_display.g, 0.0f, 1.0f),
                 std::clamp(linear_display.b, 0.0f, 1.0f)};
  return LinearToSrgb(c);
}

// The same, for input already sRGB-encoded -- what the scene target holds,
// because that is the space its passes blended in.
inline Rgb EncodeOutputFromSrgb(Rgb srgb_encoded, OutputMode mode) {
  const Rgb clamped = {std::clamp(srgb_encoded.r, 0.0f, 1.0f),
                       std::clamp(srgb_encoded.g, 0.0f, 1.0f),
                       std::clamp(srgb_encoded.b, 0.0f, 1.0f)};
  return EncodeOutput(SrgbToLinear(clamped), mode);
}

// Linear Display P3 -> linear sRGB. The inverse of the matrix above, from the
// same source (P3_TO_SRGB in shaders/common/colorspace.wesl).
inline Rgb LinearP3ToLinearSrgb(Rgb c) {
  return {1.2249401762f * c.r - 0.2249401762f * c.g + 0.0000000000f * c.b,
          -0.0420569547f * c.r + 1.0420569547f * c.g + 0.0000000000f * c.b,
          -0.0196375545f * c.r - 0.0786360455f * c.g + 1.0982736002f * c.b};
}

// Recovers what the SCENE TARGET held from what the sink holds -- the inverse
// of EncodeOutputFromSrgb.
//
// EXISTS BECAUSE SOME CLAIMS ARE ABOUT THE SCENE, NOT THE SINK. The barycentric
// view's three weights sum to 1 in the scene target and NOT in the sink: the
// primaries matrix has rows summing to 1 (which preserves neutrals) but columns
// that do not (which is what a sum across channels depends on), and the sRGB
// curve is non-linear on top of that. Asserting the partition on sink values
// measures the transform instead of the resolve.
//
// Lossy through the 8-bit sink, so comparisons still take the +/-1 LSB tolerance.
inline Rgb DecodeSinkToScene(Rgb sink, OutputMode mode) {
  Rgb linear_p3;
  if (mode == OutputMode::ExtendedLinearP3) {
    linear_p3 = sink;  // already linear
  } else if (mode == OutputMode::DisplayP3) {
    linear_p3 = SrgbToLinear(sink);
  } else {
    // Srgb: no primaries change on the way out, so none on the way back.
    return sink;
  }
  return LinearToSrgb(LinearP3ToLinearSrgb(linear_p3));
}

inline uint8_t ToByte(float v) {
  return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// What an 8-bit sink holds for a colour authored as `srgb_encoded`, INCLUDING
// the scene target's own quantization -- the value passes through 8 bits twice
// on this path, and an oracle that quantized once would be off by an LSB in
// exactly the places a tolerance is meant to be catching real errors.
//
// Stage 4d makes the scene target float and the first quantization disappears;
// this function is where that change gets noticed.
struct Rgba8 {
  uint8_t r = 0, g = 0, b = 0, a = 255;
};

inline Rgba8 ExpectedSinkByte(Rgb authored_srgb, float alpha, OutputMode mode) {
  const Rgb quantized{float(ToByte(authored_srgb.r)) / 255.0f,
                      float(ToByte(authored_srgb.g)) / 255.0f,
                      float(ToByte(authored_srgb.b)) / 255.0f};
  const Rgb out = EncodeOutputFromSrgb(quantized, mode);
  return {ToByte(out.r), ToByte(out.g), ToByte(out.b), ToByte(alpha)};
}

}  // namespace badlands::color
