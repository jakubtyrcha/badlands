#include "shader_paths.h"

#include <CoreFoundation/CoreFoundation.h>

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <spdlog/spdlog.h>
#include <os/log.h>

#include "shader_manifest.h" // GENERATED: SHAPESHIFTER_SHADER_HASH

namespace sq {
namespace {

namespace fs = std::filesystem;

// Every filesystem call takes the error_code overload. A tier that exists but
// cannot be READ -- a sandbox denial, a permission, an unmounted volume -- has
// to be "unavailable, try the next one", not an exception thrown out of startup
// through Swift. The app declares no sandbox entitlement today, so this is
// defence rather than a live case.
bool FileExists(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && !ec;
}

std::string ReadManifest(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::string hash;
    in >> hash; // one token; trailing newline is not part of it
    return hash;
}

// The bundle's Resources/shaders, or empty when there is no usable one.
//
// CFBundleCopyResourcesDirectoryURL DOES NOT report "not bundled". For a bare
// executable it hands back the executable's own directory, so "am I in a
// bundle?" is not a question this API answers -- measured, and the reason the
// caller decides by MANIFEST presence instead. A bare binary sitting next to a
// shaders/MANIFEST really does select this tier, which is why CMake stages to
// `shaders_staged` and never to `shaders`.
std::string BundleShaderRoot() {
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) return {}; // GET rule: not owned, not released
    CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle);
    if (!url) return {};
    // COPY rule: both of these are owned here, and there is no ARC in this TU.
    CFURLRef absolute = CFURLCopyAbsoluteURL(url);
    CFRelease(url);
    if (!absolute) return {};
    char path[PATH_MAX] = {0};
    const bool ok =
        CFURLGetFileSystemRepresentation(absolute, true, (UInt8*)path, PATH_MAX);
    CFRelease(absolute);
    if (!ok) return {};
    return (fs::path(path) / "shaders").string();
}

// Both channels, deliberately. spdlog is what the terminal and the test suites
// read; os_log is what survives a Finder launch, where stderr goes nowhere --
// and "the window was black and nothing said why" is precisely the failure this
// whole file exists to end.
template <typename... Args>
void ReportFatal(fmt::format_string<Args...> fmt, Args&&... args) {
    const std::string msg = fmt::format(fmt, std::forward<Args>(args)...);
    spdlog::error("shapeshifter: {}", msg);
    os_log_error(OS_LOG_DEFAULT, "shapeshifter: %{public}s", msg.c_str());
}

} // namespace

const char* BuiltShaderHash() { return SHAPESHIFTER_SHADER_HASH; }

std::optional<ShaderLocation> ShaderLocationAt(const std::string& root,
                                                const char* tier) {
    const fs::path base(root);
    const fs::path manifest = base / "MANIFEST";
    if (!FileExists(manifest)) return std::nullopt;

    const std::string found = ReadManifest(manifest);
    if (found != SHAPESHIFTER_SHADER_HASH) {
        // Refused, not used. The absolute path is the load-bearing part: a
        // phantom MANIFEST in a directory that merely resolves as Resources is
        // otherwise indistinguishable from a real one.
        ReportFatal(
            "the {} shader tree does not match this binary.\n"
            "  manifest: {}\n"
            "  tree was built from: {}\n"
            "  this binary expects: {}\n"
            "  Rebuild -- the shaders and the binary come from one build.",
            tier, manifest.string(), found.empty() ? "<unreadable>" : found,
            SHAPESHIFTER_SHADER_HASH);
        return std::nullopt;
    }

    // The staged layout, and the only place it is spelled out on this side.
    // Mirrors SHAPESHIFTER_STAGED_INCLUDES in cmake/ShapeshifterShaders.cmake.
    ShaderLocation loc;
    loc.root = base.string();
    loc.tier = tier;
    loc.search_paths = {(base / "slang" / "shapeshifter").string(), base.string(),
                        (base / "common").string()};
    return loc;
}

ShaderResolution ResolveShaderTiers() {
    const std::string bundle_root = BundleShaderRoot();
    if (!bundle_root.empty()) {
        // PRESENT-BUT-WRONG IS NOT ABSENT. A bundle whose manifest disagrees is
        // a mispaired app, and continuing from the staged tree would render it
        // perfectly on the one machine that has a staged tree -- so the stale
        // bundle becomes invisible from running the app, which is the whole
        // failure this mechanism exists to end. Only a MISSING bundle falls
        // through, which is the ordinary non-bundled case.
        if (FileExists(fs::path(bundle_root) / "MANIFEST")) {
            auto loc = ShaderLocationAt(bundle_root, "bundle");
            if (!loc) return {std::nullopt, ShaderTierProblem::BundleMismatch};
            spdlog::info("shapeshifter: shaders from the {} tier at {}",
                         loc->tier, loc->root);
            return {loc, ShaderTierProblem::None};
        }
    }

    if (auto loc = ShaderLocationAt(SHAPESHIFTER_STAGED_SHADER_DIR, "staged")) {
        spdlog::info("shapeshifter: shaders from the {} tier at {}", loc->tier,
                     loc->root);
        return {loc, ShaderTierProblem::None};
    }

    ReportFatal(
        "no usable shader tree, so the viewport will stay blank.\n"
        "  bundle tier: {}\n"
        "  staged tier: {}\n"
        "  Neither carries a MANIFEST matching this binary ({}).",
        bundle_root.empty() ? "<not bundled>" : bundle_root,
        SHAPESHIFTER_STAGED_SHADER_DIR, SHAPESHIFTER_SHADER_HASH);
    return {std::nullopt, ShaderTierProblem::NoUsableTier};
}

std::optional<ShaderLocation> ResolveShaderLocation() {
    ShaderResolution r = ResolveShaderTiers();
    if (r.problem == ShaderTierProblem::BundleMismatch) {
        ReportFatal("refusing to run against shaders this binary was not built "
                    "with. Rebuild the app.");
        // FLUSHED, THEN _Exit. spdlog buffers, and os_log has already taken its
        // copy; _Exit rather than exit because the Metal device, the layer and
        // a half-built renderer are alive here and running their destructors
        // during a launch failure is how a clear error becomes a crash report.
        spdlog::default_logger()->flush();
        std::_Exit(1);
    }
    return r.location;
}

} // namespace sq
