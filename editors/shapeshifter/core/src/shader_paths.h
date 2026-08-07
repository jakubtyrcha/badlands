#pragma once

// Where the editor's Slang comes from at run time, and the ONE place that
// answers it.
//
// The rule is two tiers and no blending:
//
//   1. the app bundle's Contents/Resources/shaders, chosen iff its MANIFEST
//      exists -- what a built .app carries, produced by the same build as the
//      binary reading it;
//   2. the STAGED tree baked in at compile time (build/shaders_staged), which
//      CMake keeps in step with the sources -- what ctest binaries get;
//   3. neither, which is fatal and says so.
//
// The repo's shader directory is deliberately absent from that list. Reading it
// is what let a binary compile shaders newer than itself: every pipeline
// failed, the renderer came back null, and the window went black with the
// reason on a stderr that a Finder launch discards.
//
// NEVER MERGE THE TIERS. A half-populated bundle must fail rather than quietly
// borrow the other tier's files, because "it worked on the machine that had the
// source tree" is the bug this replaces.

#include <optional>
#include <string>
#include <vector>

namespace sq {

struct ShaderLocation {
    // Passed to CreateSlangCompiler in order, resolving both `import` (module
    // search) and `#include` (preprocessor) out of one re-rooted tree.
    std::vector<std::string> search_paths;
    std::string root;
    // "bundle" or "staged", for the one line this logs on success.
    const char* tier = "";
};

// Null after logging exactly why: which tiers were considered, the absolute
// path each was looked for at, and -- on a hash mismatch -- both hashes.
//
// The MISMATCH case is the one worth stating. A tier whose MANIFEST disagrees
// with the hash baked into this binary is refused rather than used, so a stale
// pairing becomes a sentence instead of a black viewport.
std::optional<ShaderLocation> ResolveShaderLocation();

// The hash this binary was built against. Exposed for tests and for the
// diagnostic; SHAPESHIFTER_SHADER_HASH itself is generated, so nothing outside
// this TU needs to know the macro exists.
const char* BuiltShaderHash();

// Split out so a test can point it at a fixture directory. `root` is a staged
// layout root; returns the paths and tier without touching CFBundle.
std::optional<ShaderLocation> ShaderLocationAt(const std::string& root,
                                                const char* tier);

} // namespace sq
