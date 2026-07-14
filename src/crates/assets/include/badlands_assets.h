// C ABI for the `assets` Rust crate (src/crates/assets): JPEG decode + glTF
// pack texture-URI parsing. Linked into the badlands C++ app via Corrosion.
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

// The three PBR texture URIs (relative to the glTF file's directory)
// resolved from a glTF pack's first material, by texture *URI* — not the
// glTF image `name` field, which authoring tools are known to leave
// stale/misleading.
//
// Each field is a malloc'd NUL-terminated string, or NULL if missing,
// unresolvable, or the whole document failed to open.
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

// Free the pixel buffer of a BadlandsImage previously returned by
// badlands_decode_jpeg(). Safe to call on a failure result (NULL `rgba`).
void badlands_image_free(BadlandsImage image);

// Resolve the base color / normal / metallic-roughness texture URIs of the
// first material in the glTF document at `gltf_path`.
//
// Returns a BadlandsGltfTextures owned by the caller; free each non-NULL
// field with badlands_string_free().
BadlandsGltfTextures badlands_gltf_pack_textures(const char* gltf_path);

// Free a string previously returned in a BadlandsGltfTextures by
// badlands_gltf_pack_textures(). Safe to call with NULL.
void badlands_string_free(char* s);

#ifdef __cplusplus
}
#endif

#endif  // BADLANDS_ASSETS_H
