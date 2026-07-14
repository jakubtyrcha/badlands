//! JPEG decode + glTF pack texture-URI parsing, exposed to the C++ engine
//! via a small C ABI.
//!
//! Ported from badlands v0.35's `src/scene/material.rs` (`decode_rgba` /
//! `load_pack_uris`), minus mip generation (mips are GPU-side in Stage 1).

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic;
use std::ptr;

/// Decode a JPEG file at `path` to raw RGBA8 pixels at native resolution
/// (no resize, no mip generation — that's GPU-side later).
///
/// Returns `(rgba_bytes, width, height)` on success, `None` on any failure
/// (file read, JPEG decode).
fn decode(path: &str) -> Option<(Vec<u8>, u32, u32)> {
    let bytes = std::fs::read(path).ok()?;
    let img = image::load_from_memory_with_format(&bytes, image::ImageFormat::Jpeg).ok()?;
    let rgba = img.to_rgba8();
    let (w, h) = rgba.dimensions();
    Some((rgba.into_raw(), w, h))
}

/// Resolve the texture URI referenced by a glTF `Texture`, if any. Only
/// `Source::Uri` images are supported (external files) — embedded
/// `Source::View` (glb-bufferview) images have no URI to resolve.
fn tex_uri(tex: Option<gltf::Texture>) -> Option<String> {
    match tex?.source().source() {
        gltf::image::Source::Uri { uri, .. } => Some(uri.to_string()),
        gltf::image::Source::View { .. } => None,
    }
}

/// Resolve the base color / normal / metallic-roughness texture URIs of the
/// first material in the glTF document at `path`.
///
/// Resolution is by texture URI, not the glTF image `name` field — pack
/// authoring tools are known to leave stale/misleading `name`s behind.
fn pack_uris(path: &str) -> Option<(String, String, String)> {
    let g = gltf::Gltf::open(path).ok()?;
    let m = g.materials().next()?;
    let pbr = m.pbr_metallic_roughness();
    let base = tex_uri(pbr.base_color_texture().map(|i| i.texture()))?;
    let normal = tex_uri(m.normal_texture().map(|n| n.texture()))?;
    let mr = tex_uri(pbr.metallic_roughness_texture().map(|i| i.texture()))?;
    Some((base, normal, mr))
}

/// C-ABI-compatible decoded image: `rgba` points at `width * height * 4`
/// bytes owned by this struct. On failure `rgba` is NULL and
/// `width`/`height` are both 0.
#[repr(C)]
pub struct BadlandsImage {
    pub rgba: *mut u8,
    pub width: u32,
    pub height: u32,
}

/// C-ABI-compatible bundle of the three PBR texture URIs resolved from a
/// glTF pack's first material. Each field is a malloc'd (`CString::into_raw`)
/// NUL-terminated string, or NULL if missing/unresolvable.
#[repr(C)]
pub struct BadlandsGltfTextures {
    pub base_color: *mut c_char,
    pub normal: *mut c_char,
    pub metallic_roughness: *mut c_char,
}

fn failed_image() -> BadlandsImage {
    BadlandsImage {
        rgba: ptr::null_mut(),
        width: 0,
        height: 0,
    }
}

/// Decode the JPEG file at `path` to raw RGBA8 across the C ABI.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
///
/// # Returns
/// On success, `rgba` points at a malloc'd (via `Vec::into_raw_parts`)
/// buffer of `width * height * 4` bytes, owned by the caller and must be
/// freed with `badlands_image_free`. On failure (invalid input, missing
/// file, decode error, or an internal panic), `rgba` is NULL and
/// `width`/`height` are both 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn badlands_decode_jpeg(path: *const c_char) -> BadlandsImage {
    let result = panic::catch_unwind(|| {
        if path.is_null() {
            return None;
        }
        let path = unsafe { CStr::from_ptr(path) }.to_str().ok()?;
        decode(path)
    });

    match result {
        Ok(Some((rgba, width, height))) => {
            // Leak the Vec's buffer to the caller; badlands_image_free
            // reconstructs it via Vec::from_raw_parts using the same
            // length/capacity (rgba8 buffers have no spare capacity, so
            // len == capacity == width * height * 4).
            let mut rgba = std::mem::ManuallyDrop::new(rgba);
            let ptr = rgba.as_mut_ptr();
            BadlandsImage { rgba: ptr, width, height }
        }
        Ok(None) => failed_image(),
        Err(_) => {
            eprintln!("badlands_decode_jpeg: panicked");
            failed_image()
        }
    }
}

/// Free the pixel buffer of a `BadlandsImage` returned by
/// `badlands_decode_jpeg`. Safe to call on a failure result (NULL `rgba`).
///
/// # Safety
/// `image.rgba` must be either NULL or a pointer previously returned by
/// `badlands_decode_jpeg` (with matching `width`/`height`) that has not
/// already been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn badlands_image_free(image: BadlandsImage) {
    let _ = panic::catch_unwind(|| {
        if !image.rgba.is_null() {
            let len = (image.width as usize) * (image.height as usize) * 4;
            drop(unsafe { Vec::from_raw_parts(image.rgba, len, len) });
        }
    });
}

fn opt_string_to_raw(s: Option<String>) -> *mut c_char {
    s.and_then(|s| CString::new(s).ok())
        .map(CString::into_raw)
        .unwrap_or(ptr::null_mut())
}

/// Resolve the base color / normal / metallic-roughness texture URIs of a
/// glTF pack's first material across the C ABI.
///
/// # Safety
/// `gltf_path` must be a valid NUL-terminated C string.
///
/// # Returns
/// Each of `base_color`/`normal`/`metallic_roughness` is a malloc'd (via
/// `CString::into_raw`) NUL-terminated string on success, owned by the
/// caller and must be freed with `badlands_string_free`; NULL if missing,
/// unresolvable, or the whole document fails to open (invalid input, no
/// materials, or an internal panic).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn badlands_gltf_pack_textures(
    gltf_path: *const c_char,
) -> BadlandsGltfTextures {
    let result = panic::catch_unwind(|| {
        if gltf_path.is_null() {
            return None;
        }
        let gltf_path = unsafe { CStr::from_ptr(gltf_path) }.to_str().ok()?;
        pack_uris(gltf_path)
    });

    let (base, normal, mr) = match result {
        Ok(Some((base, normal, mr))) => (Some(base), Some(normal), Some(mr)),
        Ok(None) => (None, None, None),
        Err(_) => {
            eprintln!("badlands_gltf_pack_textures: panicked");
            (None, None, None)
        }
    };

    BadlandsGltfTextures {
        base_color: opt_string_to_raw(base),
        normal: opt_string_to_raw(normal),
        metallic_roughness: opt_string_to_raw(mr),
    }
}

/// Free a string previously returned in a `BadlandsGltfTextures` by
/// `badlands_gltf_pack_textures`. Safe to call with NULL.
///
/// # Safety
/// `ptr` must be either NULL or a pointer previously returned by
/// `badlands_gltf_pack_textures` that has not already been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn badlands_string_free(ptr: *mut c_char) {
    let _ = panic::catch_unwind(|| {
        if !ptr.is_null() {
            drop(unsafe { CString::from_raw(ptr) });
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decode_returns_native_size() {
        // Cargo always runs unit test binaries with the current directory
        // set to the *crate's* manifest directory (src/crates/assets), not
        // the directory `cargo test` was invoked from. Use
        // CARGO_MANIFEST_DIR to find the repo-root assets/ directory
        // reliably regardless of invocation cwd (same trick as
        // src/crates/wesl/src/lib.rs).
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../assets/materials/rocky_trail_1k.gltf/textures/rocky_trail_diff_1k.jpg"
        );
        let (rgba, w, h) = decode(path).expect("decode ok");
        assert_eq!((w, h), (1024, 1024));
        assert_eq!(rgba.len(), (w * h * 4) as usize);
    }

    #[test]
    fn pack_uris_resolves_base_normal_mr() {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../assets/materials/rocky_trail_1k.gltf/rocky_trail_1k.gltf"
        );
        let (base, normal, mr) = pack_uris(path).expect("pack uris ok");
        assert!(base.contains("_diff"), "base: {base}");
        assert!(normal.contains("_nor_gl"), "normal: {normal}");
        assert!(mr.contains("_arm"), "mr: {mr}");
    }
}
