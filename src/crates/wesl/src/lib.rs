//! `.wesl` -> WGSL compilation, exposed to the C++ engine via a small C ABI.
//!
//! Ported from badlands' `src/gpu/pipelines.rs::PipelineGenerator::compile_wesl`
//! (wesl 0.4's `Wesl::new(shader_dir).compile(&ModulePath{..})` API), and
//! following the FFI thunk structure of sampo's `wesl-ffi` crate (malloc'd
//! NUL-terminated C strings, `catch_unwind` around every extern "C" body).

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic;
use std::path::Path;
use std::ptr;

use wesl::{CompileOptions, ModulePath, Wesl, syntax::PathOrigin};

/// Compile the WESL module `module_path` (e.g. `"common/frame"`) found under
/// `shader_dir` to a WGSL source string.
pub fn compile_to_wgsl(shader_dir: &str, module_path: &str) -> Result<String, String> {
    let dir = Path::new(shader_dir);
    let mut wesl = Wesl::new(dir);
    // Unlike pipelines.rs::compile_wesl (which only ever compiles full
    // pipeline shaders that have their own vs_main/fs_main entry points),
    // this FFI is meant to compile *any* named module on request, including
    // entry-point-less shared/utility modules (e.g. common/frame.wesl). By
    // default wesl's dead-code stripping only keeps declarations reachable
    // from entry points, which would silently compile such modules down to
    // nothing. keep_root preserves the requested module's own top-level
    // declarations while still letting imported dependencies get normal
    // reachability-based stripping.
    wesl.set_options(CompileOptions {
        keep_root: true,
        ..Default::default()
    });
    let module = ModulePath {
        origin: PathOrigin::Absolute,
        components: module_path.split('/').map(str::to_string).collect(),
    };
    match wesl.compile(&module) {
        Ok(result) => Ok(result.to_string()),
        Err(err) => Err(format!(
            "WESL compilation of '{module_path}' (in {}) failed:\n{err}",
            dir.display()
        )),
    }
}

/// Compile a WESL module to WGSL across the C ABI.
///
/// # Safety
/// `shader_dir` and `module_path` must be valid NUL-terminated C strings.
///
/// # Returns
/// A malloc'd (via `CString::into_raw`) NUL-terminated WGSL string on
/// success, owned by the caller and must be freed with
/// `badlands_wesl_free`. Returns NULL on error (invalid input, panic, or a
/// WESL compilation failure).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn badlands_wesl_compile(
    shader_dir: *const c_char,
    module_path: *const c_char,
) -> *mut c_char {
    let result = panic::catch_unwind(|| {
        if shader_dir.is_null() || module_path.is_null() {
            return None;
        }
        let shader_dir = unsafe { CStr::from_ptr(shader_dir) }.to_str().ok()?;
        let module_path = unsafe { CStr::from_ptr(module_path) }.to_str().ok()?;
        match compile_to_wgsl(shader_dir, module_path) {
            Ok(wgsl) => CString::new(wgsl).ok(),
            Err(err) => {
                eprintln!("badlands_wesl_compile: {err}");
                None
            }
        }
    });

    match result {
        Ok(Some(cstring)) => cstring.into_raw(),
        Ok(None) => ptr::null_mut(),
        Err(_) => {
            eprintln!("badlands_wesl_compile: panicked");
            ptr::null_mut()
        }
    }
}

/// Free a string returned by `badlands_wesl_compile`.
///
/// # Safety
/// `ptr` must be either NULL or a pointer previously returned by
/// `badlands_wesl_compile` that has not already been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn badlands_wesl_free(ptr: *mut c_char) {
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
    fn compiles_frame_module() {
        // shaders/common/frame.wesl must exist relative to the repo root.
        //
        // Cargo always runs unit test binaries with the current directory
        // set to the *crate's* manifest directory (src/crates/wesl), not
        // the directory `cargo test` was invoked from (confirmed
        // empirically: a plain relative "shaders" here fails to resolve).
        // Use CARGO_MANIFEST_DIR to find the repo-root shaders/ directory
        // reliably regardless of invocation cwd.
        let shader_dir = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../shaders");
        let wgsl = compile_to_wgsl(shader_dir, "common/frame");
        assert!(wgsl.expect("compile ok").contains("struct FrameUniforms"));
    }
}
