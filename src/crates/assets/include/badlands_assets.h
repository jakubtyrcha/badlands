// C ABI for the `assets` Rust crate (src/crates/assets): format-
// autodetecting image decode (JPEG/PNG) + glTF pack texture-URI parsing.
// Linked into the badlands C++ app via Corrosion.
//
// Note: no mip generation here — mips are produced GPU-side later. Images
// are decoded to raw RGBA8 at native resolution only.
#ifndef BADLANDS_ASSETS_H
#define BADLANDS_ASSETS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A decoded JPEG image: `rgba` points at `width * height * 4` bytes
// (native resolution, no resize/mips) owned by this struct.
//
// On failure (missing/unreadable file, decode error, or an internal panic),
// `rgba` is NULL and `width`/`height` are both 0.
typedef struct BadlandsImage {
  uint8_t* rgba;
  uint32_t width;
  uint32_t height;
} BadlandsImage;

// A decoded 16-bit single-channel image: `luma` points at `width * height`
// uint16 samples (native resolution, no resize/mips) owned by this struct.
//
// On failure (missing/unreadable file, decode error, or an internal panic),
// `luma` is NULL and `width`/`height` are both 0.
typedef struct BadlandsImage16 {
  uint16_t* luma;
  uint32_t width;
  uint32_t height;
} BadlandsImage16;

// The three PBR texture URIs (relative to the glTF file's directory)
// resolved from a glTF pack's first material, by texture *URI* — not the
// glTF image `name` field, which authoring tools are known to leave
// stale/misleading.
//
// All-or-nothing: either all three fields are non-NULL malloc'd
// NUL-terminated strings, or all three are NULL. Any single URI being
// missing/unresolvable, or the whole document failing to open, NULLs all
// three fields — fields are never independently NULL.
typedef struct BadlandsGltfTextures {
  char* base_color;
  char* normal;
  char* metallic_roughness;
} BadlandsGltfTextures;

// Decode the JPEG file at `path` to raw RGBA8 at native resolution.
//
// Returns a BadlandsImage owned by the caller; free its pixel buffer with
// badlands_image_free(). On failure, `rgba` is NULL and `width`/`height`
// are both 0.
BadlandsImage badlands_decode_jpeg(const char* path);

// Decode the image file at `path` to raw RGBA8 at native resolution, with
// the format (JPEG, PNG, ...) auto-detected from file content rather than
// the path's extension. Same contract as badlands_decode_jpeg(); prefer
// this for any new caller (e.g. PNG normal/ARM maps) since it also handles
// JPEG.
//
// Returns a BadlandsImage owned by the caller; free its pixel buffer with
// badlands_image_free(). On failure, `rgba` is NULL and `width`/`height`
// are both 0.
BadlandsImage badlands_decode_image(const char* path);

// Free the pixel buffer of a BadlandsImage previously returned by
// badlands_decode_jpeg() or badlands_decode_image(). Safe to call on a
// failure result (NULL `rgba`).
void badlands_image_free(BadlandsImage image);

// Decode the image file at `path` to raw 16-bit single-channel luminance at
// native resolution, format auto-detected from file content.
//
// The 16-bit counterpart to badlands_decode_image(), for single-channel data
// where precision is the point rather than colour — the authored map's
// heightmap. badlands_decode_image() cannot serve: it decodes via RGBA8 and
// so truncates a 16-bit source to its high byte, which for a heightmap means
// ~1.1 m elevation steps and visibly terraced terrain. An 8-bit source widens
// losslessly (0..255 -> 0..65535), so this is safe on any input.
//
// Returns a BadlandsImage16 owned by the caller; free its sample buffer with
// badlands_image16_free(). On failure, `luma` is NULL and `width`/`height`
// are both 0.
BadlandsImage16 badlands_decode_image16(const char* path);

// Free the sample buffer of a BadlandsImage16 previously returned by
// badlands_decode_image16(). Safe to call on a failure result (NULL `luma`).
void badlands_image16_free(BadlandsImage16 image);

// Resolve the base color / normal / metallic-roughness texture URIs of the
// first material in the glTF document at `gltf_path`.
//
// Returns a BadlandsGltfTextures owned by the caller; the result is
// all-or-nothing (see BadlandsGltfTextures above) — on success, free each of
// the three fields with badlands_string_free().
BadlandsGltfTextures badlands_gltf_pack_textures(const char* gltf_path);

// Free a string previously returned in a BadlandsGltfTextures by
// badlands_gltf_pack_textures(). Safe to call with NULL.
void badlands_string_free(char* s);

// Write an RGBA8 image (tightly packed, `width * height * 4` bytes, no row
// padding) to a PNG file at `path`. Used by the app's `--screenshot` mode
// to dump a rendered frame for visual verification.
//
// Failures (null input, unwritable path, encode error, or an internal
// panic) are logged to stderr; there is no success/failure return value
// across the C ABI.
void badlands_write_png(const char* path, const uint8_t* rgba, uint32_t width,
                        uint32_t height);

#ifdef __cplusplus
}
#endif

#endif  // BADLANDS_ASSETS_H
