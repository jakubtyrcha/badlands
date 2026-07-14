//! Format-autodetecting image decode (JPEG/PNG) + glTF pack texture-URI
//! parsing, exposed to the C++ engine via a small C ABI.
//!
//! Ported from badlands v0.35's `src/scene/material.rs` (`decode_rgba` /
//! `load_pack_uris`), minus mip generation (mips are GPU-side in Stage 1).

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic;
use std::ptr;

/// Decode raw image bytes to RGBA8, auto-detecting the format (JPEG, PNG,
/// ...) from content signature rather than a file extension. Shared by the
/// `badlands_decode_jpeg` and `badlands_decode_image` C-ABI thunks below —
/// both now accept any format `image::load_from_memory` can sniff; the
/// "_jpeg" name reflects its original callers, not a format restriction.
fn decode_bytes(bytes: &[u8]) -> Option<(Vec<u8>, u32, u32)> {
    let img = image::load_from_memory(bytes).ok()?;
    let rgba = img.to_rgba8();
    let (w, h) = rgba.dimensions();
    Some((rgba.into_raw(), w, h))
}

/// Decode an image file at `path` to raw RGBA8 pixels at native resolution
/// (no resize, no mip generation — that's GPU-side later). Format
/// (JPEG/PNG/...) is auto-detected from file content.
///
/// Returns `(rgba_bytes, width, height)` on success, `None` on any failure
/// (file read, decode).
fn decode(path: &str) -> Option<(Vec<u8>, u32, u32)> {
    let bytes = std::fs::read(path).ok()?;
    decode_bytes(&bytes)
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
/// glTF pack's first material. All-or-nothing: either all three fields are
/// malloc'd (`CString::into_raw`) NUL-terminated strings, or all three are
/// NULL — any single URI being missing/unresolvable, or the whole document
/// failing to open, NULLs all three fields.
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

/// Decode the image file at `path` to raw RGBA8 across the C ABI, with the
/// format (JPEG, PNG, ...) auto-detected from file content. Mirrors
/// `badlands_decode_jpeg` exactly (same panic-safe wrapping, same
/// `BadlandsImage` return, same null-on-error) — the two thunks share the
/// same `decode`/`decode_bytes` implementation; `badlands_decode_image` is
/// the format-general entry point new callers (e.g. PNG normal/ARM maps)
/// should use.
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
pub unsafe extern "C" fn badlands_decode_image(path: *const c_char) -> BadlandsImage {
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
            eprintln!("badlands_decode_image: panicked");
            failed_image()
        }
    }
}

/// Free the pixel buffer of a `BadlandsImage` returned by
/// `badlands_decode_jpeg` or `badlands_decode_image`. Safe to call on a
/// failure result (NULL `rgba`).
///
/// # Safety
/// `image.rgba` must be either NULL or a pointer previously returned by
/// `badlands_decode_jpeg`/`badlands_decode_image` (with matching
/// `width`/`height`) that has not already been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn badlands_image_free(image: BadlandsImage) {
    if panic::catch_unwind(|| {
        if !image.rgba.is_null() {
            let len = (image.width as usize) * (image.height as usize) * 4;
            drop(unsafe { Vec::from_raw_parts(image.rgba, len, len) });
        }
    })
    .is_err()
    {
        eprintln!("badlands_image_free: panicked");
    }
}

/// Write an RGBA8 image to a PNG file at `path`. Used by the C++ app's
/// `--screenshot` mode (src/main.cpp) to dump a rendered frame for visual
/// verification.
///
/// Failures (null input, non-UTF8/unwritable path, encode error, or an
/// internal panic) are logged to stderr; there is no success/failure signal
/// across the C ABI (the caller has nothing actionable to do beyond
/// checking whether the file exists afterward).
///
/// # Safety
/// `path` must be a valid NUL-terminated C string. `rgba` must point at at
/// least `width * height * 4` readable bytes (tightly packed RGBA8, no row
/// padding).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn badlands_write_png(
    path: *const c_char,
    rgba: *const u8,
    width: u32,
    height: u32,
) {
    let result = panic::catch_unwind(|| {
        if path.is_null() || rgba.is_null() {
            return Err("null path or pixel buffer".to_string());
        }
        let path = unsafe { CStr::from_ptr(path) }
            .to_str()
            .map_err(|e| e.to_string())?;
        let len = (width as usize) * (height as usize) * 4;
        let pixels = unsafe { std::slice::from_raw_parts(rgba, len) };
        image::save_buffer(path, pixels, width, height, image::ColorType::Rgba8)
            .map_err(|e| e.to_string())
    });

    match result {
        Ok(Ok(())) => {}
        Ok(Err(msg)) => eprintln!("badlands_write_png: failed: {msg}"),
        Err(_) => eprintln!("badlands_write_png: panicked"),
    }
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
/// All-or-nothing (see `BadlandsGltfTextures`): on success, each of
/// `base_color`/`normal`/`metallic_roughness` is a malloc'd (via
/// `CString::into_raw`) NUL-terminated string, owned by the caller and must
/// be freed with `badlands_string_free`. If any one URI is missing or
/// unresolvable, or the whole document fails to open (invalid input, no
/// materials, or an internal panic), all three fields are NULL.
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
    if panic::catch_unwind(|| {
        if !ptr.is_null() {
            drop(unsafe { CString::from_raw(ptr) });
        }
    })
    .is_err()
    {
        eprintln!("badlands_string_free: panicked");
    }
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
    fn decode_bytes_png_roundtrip() {
        // Build a tiny 2x2 RGBA image in memory, encode it to PNG bytes
        // (via the `image` crate), and decode those bytes back through
        // `decode_bytes` — the same format-auto-detecting code path used by
        // `badlands_decode_image` (and, since M1, `badlands_decode_jpeg`).
        let mut img = image::RgbaImage::new(2, 2);
        img.put_pixel(0, 0, image::Rgba([10, 20, 30, 255]));
        img.put_pixel(1, 0, image::Rgba([40, 50, 60, 255]));
        img.put_pixel(0, 1, image::Rgba([70, 80, 90, 255]));
        img.put_pixel(1, 1, image::Rgba([100, 110, 120, 255]));

        let mut png_bytes: Vec<u8> = Vec::new();
        image::DynamicImage::ImageRgba8(img)
            .write_to(&mut std::io::Cursor::new(&mut png_bytes), image::ImageFormat::Png)
            .expect("encode png");
        // Sanity-check we actually built a PNG (not accidentally testing a
        // format the decoder would auto-detect some other way).
        assert_eq!(&png_bytes[0..8], b"\x89PNG\r\n\x1a\n");

        let (rgba, w, h) = decode_bytes(&png_bytes).expect("decode png bytes");
        assert_eq!((w, h), (2, 2));
        assert_eq!(rgba.len(), (w * h * 4) as usize);
        assert_eq!(&rgba[0..4], &[10, 20, 30, 255], "pixel (0,0)");
        assert_eq!(&rgba[12..16], &[100, 110, 120, 255], "pixel (1,1)");
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

    // The tests below exercise the actual `extern "C"` thunks (not just the
    // safe inner `decode`/`pack_uris` helpers) via `unsafe`, so the
    // allocate -> reconstruct -> drop round-trip that crosses the FFI
    // boundary is covered by `cargo test` (and, in CI configurations that
    // run it, `cargo miri`/ASan). In particular
    // `badlands_image_free`'s `Vec::from_raw_parts(rgba, w*h*4, w*h*4)`
    // relies on `image::to_rgba8().into_raw()` producing a buffer with
    // `len == capacity == width * height * 4` — behavior `image` does not
    // guarantee in its public API — so this round-trip is the signal that
    // would catch a future `image` version regressing it.

    #[test]
    fn ffi_decode_jpeg_roundtrip() {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../assets/materials/rocky_trail_1k.gltf/textures/rocky_trail_diff_1k.jpg"
        );
        let c_path = CString::new(path).unwrap();

        let image = unsafe { badlands_decode_jpeg(c_path.as_ptr()) };
        assert!(!image.rgba.is_null());
        assert_eq!((image.width, image.height), (1024, 1024));

        unsafe { badlands_image_free(image) };
    }

    #[test]
    fn ffi_decode_image_roundtrip_jpeg() {
        // badlands_decode_image auto-detects JPEG too (not just PNG).
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../assets/materials/rocky_trail_1k.gltf/textures/rocky_trail_diff_1k.jpg"
        );
        let c_path = CString::new(path).unwrap();

        let image = unsafe { badlands_decode_image(c_path.as_ptr()) };
        assert!(!image.rgba.is_null());
        assert_eq!((image.width, image.height), (1024, 1024));

        unsafe { badlands_image_free(image) };
    }

    #[test]
    fn ffi_decode_image_roundtrip_png() {
        // A real PNG normal map from an M2 material pack (weathered_planks) —
        // this is the format-autodetection case badlands_decode_image exists
        // for: JPEG-only decode would fail on this file.
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../assets/materials/weathered_planks_1k/textures/weathered_planks_nor_dx_1k.png"
        );
        let c_path = CString::new(path).unwrap();

        let image = unsafe { badlands_decode_image(c_path.as_ptr()) };
        assert!(!image.rgba.is_null());
        assert_eq!((image.width, image.height), (1024, 1024));

        unsafe { badlands_image_free(image) };
    }

    #[test]
    fn ffi_decode_image_missing_file_returns_null() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/does/not/exist.png");
        let c_path = CString::new(path).unwrap();

        let image = unsafe { badlands_decode_image(c_path.as_ptr()) };
        assert!(image.rgba.is_null());
        assert_eq!((image.width, image.height), (0, 0));

        unsafe { badlands_image_free(image) };
    }

    #[test]
    fn ffi_decode_jpeg_missing_file_returns_null() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/does/not/exist.jpg");
        let c_path = CString::new(path).unwrap();

        let image = unsafe { badlands_decode_jpeg(c_path.as_ptr()) };
        assert!(image.rgba.is_null());
        assert_eq!((image.width, image.height), (0, 0));

        // Freeing a failure result (NULL rgba) must be a safe no-op.
        unsafe { badlands_image_free(image) };
    }

    #[test]
    fn ffi_gltf_pack_textures_roundtrip() {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../assets/materials/rocky_trail_1k.gltf/rocky_trail_1k.gltf"
        );
        let c_path = CString::new(path).unwrap();

        let textures = unsafe { badlands_gltf_pack_textures(c_path.as_ptr()) };
        assert!(!textures.base_color.is_null());
        assert!(!textures.normal.is_null());
        assert!(!textures.metallic_roughness.is_null());

        let base = unsafe { CStr::from_ptr(textures.base_color) }
            .to_str()
            .unwrap();
        let normal = unsafe { CStr::from_ptr(textures.normal) }
            .to_str()
            .unwrap();
        let mr = unsafe { CStr::from_ptr(textures.metallic_roughness) }
            .to_str()
            .unwrap();
        assert!(base.contains("_diff"), "base: {base}");
        assert!(normal.contains("_nor_gl"), "normal: {normal}");
        assert!(mr.contains("_arm"), "mr: {mr}");

        unsafe {
            badlands_string_free(textures.base_color);
            badlands_string_free(textures.normal);
            badlands_string_free(textures.metallic_roughness);
        }
    }

    #[test]
    fn write_png_roundtrip() {
        let path = std::env::temp_dir().join("badlands_write_png_roundtrip_test.png");
        let path_str = path.to_str().expect("temp path is valid UTF-8");
        let c_path = CString::new(path_str).unwrap();

        // 2x2 RGBA8: red, green, blue, white.
        #[rustfmt::skip]
        let pixels: [u8; 16] = [
            255, 0,   0,   255,
            0,   255, 0,   255,
            0,   0,   255, 255,
            255, 255, 255, 255,
        ];

        unsafe { badlands_write_png(c_path.as_ptr(), pixels.as_ptr(), 2, 2) };

        let decoded = image::open(&path).expect("decode written png");
        assert_eq!((decoded.width(), decoded.height()), (2, 2));
        let rgba = decoded.to_rgba8();
        assert_eq!(rgba.get_pixel(0, 0).0, [255, 0, 0, 255]);
        assert_eq!(rgba.get_pixel(1, 1).0, [255, 255, 255, 255]);

        let _ = std::fs::remove_file(&path);
    }
}
