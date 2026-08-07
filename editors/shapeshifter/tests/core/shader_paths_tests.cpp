#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "shader_paths.h"

using namespace sq;

namespace {

namespace fs = std::filesystem;

// A staged-layout root under a unique temp directory. Only MANIFEST matters to
// resolution -- the search paths are derived from the root, not probed -- so the
// fixture writes that and the directories, and nothing else.
struct Fixture {
    fs::path root;

    explicit Fixture(const std::string& name) {
        root = fs::temp_directory_path() / ("ss_shader_paths_" + name);
        fs::remove_all(root);
        fs::create_directories(root / "slang" / "shapeshifter");
        fs::create_directories(root / "common");
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write_manifest(const std::string& hash) const {
        std::ofstream(root / "MANIFEST") << hash << "\n";
    }
};

} // namespace

TEST_CASE("shader tier: a matching manifest resolves, and yields the staged layout") {
    Fixture f("match");
    f.write_manifest(BuiltShaderHash());

    const auto loc = ShaderLocationAt(f.root.string(), "fixture");
    REQUIRE(loc.has_value());
    CHECK(std::string(loc->tier) == "fixture");
    CHECK(loc->root == f.root.string());

    // THE LAYOUT IS THE CONTRACT, and it is spelled out in two places that must
    // agree: here and SHAPESHIFTER_STAGED_INCLUDES in
    // cmake/ShapeshifterShaders.cmake, which the slangc tests compile through.
    // Entry points, then the tree root for the shared headers, then the
    // engine's common modules.
    REQUIRE(loc->search_paths.size() == 3);
    CHECK(loc->search_paths[0] == (f.root / "slang" / "shapeshifter").string());
    CHECK(loc->search_paths[1] == f.root.string());
    CHECK(loc->search_paths[2] == (f.root / "common").string());
}

TEST_CASE("shader tier: no manifest means the tier is simply absent") {
    // Not an error -- this is how a non-bundled binary declines the bundle tier
    // and falls through to the staged one.
    Fixture f("nomanifest");
    CHECK_FALSE(ShaderLocationAt(f.root.string(), "fixture").has_value());
}

TEST_CASE("shader tier: a manifest that disagrees with the binary is REFUSED") {
    // The whole point of the manifest. A tree that does not match this binary is
    // refused rather than compiled -- which is the difference between a stated
    // error and the silent black viewport this replaced.
    Fixture f("mismatch");
    f.write_manifest("0000000000000000000000000000000000000000000000000000000000000000");
    CHECK_FALSE(ShaderLocationAt(f.root.string(), "fixture").has_value());
}

TEST_CASE("shader tier: an empty or truncated manifest is refused, not trusted") {
    // A half-written file must not read as "no hash, carry on".
    Fixture f("empty");
    std::ofstream(f.root / "MANIFEST") << "";
    CHECK_FALSE(ShaderLocationAt(f.root.string(), "fixture").has_value());
}

TEST_CASE("shader tier: a directory where MANIFEST should be is not a tier") {
    // The P4 collision, in miniature: CFBundleCopyResourcesDirectoryURL returns
    // the executable's own directory for a bare binary, so resolution can be
    // pointed at essentially anything. It must reject what it cannot read as a
    // manifest rather than treating existence as agreement.
    Fixture f("dir");
    fs::create_directories(f.root / "MANIFEST");
    CHECK_FALSE(ShaderLocationAt(f.root.string(), "fixture").has_value());
}

TEST_CASE("shader tier: the real resolution finds a usable tree") {
    // The end-to-end one: under ctest this takes the staged tier, and it must
    // agree with the hash compiled into this very binary. If staging and the
    // baked header ever drift, every other suite here fails obscurely -- this
    // one says which half moved.
    const auto loc = ResolveShaderLocation();
    REQUIRE(loc.has_value());
    CHECK(std::string(loc->tier) == "staged");
    CHECK(fs::is_regular_file(fs::path(loc->root) / "MANIFEST"));
    CHECK(fs::is_regular_file(fs::path(loc->search_paths[0]) / "raymarch.slang"));
    CHECK(fs::is_regular_file(fs::path(loc->search_paths[1]) / "sdf_scene.h"));
    CHECK(fs::is_regular_file(fs::path(loc->search_paths[2]) / "output_transform.slang"));
}
