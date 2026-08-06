#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <shared_types.h>

static_assert(sizeof(LineVertex) == 32, "LineVertex must be 32 bytes");
static_assert(sizeof(LineUniforms) == 64, "LineUniforms must be 64 bytes");
static_assert(sizeof(MeshVertex) == 32, "MeshVertex must be 32 bytes");
static_assert(sizeof(RaymarchUniforms) == 160, "RaymarchUniforms must be 160 bytes");

TEST_CASE("shared GPU struct layout") {
    CHECK(sizeof(LineVertex) == 32);
    CHECK(sizeof(LineUniforms) == 64);
    CHECK(sizeof(MeshVertex) == 32);
    CHECK(sizeof(RaymarchUniforms) == 160);
}
