// C ABI for the `wesl` Rust crate (src/crates/wesl): compiles .wesl shader
// modules to WGSL. Linked into the badlands C++ app via Corrosion.
#ifndef BADLANDS_WESL_H
#define BADLANDS_WESL_H

#ifdef __cplusplus
extern "C" {
#endif

// Compile the WESL module `module_path` (e.g. "common/frame", no extension)
// found under `shader_dir` to WGSL.
//
// Returns a malloc'd NUL-terminated WGSL string on success, owned by the
// caller; free it with badlands_wesl_free(). Returns NULL on error (invalid
// UTF-8 input, a WESL compilation failure, or an internal panic).
char* badlands_wesl_compile(const char* shader_dir, const char* module_path);

// Free a string previously returned by badlands_wesl_compile(). Safe to call
// with NULL.
void badlands_wesl_free(char* wgsl);

#ifdef __cplusplus
}
#endif

#endif  // BADLANDS_WESL_H
