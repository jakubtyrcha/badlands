// object_viewer -- the next iteration of badlands_viewer, on the native RHI and
// the render graph instead of Dawn.
//
// Stage 1: an empty screen. That is not a warm-up -- it is the smallest program
// that pins the graph's hardest interface, because windowed and headless differ
// ONLY in which texture gets imported as the graph's output:
//
//   windowed   swapchain->Acquire() -> the drawable's texture
//   headless   a plain texture, read back and written to a PNG
//
// Everything else -- the passes, the graph, the recording -- is the same code
// down to the line. A headless run is therefore not a separate path that can
// rot; it is the real one, with a different sink.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "badlands_assets.h"
#include "core/color/output_transform.hpp"
#include "engine/app/rhi_app_shell.hpp"
#include "engine/graph/render_graph.hpp"
#include "engine/rendering/debug_line_buffer.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"
#include "engine/ui/imgui_impl_rhi.hpp"
#include "executables/object_viewer/line_pass.hpp"
#include "executables/object_viewer/output_pass.hpp"
#include "executables/object_viewer/material_pack.hpp"
#include "executables/object_viewer/plane_mesh.hpp"
#include "executables/object_viewer/sphere_grid.hpp"
#include "engine/ibl/environment.hpp"
#include "engine/ibl/prefiltered_cube.hpp"
#include "executables/object_viewer/resolve_pass.hpp"
#include "executables/object_viewer/shading_cpu.hpp"
#include "executables/object_viewer/visbuffer_pass.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

using namespace badlands;
using namespace badlands::rhi;
using badlands::graph::RasterContext;
using badlands::graph::RenderGraph;
using badlands::object_viewer::LinePass;
using badlands::object_viewer::OutputPass;
using badlands::object_viewer::SceneMesh;
using badlands::object_viewer::BuildPlaneMesh;
using badlands::object_viewer::BuildSphereGrid;
using badlands::object_viewer::VisbufferPass;
using badlands::object_viewer::ResolvePass;
using badlands::object_viewer::DebugView;
using badlands::object_viewer::DebugViews;
using badlands::object_viewer::DebugViewFromName;
using badlands::object_viewer::LoadMaterialPack;
using badlands::object_viewer::MaterialPack;
using badlands::object_viewer::SunSettings;
using badlands::object_viewer::MakeConstantPack;
using badlands::object_viewer::MakeCheckerPack;
using badlands::color::OutputMode;
namespace cpu = badlands::object_viewer::cpu;
namespace ibl = badlands::ibl;

namespace {

// What the frame contains. A selector rather than a flag because each scene
// carries its OWN pixel assertion -- "every texel is the clear colour" and "a
// segment covers these texels and not those" are different claims, and a
// headless run that could not say which it was checking would be checking
// neither.
enum class Scene { Clear, Lines, Grid, Plane, Spheres };

struct Options {
  bool headless = false;
  // Pushes a synthetic Escape mid-run while OnEvent consumes EVERY event, and
  // fails unless the loop stopped anyway. The only way to test that Escape
  // survives an ImGui panel holding keyboard focus, which is what broke it.
  bool self_test_escape = false;
  Scene scene = Scene::Clear;
  // Empty means the procedural sky. A PATH is a Radiance .hdr, and a file that
  // cannot be decoded is a refusal rather than a silent fall back to the sky.
  std::string env_path;
  bool env_none = false;
  float env_intensity = 1.0f;
  uint32_t width = 1280;
  uint32_t height = 720;
  std::string out = "object_viewer.png";
  uint64_t max_frames = 0;  // 0 = until quit
  // The clear colour, so a headless run has something falsifiable to assert.
  // Authored in ENCODED sRGB, like every other colour in this app -- the scene
  // target is the space the passes blend in.
  float clear[4] = {0.05f, 0.06f, 0.09f, 1.0f};

  // What the surface is presented in, and therefore what the output pass
  // converts to. DisplayP3 by default, matching badlands_game on the same
  // display; `edr` additionally makes the sink RGBA16Float, which is how the
  // two-sink test reaches the extended-range path with no HDR display.
  rhi::ColorSpace present = rhi::ColorSpace::DisplayP3;
  // Whether --present was actually given. Windowed defaults to "upgrade to EDR
  // if the display has it", which no single ColorSpace value expresses -- and
  // headless cannot default to that, because `edr` also makes the sink a float
  // texture. So the two defaults differ, and the flag overrides both.
  bool present_explicit = false;

  // Renders the frame into an 8-bit sink AND a float one, then asserts they
  // agree through the transform. The whole HDR/LDR rule in one check, and the
  // only one that can fail for the right reason on an SDR machine.
  bool self_test_output = false;

  // Which debug view the resolve substitutes for the lit result. Reachable from
  // the CLI as well as from the Graphics debug window, so every mode the UI can
  // select has a headless assertion behind it.
  DebugView view = DebugView::Lit;
  std::string pack = "assets/materials/aerial_rocks_01_1k";
  // Whether --pack was actually given. The headless view checks default to a
  // SYNTHETIC pack, and silently overriding an explicit --pack would be a flag
  // accepted and ignored -- the trap rule 4 exists for.
  bool pack_explicit = false;

  // Puts the camera close enough to the plane that a triangle straddles the
  // near plane. The barycentric derivation divides by clip w, so a vertex
  // behind the eye makes its screen-space edge functions meaningless rather
  // than merely imprecise -- the highest-risk case in this stage, and the one
  // that decides whether the formulation has to change.
  bool near_plane_camera = false;

  // 4c's assertion, kept reachable on its own: the R32Uint target read back and
  // compared against a CPU ray/plane oracle, with NO resolve involved. A stage
  // checkable only through the next stage is not a stage.
  bool self_test_visbuffer = false;

  // Are the analytic UV gradients actually selecting mips? Two-sided, with a
  // checkerboard: the far band must be smooth and the near band sharp.
  bool self_test_gradients = false;

  // Does a value above SDR white survive to the surface? The point of the
  // float scene target, and invisible in every other assertion.
  bool self_test_hdr = false;

  // Do consecutive frames write their uniforms to distinct bytes? The GPU
  // read/write race is invisible in an image, so it is asserted structurally.
  bool self_test_frame_ring = false;

  // TEST SCAFFOLDING, not a scene. Draws the debug grid into the overlay ON TOP
  // of the lit plane, which no user-facing scene does -- --scene plane is
  // deliberately plane-only, and SceneHasLines keeps it that way for both the
  // windowed and the headless path.
  //
  // The HDR self-test needs the one combination nothing else produces: a
  // SCENE-REFERRED image above SDR white with a PARTIALLY-covering overlay over
  // it. Without both, the composite's headroom behaviour cannot be observed.
  bool force_overlay = false;

  // Sun intensity, so the lit oracle can turn the sun OFF and isolate the SH
  // ambient. Not a UI knob: with the sun at its default the ambient is a small
  // fraction of the result, and a wrong SH convention moved the centre texel by
  // exactly one LSB -- inside the tolerance, so the check could not see it.
  float sun_intensity = SunSettings{}.intensity;
};

// The sink an output mode implies. One flag rather than two, because a float
// sink presented as sRGB and an 8-bit sink presented as extended-linear are
// both nonsense -- and a pair of flags that can disagree is a pair of flags
// that will (rule 5).
rhi::Format SinkFormatFor(rhi::ColorSpace mode) {
  return mode == rhi::ColorSpace::ExtendedLinearDisplayP3
             ? Format::RGBA16Float
             : Format::RGBA8Unorm;
}

OutputMode ToCpuMode(rhi::ColorSpace c) { return OutputMode(uint8_t(c)); }

// Parses an unsigned argument, refusing anything that would truncate or wrap.
// The lab learned this the hard way: `--width 4294967297` silently became 1 and
// rendered a valid-looking image at the wrong size with exit status 0.
bool ParseU32(const char* text, uint32_t min, uint32_t max, uint32_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || v < min || v > max) return false;
  out = uint32_t(v);
  return true;
}

// Declared here because ParseArgs reconciles --debug-view against it, and its
// definition sits with the other scene predicates further down.
bool SceneUsesVisbuffer(Scene scene);

bool ParseArgs(int argc, char** argv, Options& opt) {
  // RECORDED, NOT APPLIED, and reconciled after the loop.
  //
  // These flags used to write opt.scene as they were parsed, so a later
  // --scene silently discarded them: `--debug-view normal --scene lines`
  // rendered and asserted the LINE oracle and exited 0, having checked
  // something the caller never asked for. A flag that is accepted and then
  // dropped is exactly the delayed-fuse trap rule 4 exists for.
  bool view_given = false;
  bool scene_given = false;
  bool implies_plane = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](const char*& v) {
      if (i + 1 >= argc) return false;
      v = argv[++i];
      return true;
    };
    const char* v = nullptr;
    if (a == "--headless") {
      opt.headless = true;
    } else if (a == "--self-test-escape") {
      opt.self_test_escape = true;
    } else if (a == "--self-test-output") {
      opt.self_test_output = true;
      opt.headless = true;
    } else if (a == "--present") {
      if (!value(v)) return false;
      if (std::strcmp(v, "srgb") == 0) opt.present = rhi::ColorSpace::Srgb;
      else if (std::strcmp(v, "p3") == 0) opt.present = rhi::ColorSpace::DisplayP3;
      else if (std::strcmp(v, "edr") == 0) {
        opt.present = rhi::ColorSpace::ExtendedLinearDisplayP3;
      } else {
        spdlog::error("object_viewer: unknown present mode '{}' (srgb|p3|edr)",
                      v);
        return false;
      }
      opt.present_explicit = true;
    } else if (a == "--scene") {
      if (!value(v)) return false;
      scene_given = true;
      if (std::strcmp(v, "clear") == 0) opt.scene = Scene::Clear;
      else if (std::strcmp(v, "lines") == 0) opt.scene = Scene::Lines;
      else if (std::strcmp(v, "grid") == 0) opt.scene = Scene::Grid;
      else if (std::strcmp(v, "plane") == 0) opt.scene = Scene::Plane;
      else if (std::strcmp(v, "spheres") == 0) opt.scene = Scene::Spheres;
      else {
        spdlog::error(
            "object_viewer: unknown scene '{}' (clear|lines|grid|plane)", v);
        return false;
      }
    } else if (a == "--debug-view") {
      if (!value(v)) return false;
      opt.view = DebugViewFromName(v);
      if (opt.view == DebugView::kCount) {
        std::string names;
        for (const auto& info : DebugViews()) {
          if (!names.empty()) names += "|";
          names += info.cli;
        }
        spdlog::error("object_viewer: unknown debug view '{}' ({})", v, names);
        return false;
      }
      view_given = true;
    } else if (a == "--pack") {
      if (!value(v)) return false;
      opt.pack = v;
      opt.pack_explicit = true;
    } else if (a == "--env") {
      if (!value(v)) return false;
      // "none" is a MODE, not a filename. The direct-lighting oracle needs a
      // frame with no ambient specular at all -- its CPU port of ShadeStandard
      // passes ambientSpecular = 0, and porting the split sum to the CPU to
      // match would be a second implementation of the thing under test.
      // Spelling it out beats an intensity of 0, which would reach the same
      // number by coincidence and leave the background sampling an environment
      // the run says it does not have.
      opt.env_path = v;
      opt.env_none = std::strcmp(v, "none") == 0;
    } else if (a == "--near-plane-camera") {
      opt.near_plane_camera = true;
    } else if (a == "--self-test-frame-ring") {
      opt.self_test_frame_ring = true;
      opt.headless = true;
      implies_plane = true;
    } else if (a == "--self-test-hdr") {
      opt.self_test_hdr = true;
      opt.headless = true;
      implies_plane = true;
    } else if (a == "--self-test-gradients") {
      opt.self_test_gradients = true;
      opt.headless = true;
      implies_plane = true;
    } else if (a == "--self-test-visbuffer") {
      opt.self_test_visbuffer = true;
      opt.headless = true;
      implies_plane = true;
    } else if (a == "--width") {
      if (!value(v) || !ParseU32(v, 1, 16384, opt.width)) return false;
    } else if (a == "--height") {
      if (!value(v) || !ParseU32(v, 1, 16384, opt.height)) return false;
    } else if (a == "--frames") {
      uint32_t n = 0;
      if (!value(v) || !ParseU32(v, 1, 100000, n)) return false;
      opt.max_frames = n;
    } else if (a == "--out") {
      if (!value(v)) return false;
      opt.out = v;
      opt.headless = true;  // asking for a file implies headless
    } else {
      spdlog::error("object_viewer: unknown argument '{}'", a);
      return false;
    }
  }

  // The reconciliation. A debug view needs a scene with a MATERIAL, and the
  // plane self-tests need the plane specifically -- so a contradiction is
  // refused rather than resolved by argument order.
  //
  // --debug-view now accepts `spheres` as well: every view it offers is
  // computed by the same resolve for both scenes, and refusing the chart the
  // roughness view would have made the chart's own headless oracle impossible
  // to express. The plane SELF-TESTS still require the plane, because their
  // oracles are closed forms over a flat surface.
  const bool wants_visbuffer = view_given || implies_plane;
  if (implies_plane && scene_given && opt.scene != Scene::Plane) {
    spdlog::error(
        "object_viewer: the plane self-tests require --scene plane, but "
        "--scene was given as something else -- refusing rather than silently "
        "picking one");
    return false;
  }
  if (view_given && scene_given && !SceneUsesVisbuffer(opt.scene)) {
    spdlog::error(
        "object_viewer: --debug-view needs a scene with a material (plane or "
        "spheres), but --scene was given as something else -- refusing rather "
        "than silently picking one");
    return false;
  }
  if (wants_visbuffer && !scene_given) opt.scene = Scene::Plane;
  return true;
}

// A fixed camera for the headless scenes, and the starting camera for the
// windowed one. Orthographic so a world segment maps to a screen span in closed
// form, which is what lets the line assertion name exact pixels rather than
// eyeball a picture.
struct Camera {
  glm::vec3 position{0.0f, 0.0f, 10.0f};
  float extent = 10.0f;  // half-height of the ortho box, in world units
  // Radians. Both zero looks straight down -Z, which is what the line scene
  // needs: its assertion names exact pixels, and that closed form only holds
  // for an unrotated camera. The grid scene pitches down, because a ground
  // plane viewed edge-on is a single line -- which is exactly what the first
  // attempt rendered.
  float pitch = 0.0f;
  float yaw = 0.0f;

  // Orthographic for `lines` and `grid`, perspective for `plane`.
  //
  // The line scene MUST stay orthographic: its assertion is a closed form
  // ("half the width, the middle row"), and that only holds without a
  // perspective divide. The plane scene must NOT be, because an orthographic
  // depth buffer is linear in view distance and its depth preview would be a
  // flat ramp that proves nothing about reversed-Z.
  bool perspective = false;
  float fov_deg = 60.0f;
  float near_m = 0.1f;
  float far_m = 100.0f;

  glm::vec3 Forward() const {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch),
            -std::cos(pitch) * std::cos(yaw)};
  }

  // The camera-OFFSET matrices ExpandDebugLines documents: the camera sits at
  // the origin and the world is rebased by -position.
  glm::mat4 View() const {
    return glm::lookAt(glm::vec3(0.0f), Forward(), glm::vec3(0, 1, 0));
  }
  glm::mat4 Proj(float aspect) const {
    // near and far SWAPPED in BOTH branches, which is how reversed-Z is spelled
    // with GLM_FORCE_DEPTH_ZERO_TO_ONE: the near plane maps to 1 and the far
    // to 0. Swapping them in one branch and not the other is the shape that
    // renders one scene and empties the next.
    if (perspective) {
      return glm::perspective(glm::radians(fov_deg), aspect, far_m, near_m);
    }
    return glm::ortho(-extent * aspect, extent * aspect, -extent, extent,
                      100.0f, 0.0f);
  }
};

// The sphere chart, framed to FIT rather than to a fixed distance.
//
// ASPECT-AWARE because the chart is much wider than it is tall: at 640x256 a
// distance chosen for a square frame leaves it a smudge in the middle, and one
// chosen for the width overflows a square one. Both the render and the oracle
// call this, so the projection the oracle inverts is the projection the frame
// was drawn with -- a second copy here would put every predicted pixel slightly
// off and the failures would read as a broken resolve.
Camera SphereCamera(float aspect) {
  const auto bounds = badlands::object_viewer::SphereGridExtent();
  Camera cam;
  cam.perspective = true;
  cam.far_m = 200.0f;

  const float half_fov = glm::radians(cam.fov_deg) * 0.5f;
  const float fit_vertical = bounds.radius / std::tan(half_fov);
  const float fit_horizontal =
      bounds.radius / (std::tan(half_fov) * std::max(aspect, 1e-3f));
  // The BINDING constraint, plus a margin so nothing touches the frame edge.
  const float distance = std::max(fit_vertical, fit_horizontal) * 1.12f;
  cam.position = {bounds.center.x, bounds.center.y,
                  bounds.center.z + distance};
  return cam;
}

// The frustum basis the resolve's background ray is built from.
//
// Derived from the camera rather than stored on it, so a camera the app moves
// cannot get out of step with the rays -- and both the headless and the
// windowed path call this rather than each deriving its own.
void ApplyViewRays(ResolvePass& resolve, const Camera& cam, float aspect) {
  const glm::vec3 forward = cam.Forward();
  const glm::vec3 right =
      glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
  const glm::vec3 up = glm::cross(right, forward);
  const float half = std::tan(glm::radians(cam.fov_deg) * 0.5f);
  resolve.SetViewRays(forward, right * half * aspect, up * half);
}

// The camera each scene is asserted under. The line scene MUST stay unrotated:
// its assertion is a closed form ("half the width, middle row"), and that only
// holds looking straight down -Z.
Camera CameraFor(Scene scene) {
  Camera cam;
  if (scene == Scene::Grid) {
    cam.position = {0.0f, 6.0f, 14.0f};
    cam.pitch = -0.45f;
    cam.yaw = 0.35f;
    cam.extent = 12.0f;
  } else if (scene == Scene::Spheres) {
    cam = SphereCamera(1.0f);
  } else if (scene == Scene::Plane) {
    // Looking down at the plane from in front of it, so the near edge is
    // genuinely nearer than the far one -- which is what makes the depth
    // assertion a measurement rather than a constant. No yaw: the plane is
    // square and rotating it buys nothing but a harder oracle.
    cam.position = {0.0f, 5.0f, 9.0f};
    cam.pitch = -0.5f;
    cam.perspective = true;
  }
  return cam;
}

// The camera that puts geometry across the near plane.
//
// Sat almost ON the plane and looking along it, so the quads underfoot extend
// behind the eye and their clip w goes non-positive. That is the case the
// barycentric derivation cannot express, and the reason it gets a camera of its
// own rather than a note.
Camera NearPlaneCamera() {
  Camera cam = CameraFor(Scene::Plane);
  // OFF THE GRID LINES, and that is the whole trick. The plane's vertices sit
  // at multiples of 1.25 m; a camera at the origin sits exactly on one, so
  // every triangle is either wholly in front of the eye or wholly behind it and
  // NOTHING straddles visibly. Placed mid-quad instead, the quad underfoot has
  // its near edge in front of the eye and its far edge behind -- one triangle,
  // partly visible, with a vertex at w < 0.
  cam.position = {0.3f, 0.05f, 0.6f};  // 5 cm up, mid-quad in x and z
  cam.pitch = -0.02f;                  // looking very slightly down
  cam.near_m = 0.1f;
  return cam;
}

// One horizontal segment through the world origin. Deliberately the simplest
// thing that can be asserted on: with an orthographic camera it covers the
// middle row of the image and nothing else, so "did the line pass run" and "did
// it run in the right place" are the same question.
DebugLineBuffer LineScene() {
  DebugLineBuffer lines;
  lines.AddLine({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f},
                {1.0f, 0.0f, 0.0f}, /*thickness=*/4.0f);
  return lines;
}

// What a viewer actually wants on screen: a ground grid and the three axes.
DebugLineBuffer GridScene() {
  DebugLineBuffer lines;
  constexpr int kHalf = 10;
  constexpr float kStep = 1.0f;
  const glm::vec3 grey{0.30f, 0.32f, 0.38f};
  for (int i = -kHalf; i <= kHalf; ++i) {
    const float t = float(i) * kStep;
    const float e = float(kHalf) * kStep;
    lines.AddLine({t, 0, -e}, {t, 0, e}, grey, 1.0f);
    lines.AddLine({-e, 0, t}, {e, 0, t}, grey, 1.0f);
  }
  lines.AddLine({0, 0, 0}, {3, 0, 0}, {0.9f, 0.2f, 0.2f}, 3.0f);
  lines.AddLine({0, 0, 0}, {0, 3, 0}, {0.2f, 0.9f, 0.2f}, 3.0f);
  lines.AddLine({0, 0, 0}, {0, 0, 3}, {0.3f, 0.4f, 0.95f}, 3.0f);
  return lines;
}

// The visibility-buffer chain, when there is one. Both halves or neither: a
// resolve with no visibility buffer has nothing to resolve.
struct PlaneChain {
  VisbufferPass* vis = nullptr;
  ResolvePass* resolve = nullptr;

  bool Active() const { return vis && resolve; }
};

// Does this configuration draw debug lines? ONE PREDICATE, used by both the
// windowed and the headless path.
//
// It exists because they had two, and they disagreed: windowed built the line
// pass for every scene except `clear`, so `--scene plane` drew the grid and
// axes over the resolved plane while headless drew neither. The on-screen frame
// was then not the frame any assertion covered -- and the comment claiming it
// was the same scene made that harder to notice, not easier.
bool SceneHasLines(Scene scene) {
  return scene == Scene::Lines || scene == Scene::Grid;
}

// Only a SCENE-REFERRED image gets a tone curve. A debug view has already
// written a display-referred value, and tone-mapping it would turn "roughness
// is 0.35" into a preview that shows 0.26.
// The scenes that drive the visibility-buffer chain. A predicate rather than
// `== Scene::Plane` repeated at six call sites, because that is exactly the
// shape that got the windowed path drawing a different scene from the one the
// assertions covered once before (see SceneHasLines).
// The IBL chain, and the one place that knows how to rebuild it.
//
// REBUILT ONLY ON DEMAND. Baking the cube is 128^2 x 6 CPU evaluations plus a
// thirty-draw prefilter; the sun sliders feed the sky, so dragging one has to
// re-bake -- but an UNCONDITIONAL per-frame rebuild is what the Dawn-side
// light_environment.hpp note warns about.
struct IblChain {
  rhi::TexturePtr source;              // the sun-free environment cube
  std::unique_ptr<ibl::PrefilteredCube> prefiltered;
  std::unique_ptr<ibl::BrdfLut> lut;
  rhi::SamplerPtr sampler;
  glm::vec4 ambient_sh[9]{};
  bool Ready() const { return source && prefiltered && lut; }
};

// Builds the whole chain from a radiance function. The LUT survives a rebuild
// -- its integral depends on neither the environment nor the sun, so
// regenerating it on every slider drag would be pure waste.
bool RebuildIbl(IRhiDevice& device, slang::SlangCompiler& compiler,
                const ibl::RadianceFn& radiance, IblChain& chain) {
  chain.source = ibl::BuildEnvironmentCube(device, radiance);
  if (!chain.source) return false;
  if (!chain.sampler) {
    chain.sampler = device.CreateSampler(
        {.address_u = AddressMode::ClampToEdge,
         .address_v = AddressMode::ClampToEdge,
         .label = "ibl_source"});
    if (!chain.sampler) return false;
  }
  if (!chain.prefiltered) {
    chain.prefiltered = ibl::PrefilteredCube::Create(device, compiler);
    if (!chain.prefiltered) return false;
  }
  if (!chain.prefiltered->Generate(chain.source.get(), chain.sampler.get())) {
    return false;
  }
  if (!chain.lut) {
    chain.lut = ibl::BrdfLut::Create(device, compiler);
    if (!chain.lut) return false;
  }
  // The SAME radiance function the cube was filled from, so the diffuse and
  // specular halves describe one environment.
  ibl::ProjectIrradiance(radiance, chain.ambient_sh);
  return true;
}

// The environment the viewer is showing: procedural unless --env named a file.
// Returned by value so it stays valid for the whole run -- the equirect image
// is captured by reference inside the radiance function, so it has to outlive
// every rebuild.
struct Environment {
  std::unique_ptr<ibl::EquirectImage> image;  // null for the procedural sky
  ibl::SkySettings sky;
  ibl::RadianceFn Radiance() const {
    return image ? ibl::EquirectRadiance(*image) : ibl::ProceduralSky(sky);
  }
  const char* SourceName(const std::string& path) const {
    return image ? path.c_str() : "procedural";
  }
};

bool SceneUsesVisbuffer(Scene scene) {
  return scene == Scene::Plane || scene == Scene::Spheres;
}

// The geometry each of those scenes draws.
SceneMesh MeshFor(Scene scene) {
  return scene == Scene::Spheres ? BuildSphereGrid() : BuildPlaneMesh();
}

bool SceneNeedsTonemap(const Options& opt) {
  return SceneUsesVisbuffer(opt.scene) && opt.view == DebugView::Lit;
}

// THE GRAPH, built identically for both modes. THREE targets, and the split is
// the whole design:
//
//   `scene`  RGBA16Float, LINEAR, SCENE-REFERRED. Lighting is computed with
//            headroom and values above 1 are real -- they are what reaches the
//            EDR compositor instead of clipping at SDR white.
//   `ui`     RGBA8Unorm, ENCODED, PREMULTIPLIED. Debug lines and ImGui blend
//            here, in the space UI is authored for. Blending them into a linear
//            target makes a 50%-alpha panel land at encoded 0.735 instead of
//            0.5 -- every translucent panel washes out.
//   `sink`   the presented surface. Written by exactly one pass, which
//            tonemaps, composites the overlay, and converts once.
//
// `sink` is whatever the caller wants presented; the graph neither knows nor
// cares whether a display is attached.
bool BuildGraph(RenderGraph& graph, ITexture* scene, ITexture* ui,
                ITexture* sink, const Options& opt, LinePass* lines,
                OutputPass* output, ImDrawData* imgui,
                const PlaneChain& plane = {}) {
  // Undefined on entry every frame: a freshly acquired drawable is a NEW
  // resource each time, so any state carried over from last frame would be a
  // lie. Stating it here rather than assuming it is what the entry-state
  // parameter is for.
  auto scene_h = graph.ImportTexture(scene, ResourceState::Undefined, "scene");
  auto ui_h = graph.ImportTexture(ui, ResourceState::Undefined, "ui");
  auto sink_h = graph.ImportTexture(sink, ResourceState::Undefined, "sink");
  if (!scene_h.IsValid() || !ui_h.IsValid() || !sink_h.IsValid()) return false;

  if (plane.Active()) {
    // The visibility buffer FIRST, then the resolve into the scene target. The
    // resolve clears rather than loads, so it replaces the clear pass entirely
    // -- a clear underneath a fullscreen pass is work nothing can observe.
    if (!plane.vis->AddToGraph(graph)) return false;
    if (!plane.resolve->AddToGraph(
            graph, plane.vis->VisbufferHandle(), plane.vis->Visbuffer(),
            plane.vis->VerticesHandle(), plane.vis->IndicesHandle(),
            plane.vis->DrawsHandle(), scene_h, plane.vis->Vertices(),
            plane.vis->Indices(), plane.vis->Draws())) {
      return false;
    }
  } else {
    // The clear colour is authored ENCODED, like every other colour in this
    // app, but the scene target is LINEAR -- so it is converted here. Clearing
    // a linear target to an encoded value is the mistake that makes a dark
    // background render washed out.
    const badlands::color::Rgb lin = badlands::color::SrgbToLinear(
        {opt.clear[0], opt.clear[1], opt.clear[2]});
    const float clear_linear[4] = {lin.r, lin.g, lin.b, opt.clear[3]};
    graph.AddRasterPass("clear")
        .ColorTarget(scene_h, LoadOp::Clear, StoreOp::Store, clear_linear)
        .Execute([](const RasterContext&) {
          // The clear IS this pass. It exists as its own pass so the target has
          // a defined starting state whether or not anything draws afterwards.
        });
  }

  // The OVERLAY, always cleared even when nothing draws into it: the output
  // pass samples it unconditionally, and an uncleared layer is whatever the
  // allocation happened to hold.
  const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  graph.AddRasterPass("ui_clear")
      .ColorTarget(ui_h, LoadOp::Clear, StoreOp::Store, transparent)
      .Execute([](const RasterContext&) {});

  if (lines) lines->AddToGraph(graph, ui_h, LoadOp::Load);
  // ImGui LAST of the overlay passes, so the debug UI always sits on top.
  if (imgui) ImGui_ImplRHI_AddPass(imgui, graph, ui_h);

  if (!output->AddToGraph(graph, scene_h, scene, ui_h, ui, sink_h, opt.present,
                          SceneNeedsTonemap(opt))) {
    return false;
  }
  return graph.Compile();
}

// The Slang compiler, created only when a scene needs a shader. The clear scene
// does not, so a run without a Slang SDK still proves the graph.
std::unique_ptr<slang::SlangCompiler> MakeCompiler() {
  // Three roots: shared modules first, then the viewer's own shaders and the
  // ImGui one, each resolving its own imports.
  const std::vector<std::string> paths = {"shaders/slang/common",
                                          "shaders/slang/object_viewer",
                                          "shaders/slang/ibl",
                                          "shaders/slang/ui"};
  return slang::CreateSlangCompiler(paths);
}

// THE TWO INTERMEDIATE FORMATS, named once.
//
// Every pipeline that renders into one of these targets must declare the same
// format, and a mismatch does NOT fail loudly -- Metal writes through a
// pipeline built for RGBA8Unorm into an RGBA16Float attachment and produces a
// red-only image with wrong values, which reads as a broken shader. That is
// exactly what happened when one of the two call sites was updated and the
// other was not, so the constant exists to make that unrepresentable.
inline constexpr Format kSceneFormat = Format::RGBA16Float;
inline constexpr Format kUiFormat = Format::RGBA8Unorm;

// The scene target: LINEAR, whatever the surface is.
//
// Float because lighting needs headroom -- an 8-bit target clamps the lit
// result to SDR white inside the resolve, and no amount of EDR downstream can
// recover what was already thrown away.
rhi::TexturePtr MakeSceneTarget(IRhiDevice& device, uint32_t w, uint32_t h) {
  return device.CreateTexture({.width = w,
                               .height = h,
                               .format = kSceneFormat,
                               .usage = TextureUsage::RenderTarget |
                                        TextureUsage::Sampled,
                               .label = "scene"});
}

// The overlay: 8-bit, ENCODED, premultiplied. Debug lines and ImGui blend here
// rather than into the scene, because UI is authored for encoded-space blending
// and the scene is linear.
rhi::TexturePtr MakeUiTarget(IRhiDevice& device, uint32_t w, uint32_t h) {
  return device.CreateTexture({.width = w,
                               .height = h,
                               .format = kUiFormat,
                               // CopySrc so a headless run can read back WHAT
                               // THE UI COVERED -- the two composites agree at
                               // alpha 0 and 1 and differ only in between, so a
                               // test cannot check either claim without it.
                               .usage = TextureUsage::RenderTarget |
                                        TextureUsage::Sampled |
                                        TextureUsage::CopySrc,
                               .label = "ui"});
}

// How many of `mesh`'s triangles straddle the near plane under `cam` -- that
// is, have at least one vertex with clip w <= 0 and at least one with w > 0.
//
// THE NEAR-PLANE TEST'S SELF-GUARD. The resolve refetches the ORIGINAL,
// unclipped vertices, so those triangles are the ones whose barycentric
// derivation divides by a non-positive w. A "--near-plane-camera" run that
// produced none of them would pass while testing nothing, which is a worse
// outcome than failing.
std::vector<bool> NearPlaneStraddlers(const Camera& cam, const SceneMesh& mesh,
                                      float aspect) {
  const glm::mat4 vp = cam.Proj(aspect) * cam.View();
  std::vector<bool> straddling(mesh.TriangleCount(), false);
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    int behind = 0;
    for (int k = 0; k < 3; ++k) {
      const glm::vec3 p = glm::vec3(mesh.vertices[mesh.indices[t + k]].pos_nx);
      // Camera-OFFSET, exactly as both the raster and the resolve project it.
      if ((vp * glm::vec4(p - cam.position, 1.0f)).w <= 0.0f) ++behind;
    }
    if (behind > 0 && behind < 3) straddling[t / 3] = true;
  }
  return straddling;
}

// Begins a device frame and ALWAYS ends it.
//
// Every early return between BeginFrame and EndFrame leaks a pacing count, and
// the destructor then reports "device destroyed with frame N still open" as the
// last line of the run -- so a shader that failed to compile surfaces as an RHI
// frame-model violation and the real error scrolls past above it. There were
// four such returns; a guard is one thing to get right instead of four.
class FrameScope {
 public:
  explicit FrameScope(IRhiDevice& device) : device_(&device) {
    device_->BeginFrame();
  }
  ~FrameScope() { device_->EndFrame(); }
  FrameScope(const FrameScope&) = delete;
  FrameScope& operator=(const FrameScope&) = delete;

 private:
  IRhiDevice* device_;
};

// IEEE 754 binary16 -> float. Needed because the extended-range sink is
// RGBA16Float and a readback hands back its raw bytes; without this the
// two-sink comparison could only be made on a machine with an HDR display,
// which is to say on nobody's CI.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1Fu;
  const uint32_t mant = h & 0x3FFu;
  if (exp == 0) {
    if (mant == 0) return std::bit_cast<float>(sign);  // +/-0
    // Subnormal: normalize by hand rather than trusting a shift chain.
    float v = float(mant) / 1024.0f * std::ldexp(1.0f, -14);
    return sign ? -v : v;
  }
  if (exp == 31) {  // inf / nan
    return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
  }
  return std::bit_cast<float>(sign | ((exp + 112u) << 23) | (mant << 13));
}

// One rendered frame, read back as floats in 0..1 (or beyond, on the extended
// -range sink). Float rather than bytes so the two sinks are comparable at all:
// one of them has no bytes to compare.
struct Frame {
  std::vector<float> rgba;  // width * height * 4
  uint32_t width = 0, height = 0;
  // The UI overlay, read back alongside the sink. Needed because the two
  // composites agree at alpha 0 and alpha 1 and differ only in between, so a
  // test has to know which texels are partially covered.
  std::vector<float> overlay;  // width * height * 4, encoded premultiplied
  // How many passes the graph compiled. Reported so a test can assert on the
  // SHAPE of the frame and not only its pixels -- which is how a stray pass
  // gets caught. The windowed path once added a debug-line pass to --scene
  // plane that headless never added, and no pixel assertion noticed because no
  // headless run ever built that graph.
  size_t passes = 0;
};

// Renders `opt`'s scene once, into a sink of the format `mode` implies.
// Returns false after logging; a caller must not read `out` on failure.
bool RenderOnce(IRhiDevice& device, const Options& opt, rhi::ColorSpace mode,
                slang::SlangCompiler& compiler, Frame& out) {
  const Format sink_format = SinkFormatFor(mode);
  const uint32_t texel_bytes = FormatByteSize(sink_format);

  auto scene = MakeSceneTarget(device, opt.width, opt.height);
  auto ui = MakeUiTarget(device, opt.width, opt.height);
  auto sink = device.CreateTexture({.width = opt.width,
                                    .height = opt.height,
                                    .format = sink_format,
                                    .usage = TextureUsage::RenderTarget |
                                             TextureUsage::CopySrc,
                                    .label = "sink"});
  auto readback = device.CreateBuffer(
      {.size = uint64_t(opt.width) * opt.height * texel_bytes,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "readback"});
  auto ui_readback = device.CreateBuffer(
      {.size = uint64_t(opt.width) * opt.height * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "ui_readback"});
  if (!scene || !ui || !sink || !readback || !ui_readback) return false;

  auto output = OutputPass::Create(device, compiler, sink_format);
  if (!output) return false;

  std::unique_ptr<LinePass> lines;
  if (SceneHasLines(opt.scene) || opt.force_overlay) {
    // Targets the OVERLAY, which is 8-bit encoded -- not the scene, which is
    // linear float, and not the surface.
    lines = LinePass::Create(device, compiler, kUiFormat);
    if (!lines) return false;
  }

  // The visibility-buffer chain, built only for the plane scene. Its material
  // pack is four decoded textures with full mip chains, which is not a cost to
  // pay for a scene that shows a line.
  std::unique_ptr<MaterialPack> pack;
  std::unique_ptr<VisbufferPass> vis;
  std::unique_ptr<ResolvePass> resolve;
  // FUNCTION SCOPE, not the branch below. The resolve holds BORROWED views into
  // the chain, and the equirect image is captured by reference by the radiance
  // function -- either one going out of scope early leaves the table pointing
  // at freed memory, which is a segfault rather than a wrong image.
  Environment env;
  IblChain ibl_chain;
  PlaneChain chain;
  const float scene_aspect = float(opt.width) / float(std::max(1u, opt.height));
  const Camera plane_cam =
      opt.near_plane_camera
          ? NearPlaneCamera()
          : (opt.scene == Scene::Spheres ? SphereCamera(scene_aspect)
                                         : CameraFor(opt.scene));
  if (SceneUsesVisbuffer(opt.scene)) {
    // "test" and "checker" are SYNTHETIC packs, not directories. The headless
    // oracles use them so no assertion depends on a shipped data file, and
    // because constant textures make the mip level irrelevant to nine of the
    // ten views.
    pack = opt.pack == "test"
               ? MakeConstantPack(device, badlands::object_viewer::TestPackValues{})
               : (opt.pack == "checker" ? MakeCheckerPack(device)
                                        : LoadMaterialPack(device, opt.pack));
    if (!pack) return false;
    const SceneMesh mesh = MeshFor(opt.scene);
    vis = VisbufferPass::Create(device, compiler, mesh, opt.width, opt.height);
    resolve = ResolvePass::Create(device, compiler, *pack, kSceneFormat);
    if (!vis || !resolve) return false;

    const float aspect = float(opt.width) / float(opt.height);
    vis->SetCamera(plane_cam.View(), plane_cam.Proj(aspect), plane_cam.position);
    resolve->SetCamera(plane_cam.View(), plane_cam.Proj(aspect),
                       plane_cam.position, plane_cam.near_m, plane_cam.far_m);
    SunSettings sun;
    sun.intensity = opt.sun_intensity;
    resolve->SetSun(sun);
    resolve->SetView(opt.view);
    ApplyViewRays(*resolve, plane_cam, aspect);

    // THE IBL CHAIN. Built here rather than passed in, so the headless path is
    // the same path the window runs -- which is the whole reason the headless
    // assertions mean anything.
    if (!opt.env_none) {
      if (!opt.env_path.empty()) {
        env.image = ibl::EquirectImage::Load(opt.env_path);
        if (!env.image) return false;  // a bad --env is a refusal, not a sky
      }
      if (!RebuildIbl(device, compiler, env.Radiance(), ibl_chain)) return false;
      resolve->SetEnvironment(ibl_chain.prefiltered->CubeView(),
                              ibl::PrefilteredCube::kMipCount,
                              ibl_chain.lut->View(), opt.env_intensity);
      resolve->SetAmbient(ibl_chain.ambient_sh);
    }
    chain = {.vis = vis.get(), .resolve = resolve.get()};
  }

  device.BeginValidationScope();
  // BRACED so the guard's scope is exactly the frame. Every `return false`
  // inside now ends the frame on the way out, instead of leaking a pacing count
  // and turning a shader error into a spurious "frame still open" report.
  {
  FrameScope frame_scope(device);
  // Every per-frame ring recycles here, after BeginFrame and before anything
  // writes into it.
  if (vis) vis->BeginFrame(device.CurrentFrame());
  if (resolve) resolve->BeginFrame(device.CurrentFrame());
  output->BeginFrame(device.CurrentFrame());
  if (lines) {
    lines->BeginFrame(device.CurrentFrame());
    // The PLANE's camera when the plane chain is active, so the overlay lands
    // over the lit geometry rather than somewhere else entirely.
    const Camera cam = chain.Active() ? plane_cam : CameraFor(opt.scene);
    const float aspect = float(opt.width) / float(opt.height);
    const DebugLineBuffer buf =
        opt.scene == Scene::Lines ? LineScene() : GridScene();
    if (!lines->Upload(buf, cam.View(), cam.Proj(aspect),
                       {float(opt.width), float(opt.height)}, cam.position)) {
      return false;
    }
  }

  Options local = opt;
  local.present = mode;
  RenderGraph graph(device);
  if (!BuildGraph(graph, scene.get(), ui.get(), sink.get(), local, lines.get(),
                  output.get(), nullptr, chain)) {
    return false;
  }

  out.passes = graph.PassCount();
  auto encoder = device.CreateCommandEncoder("frame");
  graph.Execute(*encoder);
  // The readback is the CALLER's, not the graph's: copying out is a property of
  // this run, not of the passes, and putting it in the graph would give the
  // windowed path a copy it never performs.
  encoder->Transition(sink.get(), ResourceState::CopySrc);
  encoder->Transition(readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(sink.get(), 0, 0, readback.get(), 0);
  encoder->Transition(ui.get(), ResourceState::CopySrc);
  encoder->Transition(ui_readback.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(ui.get(), 0, 0, ui_readback.get(), 0);
  encoder->Finish();
  device.Submit(*encoder);
  }  // the frame ends HERE, on every path out of the block above
  device.WaitIdle();

  if (auto report = device.EndValidationScope();
      report && !report->IsClean()) {
    spdlog::error("object_viewer: validation observed: {}", report->violations);
    return false;
  }

  std::vector<uint8_t> raw(size_t(opt.width) * opt.height * texel_bytes, 0);
  if (!readback->Read(0, raw)) {
    spdlog::error("object_viewer: readback failed");
    return false;
  }

  std::vector<uint8_t> ui_raw(size_t(opt.width) * opt.height * 4, 0);
  if (!ui_readback->Read(0, ui_raw)) {
    spdlog::error("object_viewer: overlay readback failed");
    return false;
  }
  out.overlay.assign(ui_raw.size(), 0.0f);
  for (size_t i = 0; i < ui_raw.size(); ++i) {
    out.overlay[i] = float(ui_raw[i]) / 255.0f;
  }

  out.width = opt.width;
  out.height = opt.height;
  out.rgba.assign(size_t(opt.width) * opt.height * 4, 0.0f);
  for (size_t i = 0; i < out.rgba.size(); ++i) {
    if (sink_format == Format::RGBA16Float) {
      uint16_t h = 0;
      std::memcpy(&h, &raw[i * 2], 2);
      out.rgba[i] = HalfToFloat(h);
    } else {
      out.rgba[i] = float(raw[i]) / 255.0f;
    }
  }
  return true;
}

// THE TWO-SINK TEST, and the whole of the HDR/LDR rule in one check.
//
// The same frame is rendered into an 8-bit P3 sink and a float extended-linear
// one. Both derive from the same encoded scene target through the same
// transform, so the float texel must equal the 8-bit one decoded and re-run --
// and BECAUSE THE SCENE TARGET IS SHARED, blended pixels compare too. That is
// what the offscreen-target design bought: had each pass converted for itself,
// the two sinks would have blended in different spaces and only opaque pixels
// would have been comparable.
int RunOutputSelfTest(IRhiDevice& device, const Options& opt) {
  // A TONE-MAPPED SCENE IS NOT COMPARABLE ACROSS THE TWO SINKS, by design: the
  // curve is a fit to the display's range, so the 8-bit render is Reinhard-
  // compressed and the float one deliberately is not. Comparing them would
  // blame the output transform for the difference the curve exists to make.
  //
  // Refused rather than silently skipped: --self-test-output --scene plane
  // otherwise reported a failure of a transform that is behaving correctly.
  if (SceneNeedsTonemap(opt)) {
    spdlog::error(
        "object_viewer: --self-test-output compares the two sinks against each "
        "other, which only holds where NEITHER is tone-mapped -- and a lit "
        "plane is. Use a debug view, or a scene that is already "
        "display-referred.");
    return 1;
  }
  // AND NO OVERLAY. The two composites agree at alpha 0 and alpha 1 and differ
  // BY DESIGN in between: linear on the extended-range path (which is what
  // preserves the scene's headroom) and encoded on the 8-bit one. An antialias
  // fringe is exactly the partial-alpha case, so a scene with debug lines makes
  // this test fail on a difference that is correct.
  if (SceneHasLines(opt.scene)) {
    spdlog::error(
        "object_viewer: --self-test-output needs a scene with no UI overlay -- "
        "the two composites differ at PARTIAL alpha by design, and a line's "
        "antialias fringe is exactly that. Use --scene plane with a debug "
        "view.");
    return 1;
  }
  auto compiler = MakeCompiler();
  if (!compiler) return 1;

  Frame p3, edr;
  if (!RenderOnce(device, opt, rhi::ColorSpace::DisplayP3, *compiler, p3)) {
    return 1;
  }
  if (!RenderOnce(device, opt, rhi::ColorSpace::ExtendedLinearDisplayP3,
                  *compiler, edr)) {
    return 1;
  }

  // Half-float carries ~11 bits of mantissa, and the 8-bit sink quantizes to
  // 1/255 -- so the comparison is made in the 8-bit sink's units with a 1-LSB
  // tolerance, which is where the real precision floor is.
  constexpr float kTol = 1.5f / 255.0f;
  size_t compared = 0, nonzero = 0;
  for (size_t i = 0; i < p3.rgba.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      // The 8-bit sink holds an ENCODED P3 value; the float sink holds the
      // LINEAR one. Decoding the former is the only step between them.
      const float want = badlands::color::SrgbToLinear(p3.rgba[i + c]);
      const float got = edr.rgba[i + c];
      if (std::abs(want - got) > kTol) {
        spdlog::error(
            "object_viewer: texel {} channel {}: the 8-bit sink holds {:.4f} "
            "(decodes to {:.4f}) but the float sink holds {:.4f} -- the two "
            "output modes disagree",
            i / 4, c, p3.rgba[i + c], want, got);
        return 1;
      }
      ++compared;
      if (got > 0.001f) ++nonzero;
    }
  }
  // A frame of black would satisfy every comparison above. This is the check
  // that the test looked at something.
  if (nonzero * 4 < compared) {
    spdlog::error(
        "object_viewer: only {} of {} channels were non-black -- the two-sink "
        "test compared an empty frame",
        nonzero, compared);
    return 1;
  }
  spdlog::info("object_viewer: the 8-bit and float sinks agree across {} "
               "channels ({} non-black)", compared, nonzero);
  return 0;
}

// Where a pixel's view ray meets the plane, in world space. Returns the origin
// when the ray misses; callers pair it with PlaneTriangleUnderPixel, which says
// whether it did.
glm::vec3 PlaneHitPoint(const Camera& cam, uint32_t px, uint32_t py,
                        uint32_t width, uint32_t height) {
  const float aspect = float(width) / float(height);
  const float ndc_x = (float(px) + 0.5f) / float(width) * 2.0f - 1.0f;
  const float ndc_y = 1.0f - (float(py) + 0.5f) / float(height) * 2.0f;
  const glm::mat4 inv = glm::inverse(cam.Proj(aspect) * cam.View());
  glm::vec4 pn = inv * glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
  glm::vec4 pf = inv * glm::vec4(ndc_x, ndc_y, 0.0f, 1.0f);
  const glm::vec3 a = glm::vec3(pn) / pn.w + cam.position;
  const glm::vec3 b = glm::vec3(pf) / pf.w + cam.position;
  const glm::vec3 dir = b - a;
  if (std::abs(dir.y) < 1e-6f) return glm::vec3(0.0f);
  return a + dir * (-a.y / dir.y);
}

// Which triangle of the plane a pixel's view ray lands on, computed on the CPU.
//
// THE ORACLE for the visibility buffer, and the reason 4c can be asserted with
// no resolve at all. The mesh is 8x8 quads; within a quad, triangle A has
// corners (0,0), (0,1), (1,0) in local coordinates -- so it is the half where
// fx + fz <= 1, and triangle B is the other. Returns -1 when the ray misses.
int PlaneTriangleUnderPixel(const Camera& cam, uint32_t px, uint32_t py,
                            uint32_t width, uint32_t height,
                            float half_extent) {
  constexpr int kQuads = 8;  // must match kQuadsPerSide in plane_mesh.cpp
  const float aspect = float(width) / float(height);
  // Pixel centre -> NDC. The half-texel matters: sampling the corner of a texel
  // puts a pixel on the far side of a triangle edge from where it was rasterized.
  const float ndc_x = (float(px) + 0.5f) / float(width) * 2.0f - 1.0f;
  const float ndc_y = 1.0f - (float(py) + 0.5f) / float(height) * 2.0f;

  const glm::mat4 inv = glm::inverse(cam.Proj(aspect) * cam.View());
  // Reversed-Z: z = 1 is the NEAR plane, z = 0 the far one. Two points on the
  // ray rather than one plus a direction, so the same inverse serves both.
  glm::vec4 pn = inv * glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
  glm::vec4 pf = inv * glm::vec4(ndc_x, ndc_y, 0.0f, 1.0f);
  const glm::vec3 a = glm::vec3(pn) / pn.w + cam.position;  // undo the rebase
  const glm::vec3 b = glm::vec3(pf) / pf.w + cam.position;
  const glm::vec3 dir = b - a;
  if (std::abs(dir.y) < 1e-6f) return -1;  // parallel to the plane

  const float t = -a.y / dir.y;
  if (t < 0.0f || t > 1.0f) return -1;  // behind the eye or past the far plane
  const glm::vec3 hit = a + dir * t;
  if (std::abs(hit.x) > half_extent || std::abs(hit.z) > half_extent) return -1;

  const float u = (hit.x / half_extent * 0.5f + 0.5f) * float(kQuads);
  const float v = (hit.z / half_extent * 0.5f + 0.5f) * float(kQuads);
  const int qx = std::clamp(int(std::floor(u)), 0, kQuads - 1);
  const int qz = std::clamp(int(std::floor(v)), 0, kQuads - 1);
  const float fx = u - float(qx);
  const float fz = v - float(qz);
  return 2 * (qz * kQuads + qx) + (fx + fz <= 1.0f ? 0 : 1);
}

// 4c: the visibility buffer, asserted WITHOUT the resolve.
//
// The R32Uint target is read back and compared against the CPU ray/plane
// oracle directly. A stage that could only be checked through the next stage is
// not a stage -- and this way a wrong resolve in 4d cannot be mistaken for a
// wrong raster here.
int RunPlaneVisbufferCheck(IRhiDevice& device, const Options& opt) {
  auto compiler = MakeCompiler();
  if (!compiler) return 1;

  constexpr float kHalfExtent = 5.0f;
  const SceneMesh mesh = BuildPlaneMesh(kHalfExtent);
  auto vis = VisbufferPass::Create(device, *compiler, mesh, opt.width,
                                   opt.height);
  if (!vis) return 1;

  // HONOURS --near-plane-camera. It used to hardcode the ordinary camera, so
  // the flag was accepted and did nothing and a near-plane raster regression
  // could not be caught from this entry point at all.
  const Camera cam =
      opt.near_plane_camera ? NearPlaneCamera() : CameraFor(Scene::Plane);
  vis->SetCamera(cam.View(), cam.Proj(float(opt.width) / float(opt.height)),
                 cam.position);

  auto vis_read = device.CreateBuffer(
      {.size = uint64_t(opt.width) * opt.height * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "vis_readback"});
  auto depth_read = device.CreateBuffer(
      {.size = uint64_t(opt.width) * opt.height * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "depth_readback"});
  if (!vis_read || !depth_read) return 1;

  device.BeginValidationScope();
  // BRACED, so the two `return 1`s below still end the frame -- see FrameScope.
  {
  FrameScope frame_scope(device);
  vis->BeginFrame(device.CurrentFrame());
  RenderGraph graph(device);
  if (!vis->AddToGraph(graph)) return 1;
  if (!graph.Compile()) return 1;

  auto encoder = device.CreateCommandEncoder("frame");
  graph.Execute(*encoder);
  encoder->Transition(vis->Visbuffer(), ResourceState::CopySrc);
  encoder->Transition(vis->Depth(), ResourceState::CopySrc);
  encoder->Transition(vis_read.get(), ResourceState::CopyDst);
  encoder->Transition(depth_read.get(), ResourceState::CopyDst);
  encoder->CopyTextureToBuffer(vis->Visbuffer(), 0, 0, vis_read.get(), 0);
  encoder->CopyTextureToBuffer(vis->Depth(), 0, 0, depth_read.get(), 0);
  encoder->Finish();
  device.Submit(*encoder);
  }
  device.WaitIdle();

  if (auto report = device.EndValidationScope();
      report && !report->IsClean()) {
    spdlog::error("object_viewer: validation observed: {}", report->violations);
    return 1;
  }

  std::vector<uint8_t> vis_raw(size_t(opt.width) * opt.height * 4, 0);
  std::vector<uint8_t> depth_raw(size_t(opt.width) * opt.height * 4, 0);
  if (!vis_read->Read(0, vis_raw) || !depth_read->Read(0, depth_raw)) {
    spdlog::error("object_viewer: visibility-buffer readback failed");
    return 1;
  }
  auto vis_at = [&](uint32_t x, uint32_t y) {
    uint32_t v = 0;
    std::memcpy(&v, &vis_raw[(size_t(y) * opt.width + x) * 4], 4);
    return v;
  };
  auto depth_at = [&](uint32_t x, uint32_t y) {
    float v = 0;
    std::memcpy(&v, &depth_raw[(size_t(y) * opt.width + x) * 4], 4);
    return v;
  };

  constexpr uint32_t kPrimBits = 24;
  constexpr uint32_t kPrimMask = (1u << kPrimBits) - 1u;

  // 1. EVERY covered pixel agrees with the oracle, and every uncovered one is
  //    exactly 0. Checking only the centre would pass against a raster that
  //    smeared one triangle over the whole screen.
  size_t covered = 0, background = 0;
  for (uint32_t y = 0; y < opt.height; ++y) {
    for (uint32_t x = 0; x < opt.width; ++x) {
      const uint32_t packed = vis_at(x, y);
      const int want = PlaneTriangleUnderPixel(cam, x, y, opt.width, opt.height,
                                               kHalfExtent);
      if (packed == 0) {
        ++background;
        // A pixel the oracle says is covered but the GPU left empty is a real
        // failure everywhere except within a texel of a silhouette, where the
        // two rasterizers legitimately disagree. Edges are excluded by only
        // failing when the oracle is confident -- see the interior test below.
        continue;
      }
      ++covered;
      if ((packed >> kPrimBits) != 0) {
        spdlog::error(
            "object_viewer: texel ({},{}) packs draw slot {} -- there is one "
            "draw, so the primitive field has overflowed into it",
            x, y, packed >> kPrimBits);
        return 1;
      }
      const uint32_t prim = (packed & kPrimMask) - 1u;
      if (prim >= mesh.TriangleCount()) {
        spdlog::error(
            "object_viewer: texel ({},{}) names primitive {} of a {}-triangle "
            "mesh", x, y, prim, mesh.TriangleCount());
        return 1;
      }
      if (want < 0) continue;  // silhouette disagreement, allowed
      // Neighbouring triangles are one apart within a quad and 2/16 apart
      // across one, so an exact match is required in the interior and only the
      // ray/raster edge disagreement is tolerated.
      if (int(prim) != want) {
        // The oracle must be STABLE around this pixel before a mismatch counts:
        // a pixel whose centre falls on a triangle edge is one the ray and the
        // rasterizer may legitimately assign differently.
        //
        // (This used to re-call PlaneTriangleUnderPixel with identical
        // arguments and compare it to `want` -- which is that same call's
        // result. A tautology, plus a per-pixel glm::inverse on the failure
        // path.)
        const int l = PlaneTriangleUnderPixel(cam, x ? x - 1 : x, y, opt.width,
                                              opt.height, kHalfExtent);
        const int r = PlaneTriangleUnderPixel(cam, x + 1 < opt.width ? x + 1 : x,
                                              y, opt.width, opt.height,
                                              kHalfExtent);
        const int u = PlaneTriangleUnderPixel(cam, x, y ? y - 1 : y, opt.width,
                                              opt.height, kHalfExtent);
        const int dn = PlaneTriangleUnderPixel(cam, x,
                                               y + 1 < opt.height ? y + 1 : y,
                                               opt.width, opt.height,
                                               kHalfExtent);
        const bool on_edge = l != want || r != want || u != want || dn != want;
        if (!on_edge) {
          spdlog::error(
              "object_viewer: texel ({},{}) names primitive {}, but the CPU "
              "ray/plane intersection puts triangle {} there",
              x, y, prim, want);
          return 1;
        }
      }
    }
  }

  // 2. NOT VACUOUS. A visibility buffer that stayed empty satisfies every
  //    check above, because "packed == 0" simply skips.
  const size_t total = size_t(opt.width) * opt.height;
  if (covered < total / 20) {
    spdlog::error(
        "object_viewer: only {} of {} texels are covered -- the plane did not "
        "rasterize (check the winding and the reversed-Z depth clear)",
        covered, total);
    return 1;
  }
  if (background == 0) {
    spdlog::error(
        "object_viewer: no texel is empty -- the plane should not fill the "
        "frame, so kVisEmpty is never being observed");
    return 1;
  }

  // 3. REVERSED-Z, asserted as an ordering between two GPU values rather than
  //    against a computed depth: nearer must hold the LARGER value. The camera
  //    pitches down at the plane, so a lower row is nearer.
  const uint32_t cx = opt.width / 2;
  uint32_t near_y = 0, far_y = 0;
  bool have_near = false, have_far = false;
  for (uint32_t y = opt.height; y-- > 0;) {
    if (vis_at(cx, y) != 0) { near_y = y; have_near = true; break; }
  }
  for (uint32_t y = 0; y < opt.height; ++y) {
    if (vis_at(cx, y) != 0) { far_y = y; have_far = true; break; }
  }
  if (!have_near || !have_far || near_y == far_y) {
    spdlog::error(
        "object_viewer: the centre column covers too few rows to compare "
        "depths");
    return 1;
  }
  const float near_d = depth_at(cx, near_y);
  const float far_d = depth_at(cx, far_y);
  if (!(near_d > far_d)) {
    spdlog::error(
        "object_viewer: the nearer texel (row {}) holds depth {:.6f} and the "
        "farther one (row {}) holds {:.6f} -- reversed-Z requires nearer to be "
        "LARGER",
        near_y, near_d, far_y, far_d);
    return 1;
  }

  spdlog::info(
      "object_viewer: visibility buffer OK -- {} covered texels agree with the "
      "CPU ray/plane oracle, {} empty, depth {:.4f} (near) > {:.4f} (far)",
      covered, background, near_d, far_d);
  return 0;
}

// --- The per-view oracles ---------------------------------------------------
//
// Every one compares a read-back texel against a CPU evaluation of the same
// rule. The pack is SYNTHETIC and its textures are constant, which is what
// makes nine of these exact: with every mip holding the same value, the mip the
// GPU chose cannot affect the answer, so mip prediction is confined to the one
// test that is actually about it.
//
// The pixel under test is the image centre, and it must be covered -- a check
// against the background colour would pass against a resolve that drew nothing.
int RunDebugViewCheck(IRhiDevice& device, const Options& opt) {
  using badlands::object_viewer::TestPackValues;
  const TestPackValues kv{};

  Options local = opt;
  // The exact oracles below describe the CONSTANT pack's values, so they only
  // mean anything against it. An explicit --pack is honoured and the exact
  // comparisons are skipped, with the reason logged -- silently substituting
  // the test pack would make --pack a flag that changed nothing.
  const bool exact = !opt.pack_explicit;
  if (!exact) {
    spdlog::info(
        "object_viewer: --pack '{}' given, so the {} view is checked for "
        "coverage but not against the constant-pack oracle",
        opt.pack, DebugViews()[size_t(opt.view)].cli);
  } else {
    local.pack = "test";
  }

  // WHICH VIEWS STILL MEAN SOMETHING WITHOUT THE CONSTANT PACK. Triangle id,
  // barycentrics and depth are pure geometry -- no texture is involved -- so
  // their oracles hold against any pack. The material views do not, and for
  // those an explicit --pack leaves only a coverage check.
  //
  // The counter below is what stops that becoming nothing at all: a run that
  // asserted zero things must not exit 0, because in a ctest summary that is
  // indistinguishable from a run that passed.
  const bool geometry_view = opt.view == DebugView::TriangleId ||
                             opt.view == DebugView::Barycentric ||
                             opt.view == DebugView::Depth;
  int assertions_run = 0;
  // THE BARYCENTRIC PARTITION IS MEASURED THROUGH AN IDENTITY TRANSFORM.
  //
  // "The three weights sum to 1" is a claim about the RESOLVE, and observing it
  // through the P3 path measures the transform instead: the primaries matrix
  // mixes channels, the sRGB curve is steep near black, and the round trip
  // amplifies an 8-bit step into ~4% at pixels where one weight is small. Under
  // Srgb the encode and decode are exact inverses, so the sink holds the scene
  // code value and the only loss is one quantization.
  //
  // Nothing is skipped by this: the transform has its own tests (the two-sink
  // comparison and one per mode), and every other view here is still asserted
  // in whatever mode the caller asked for.
  if (opt.view == DebugView::Barycentric) {
    local.present = rhi::ColorSpace::Srgb;
  }

  // A near-plane run that produced no straddling triangle would pass while
  // testing nothing. Refused rather than reported, because a green result from
  // a vacuous check is what retires a risk that is still there.
  if (opt.near_plane_camera) {
    const Camera np = NearPlaneCamera();
    const SceneMesh mesh = BuildPlaneMesh();
    const float aspect = float(opt.width) / float(opt.height);
    const std::vector<bool> straddling = NearPlaneStraddlers(np, mesh, aspect);
    const size_t n =
        size_t(std::count(straddling.begin(), straddling.end(), true));

    // Existing is not enough -- one has to be ON SCREEN. A straddling triangle
    // whose visible part falls outside the frame never reaches the resolve, and
    // the run would be green without the w <= 0 path executing once.
    size_t visible_px = 0;
    for (uint32_t y = 0; y < opt.height; ++y) {
      for (uint32_t x = 0; x < opt.width; ++x) {
        const int tri =
            PlaneTriangleUnderPixel(np, x, y, opt.width, opt.height, 5.0f);
        if (tri >= 0 && size_t(tri) < straddling.size() && straddling[tri]) {
          ++visible_px;
        }
      }
    }
    if (n == 0 || visible_px == 0) {
      spdlog::error(
          "object_viewer: --near-plane-camera has {} straddling triangles "
          "covering {} pixels -- this run would assert nothing about the case "
          "it exists for. Move the camera closer to the plane.",
          n, visible_px);
      return 1;
    }
    spdlog::info(
        "object_viewer: {} triangles straddle the near plane, covering {} "
        "pixels", n, visible_px);
  }

  auto compiler = MakeCompiler();
  if (!compiler) return 1;
  Frame frame;
  if (!RenderOnce(device, local, local.present, *compiler, frame)) return 1;

  // THE SHAPE OF THE FRAME, before any pixel is looked at.
  //
  // --scene plane is exactly: visbuffer, resolve, ui_clear, output. A fifth
  // pass means something is drawing that the spec says should not be -- which
  // is precisely the defect where the windowed path added the debug grid over
  // the plane and headless did not, so the on-screen frame was not the frame
  // any assertion covered. Both paths now share SceneHasLines, and this is what
  // notices if that stops being true.
  constexpr size_t kPlanePasses = 4;
  if (frame.passes != kPlanePasses) {
    spdlog::error(
        "object_viewer: the plane graph compiled {} passes, expected {} "
        "(visbuffer, resolve, ui_clear, output) -- something is drawing that "
        "should not be",
        frame.passes, kPlanePasses);
    return 1;
  }

  const uint32_t cx = opt.width / 2;
  const uint32_t cy = opt.height / 2;
  auto texel = [&](uint32_t x, uint32_t y, int c) {
    return frame.rgba[(size_t(y) * opt.width + x) * 4 + c];
  };

  // What the SCENE target held before the output pass converted it: the view's
  // own value as a code value. The comparison runs in that space, because that
  // is where every claim in the spec is stated.
  const badlands::color::Rgb sink{texel(cx, cy, 0), texel(cx, cy, 1),
                                  texel(cx, cy, 2)};
  auto expect = [&](badlands::color::Rgb scene_code_value) {
    return badlands::color::EncodeOutputFromSrgb(scene_code_value,
                                                 ToCpuMode(local.present));
  };
  auto agrees = [&](badlands::color::Rgb want, const char* what) {
    if (!exact && !geometry_view) {
      // A material view under an arbitrary pack: the value cannot be predicted,
      // but "something was resolved here" still can be, and it still catches a
      // resolve that drew nothing.
      const badlands::color::Rgb bg = expect(
          {opt.clear[0], opt.clear[1], opt.clear[2]});
      if (std::abs(sink.r - bg.r) < 2e-3f && std::abs(sink.g - bg.g) < 2e-3f &&
          std::abs(sink.b - bg.b) < 2e-3f) {
        spdlog::error(
            "object_viewer: the {} view's centre texel is the background "
            "colour -- nothing was resolved there",
            DebugViews()[size_t(opt.view)].cli);
        return false;
      }
      ++assertions_run;
      return true;
    }
    ++assertions_run;
    const badlands::color::Rgb w = expect(want);
    // +/-1 LSB at 8 bits: rasterization and interpolation differences, and the
    // policy applied to every CPU-vs-GPU value comparison in this stage.
    constexpr float kTol = 1.5f / 255.0f;
    if (std::abs(sink.r - w.r) > kTol || std::abs(sink.g - w.g) > kTol ||
        std::abs(sink.b - w.b) > kTol) {
      spdlog::error(
          "object_viewer: the {} view's centre texel is ({:.4f},{:.4f},{:.4f}) "
          "but {} predicts ({:.4f},{:.4f},{:.4f})",
          DebugViews()[size_t(opt.view)].cli, sink.r, sink.g, sink.b, what,
          w.r, w.g, w.b);
      return false;
    }
    return true;
  };
  auto byte01 = [](uint8_t b) { return float(b) / 255.0f; };

  switch (opt.view) {
    case DebugView::Roughness:
      if (!agrees({byte01(kv.roughness), byte01(kv.roughness),
                   byte01(kv.roughness)}, "the ARM green channel")) return 1;
      break;
    case DebugView::Metallic:
      if (!agrees({byte01(kv.metallic), byte01(kv.metallic),
                   byte01(kv.metallic)}, "the ARM blue channel")) return 1;
      break;
    case DebugView::Ao:
      if (!agrees({byte01(kv.ao), byte01(kv.ao), byte01(kv.ao)},
                  "the ARM red channel")) return 1;
      break;
    case DebugView::Displacement:
      if (!agrees({byte01(kv.displacement), byte01(kv.displacement),
                   byte01(kv.displacement)}, "the displacement map")) return 1;
      break;
    case DebugView::Albedo:
      // The sampler decoded sRGB -> linear and the shader re-encoded, so the
      // scene target holds the SOURCE BYTE. A missing re-encode shows up here
      // as a much darker texel.
      if (!agrees({byte01(kv.albedo[0]), byte01(kv.albedo[1]),
                   byte01(kv.albedo[2])}, "the albedo source texel")) return 1;
      break;
    case DebugView::Normal:
      // CLOSED FORM. A flat normal map over a plane whose geometric normal is
      // +y must resolve to exactly (0,1,0), which encodes to (0.5, 1, 0.5).
      // This is the assertion that catches a transposed tangent frame, a
      // flipped green channel, or a normal read from the wrong float4 lane.
      if (!agrees({0.5f, 1.0f, 0.5f}, "a flat normal over a +y plane")) return 1;
      break;
    case DebugView::Barycentric: {
      // The three weights must sum to 1 wherever the surface is covered. That
      // holds for every pixel, so it is checked across the image rather than at
      // one point -- a resolve returning a constant would satisfy a single
      // sample.
      size_t checked = 0, black = 0;
      float worst = 0.0f;
      uint32_t worst_x = 0, worst_y = 0;
      for (uint32_t y = 0; y < opt.height; ++y) {
        for (uint32_t x = 0; x < opt.width; ++x) {
          // DECODED BACK TO THE SCENE TARGET first. The partition holds where
          // the resolve wrote it; in the sink it does not, because the
          // primaries matrix has rows summing to 1 (preserving neutrals) but
          // columns that do not, and a sum across channels depends on the
          // columns. Asserting on sink values measured the transform instead of
          // the resolve, and read as a 2.4% error in the barycentrics.
          const badlands::color::Rgb scene =
              badlands::color::DecodeSinkToScene(
                  {texel(x, y, 0), texel(x, y, 1), texel(x, y, 2)},
                  ToCpuMode(local.present));
          const float s = scene.r + scene.g + scene.b;
          // BLACK IS NOT BACKGROUND, and this is what stops the test being
          // blind to the failure it exists for. The background sums to 0.20;
          // a weight triple that came out NaN or wildly out of range saturates
          // to (0,0,0) and sums to 0 -- which the "s < 0.3" skip below would
          // quietly treat as "not on the surface". A near-plane blowup would
          // therefore have passed silently.
          if (s < 0.05f) {
            ++black;
            continue;
          }
          if (s < 0.3f) continue;
          // INTERIOR PIXELS ONLY. The shader saturates the weights, so at a
          // pixel the rasterizer assigned to this triangle but whose centre
          // falls just outside it, one weight is slightly negative, gets
          // clamped to 0, and the sum comes up short. That is correct
          // behaviour at an edge and says nothing about the derivation, so the
          // partition is asserted where all three weights are comfortably
          // positive.
          const float lo = std::min({scene.r, scene.g, scene.b});
          if (lo < 0.03f) continue;
          ++checked;
          if (std::abs(s - 1.0f) > worst) {
            worst = std::abs(s - 1.0f);
            worst_x = x;
            worst_y = y;
          }
        }
      }
      if (checked < 50) {
        spdlog::error(
            "object_viewer: only {} barycentric samples were on the surface -- "
            "the plane did not cover enough of the frame to check", checked);
        return 1;
      }
      ++assertions_run;
      // One 8-bit quantization per channel, and nothing else: under Srgb the
      // output transform is the identity, so three half-LSB errors bound the
      // sum's deviation at 3/510 = 0.006. Twice that leaves headroom without
      // admitting anything geometric -- a triangle whose vertex is behind the
      // eye produces weights wrong by whole numbers or NaN, not by 1%.
      if (worst > 0.012f) {
        spdlog::error(
            "object_viewer: barycentric weights at ({},{}) are off by {:.4f} "
            "from summing to 1 -- they are not a partition of the triangle",
            worst_x, worst_y, worst);
        return 1;
      }
      if (black > 0) {
        spdlog::error(
            "object_viewer: {} texels have all-zero barycentric weights -- "
            "those are neither background nor a partition, so the derivation "
            "produced NaN or out-of-range values there (a triangle straddling "
            "the near plane is the expected cause)", black);
        return 1;
      }
      spdlog::info(
          "object_viewer: {} interior barycentric triples sum to 1, worst "
          "deviation {:.4f} at ({},{}), no degenerate texels", checked, worst,
          worst_x, worst_y);
      break;
    }
    case DebugView::Depth: {
      // An ORDERING between two GPU values, so no tolerance: white is near, so
      // a lower row (nearer, with the camera pitched down) must be brighter.
      // An epsilon here would only hide a real inversion.
      float near_v = -1, far_v = -1;
      for (uint32_t y = opt.height; y-- > 0;) {
        if (texel(cx, y, 0) > 0.15f) { near_v = texel(cx, y, 0); break; }
      }
      for (uint32_t y = 0; y < opt.height; ++y) {
        if (texel(cx, y, 0) > 0.15f) { far_v = texel(cx, y, 0); break; }
      }
      if (near_v < 0 || far_v < 0 || !(near_v > far_v)) {
        spdlog::error(
            "object_viewer: the depth view reads {:.4f} nearest and {:.4f} "
            "farthest -- white must be near", near_v, far_v);
        return 1;
      }
      spdlog::info("object_viewer: depth {:.3f} (near) > {:.3f} (far)", near_v,
                   far_v);
      ++assertions_run;
      break;
    }
    case DebugView::TriangleId: {
      // The hash of the triangle the CPU ray/plane intersection names. This is
      // the check that the whole index-buffer-to-screen chain is sound: a
      // resolve that fetched the wrong DrawInfo, added first_index wrong, or
      // read the indices at the wrong stride lands on a different triangle and
      // a completely different colour.
      const Camera cam =
          opt.near_plane_camera ? NearPlaneCamera() : CameraFor(Scene::Plane);
      const int tri = PlaneTriangleUnderPixel(cam, cx, cy, opt.width,
                                              opt.height, 5.0f);
      if (tri < 0) {
        spdlog::error("object_viewer: the centre pixel is not on the plane");
        return 1;
      }
      badlands::color::Rgb want{};
      for (int c = 0; c < 3; ++c) {
        const float phase = c == 0 ? 0.0f : (c == 1 ? 0.33f : 0.67f);
        const float v = 0.5f + 0.5f * std::cos(6.2831853f *
                                               (float(tri) * 0.113f + phase));
        (&want.r)[c] = v;
      }
      if (!agrees(want, "the hash of the CPU-named triangle")) return 1;
      break;
    }
    case DebugView::Lit: {
      // THE PORT PROOF. Every other assertion here would pass against a Lambert
      // term or a dropped retroreflection lobe -- they check that a surface was
      // lit, not that these equations lit it. This runs a CPU transcription of
      // the WESL originals on the same inputs and requires the same answer.
      const Camera cam =
          opt.near_plane_camera ? NearPlaneCamera() : CameraFor(Scene::Plane);
      const int tri = PlaneTriangleUnderPixel(cam, cx, cy, opt.width,
                                              opt.height, 5.0f);
      if (tri < 0) {
        spdlog::error("object_viewer: the centre pixel is not on the plane");
        return 1;
      }
      // Everything the shader had, recomputed. The constant pack is what makes
      // this possible at all: albedo, roughness and AO are the same everywhere,
      // so no UV or mip has to be predicted.
      const glm::vec3 world = PlaneHitPoint(cam, cx, cy, opt.width, opt.height);
      const glm::vec3 N{0.0f, 1.0f, 0.0f};  // flat map over a flat plane
      const glm::vec3 V = glm::normalize(cam.position - world);
      const glm::vec3 albedo_linear{
          badlands::color::SrgbToLinear(byte01(kv.albedo[0])),
          badlands::color::SrgbToLinear(byte01(kv.albedo[1])),
          badlands::color::SrgbToLinear(byte01(kv.albedo[2]))};
      SunSettings sun;
      sun.intensity = local.sun_intensity;
      glm::vec4 sh[9]{};
      sh[0] = glm::vec4(0.12f, 0.14f, 0.18f, 0.0f);  // ResolvePass's default

      const glm::vec3 lit = cpu::ShadeStandard(
          albedo_linear, N, V, byte01(kv.roughness), byte01(kv.ao),
          byte01(kv.metallic), 1.0f, glm::vec3(0.0f),
          badlands::object_viewer::SunDirection(sun),
          sun.color * sun.intensity, sh);
      // THROUGH THE SHARED PREDICATE, not unconditionally. The shader skips
      // the curve on an extended-range surface, so an oracle that always
      // applied it failed --present edr while accusing the ported BRDF.
      const glm::vec3 mapped =
          badlands::color::AppliesTonemap(true, ToCpuMode(local.present))
              ? lit / (lit + 1.0f)
              : lit;
      if (!agrees({badlands::color::LinearToSrgb(mapped.x),
                   badlands::color::LinearToSrgb(mapped.y),
                   badlands::color::LinearToSrgb(mapped.z)},
                  "a CPU port of ShadeStandard on the same inputs")) {
        return 1;
      }

      // AND AGAIN WITH THE SUN OFF. With the sun at full strength the ambient
      // is a small fraction of the result, and a wrong SH convention -- the
      // convolution applied twice, which is the specific trap sh_lighting.slang
      // warns about -- moved this texel by exactly one LSB and passed inside
      // the tolerance. Sun off, the SH term is the entire signal.
      // Also only against the constant pack: it compares an exact albedo.
      if (exact && local.sun_intensity > 0.0f) {
        Options ambient_only = local;
        ambient_only.sun_intensity = 0.0f;
        Frame dark;
        if (!RenderOnce(device, ambient_only, ambient_only.present, *compiler,
                        dark)) {
          return 1;
        }
        const glm::vec3 amb = cpu::ShadeStandard(
            albedo_linear, N, V, byte01(kv.roughness), byte01(kv.ao),
            byte01(kv.metallic), 1.0f, glm::vec3(0.0f),
            badlands::object_viewer::SunDirection(sun), glm::vec3(0.0f), sh);
        const glm::vec3 amb_mapped =
            badlands::color::AppliesTonemap(true, ToCpuMode(local.present))
                ? amb / (amb + 1.0f)
                : amb;
        const badlands::color::Rgb want = badlands::color::EncodeOutputFromSrgb(
            {badlands::color::LinearToSrgb(amb_mapped.x),
             badlands::color::LinearToSrgb(amb_mapped.y),
             badlands::color::LinearToSrgb(amb_mapped.z)},
            ToCpuMode(local.present));
        const size_t c = (size_t(cy) * opt.width + cx) * 4;
        constexpr float kTol = 1.5f / 255.0f;
        if (std::abs(dark.rgba[c] - want.r) > kTol ||
            std::abs(dark.rgba[c + 1] - want.g) > kTol ||
            std::abs(dark.rgba[c + 2] - want.b) > kTol) {
          spdlog::error(
              "object_viewer: with the sun off the centre texel is "
              "({:.4f},{:.4f},{:.4f}) but the CPU SH evaluation predicts "
              "({:.4f},{:.4f},{:.4f}) -- the ambient convention differs",
              dark.rgba[c], dark.rgba[c + 1], dark.rgba[c + 2], want.r, want.g,
              want.b);
          return 1;
        }
        spdlog::info("object_viewer: the SH ambient agrees with the sun off");
      }
      break;
    }
    default:
      break;
  }

  if (assertions_run == 0) {
    spdlog::error(
        "object_viewer: the {} view ran ZERO assertions -- a run that checks "
        "nothing must not report success",
        DebugViews()[size_t(opt.view)].cli);
    return 1;
  }
  spdlog::info("object_viewer: {} view OK ({} assertion(s)) at the centre "
               "texel ({:.4f},{:.4f},{:.4f})",
               DebugViews()[size_t(opt.view)].cli, assertions_run, sink.r,
               sink.g, sink.b);
  badlands_write_png(opt.out.c_str(),
                     [&] {
                       static std::vector<uint8_t> px;
                       px.assign(frame.rgba.size(), 0);
                       for (size_t i = 0; i < frame.rgba.size(); ++i) {
                         px[i] = badlands::color::ToByte(frame.rgba[i]);
                       }
                       return px.data();
                     }(),
                     opt.width, opt.height);
  return 0;
}

// THE GRADIENT TEST: are the analytic UV derivatives actually selecting mips?
//
// TWO-SIDED, because each side alone is satisfiable by a different bug. The
// checker pack's albedo is a one-texel checkerboard, so mip 0 is maximum
// contrast and every level above it is flat grey:
//
//   * the FAR band must be smooth -- a resolve sampling mip 0 everywhere
//     aliases into noise there, and asserting only that would be satisfied by
//     one that blurred the whole image;
//   * the NEAR band must be sharp -- a resolve clamped to a high mip is smooth
//     everywhere, and asserting only that would be satisfied by mip 0.
//
// Measured as mean absolute difference between horizontally adjacent texels,
// which needs no reference image and no golden data.
int RunGradientSelfTest(IRhiDevice& device, const Options& opt) {
  Options local = opt;
  local.scene = Scene::Plane;
  local.pack = "checker";
  local.view = DebugView::Albedo;
  // Identity transform, so the contrast measured is the resolve's and not the
  // primaries matrix's.
  local.present = rhi::ColorSpace::Srgb;

  auto compiler = MakeCompiler();
  if (!compiler) return 1;
  Frame frame;
  if (!RenderOnce(device, local, local.present, *compiler, frame)) return 1;

  auto lum = [&](uint32_t x, uint32_t y) {
    return frame.rgba[(size_t(y) * opt.width + x) * 4];
  };
  // Mean |difference| between neighbours over a horizontal band, ignoring
  // texels the plane does not cover.
  auto contrast = [&](uint32_t y0, uint32_t y1) {
    double sum = 0;
    size_t n = 0;
    for (uint32_t y = y0; y < y1; ++y) {
      for (uint32_t x = 1; x < opt.width; ++x) {
        // The background is a fixed dark blue; the checker is greyscale, so a
        // covered texel has r == b. That separates them without a second pass.
        const size_t i = (size_t(y) * opt.width + x) * 4;
        if (std::abs(frame.rgba[i] - frame.rgba[i + 2]) > 0.01f) continue;
        sum += std::abs(lum(x, y) - lum(x - 1, y));
        ++n;
      }
    }
    return n ? sum / double(n) : -1.0;
  };

  // The camera pitches down at the plane, so lower rows are nearer.
  const double near_c = contrast(opt.height * 3 / 4, opt.height);
  const double far_c = contrast(opt.height / 3, opt.height / 2);
  if (near_c < 0 || far_c < 0) {
    spdlog::error(
        "object_viewer: the gradient test found no covered texels in one of "
        "its bands (near {:.4f}, far {:.4f})", near_c, far_c);
    return 1;
  }
  spdlog::info("object_viewer: checker contrast near {:.4f}, far {:.4f}",
               near_c, far_c);

  if (far_c > 0.05) {
    spdlog::error(
        "object_viewer: the far band has contrast {:.4f} -- a grazing surface "
        "must resolve to a smooth mip, so this is mip 0 being sampled "
        "regardless of the analytic gradients", far_c);
    return 1;
  }
  if (near_c < 0.10) {
    spdlog::error(
        "object_viewer: the near band has contrast {:.4f} -- head-on texels "
        "must keep the checker's detail, so this is an over-high mip being "
        "selected everywhere", near_c);
    return 1;
  }
  if (near_c < far_c * 3.0) {
    spdlog::error(
        "object_viewer: near contrast {:.4f} is not meaningfully above far "
        "{:.4f} -- the mip is not varying with distance", near_c, far_c);
    return 1;
  }
  return 0;
}

// THE HDR GATE: does a value above SDR white actually survive to the surface?
//
// This is the whole point of the float scene target, and it is invisible in
// every other assertion here -- an 8-bit scene clamps the lit result inside the
// resolve, and the frame still looks entirely plausible. Only a float sink can
// show the difference, and only by measuring the range rather than the picture.
//
// TWO-SIDED, because each half alone is satisfiable by the opposite mistake:
// the bright region must exceed 1, and the SAME scene under a dim sun must not.
// Asserting only the first would pass against a shader that scaled everything
// up; only the second, against one that clamped.
int RunHdrSelfTest(IRhiDevice& device, const Options& opt) {
  auto compiler = MakeCompiler();
  if (!compiler) return 1;

  Options bright = opt;
  bright.scene = Scene::Plane;
  bright.pack = "test";
  bright.view = DebugView::Lit;
  bright.present = rhi::ColorSpace::ExtendedLinearDisplayP3;
  bright.sun_intensity = 40.0f;  // well past what any tone curve maps under 1

  Frame hdr;
  if (!RenderOnce(device, bright, bright.present, *compiler, hdr)) return 1;

  Options dim = bright;
  dim.sun_intensity = 0.2f;
  Frame sdr;
  if (!RenderOnce(device, dim, dim.present, *compiler, sdr)) return 1;

  auto peak = [](const Frame& f) {
    float m = 0.0f;
    for (size_t i = 0; i < f.rgba.size(); i += 4) {
      m = std::max({m, f.rgba[i], f.rgba[i + 1], f.rgba[i + 2]});
    }
    return m;
  };
  const float bright_peak = peak(hdr);
  const float dim_peak = peak(sdr);
  spdlog::info("object_viewer: EDR peak {:.3f} at sun 40, {:.3f} at sun 0.2",
               bright_peak, dim_peak);

  // THE OVERLAY MUST NOT COST HEADROOM WHERE IT BARELY COVERS.
  //
  // Compositing in ENCODED space requires encoding the scene, and encoding
  // CLAMPS -- so the guarded-on-any-alpha version destroyed a texel's entire
  // HDR value for a 1/255 antialias fringe, drawing a hard dark ring around
  // every ImGui window, letter and debug line on an HDR display. The extended
  // -range path composites in linear instead, so the scene keeps its value
  // scaled by coverage.
  //
  // Asserted at PARTIAL alpha specifically: the two composites agree at 0 and
  // at 1, so a test that only looked at covered-or-not would pass either way.
  {
    Options with_ui = bright;
    with_ui.force_overlay = true;  // the grid OVER the lit plane
    Frame overlaid;
    if (!RenderOnce(device, with_ui, with_ui.present, *compiler, overlaid)) {
      return 1;
    }
    size_t partial = 0, kept_headroom = 0;
    float worst = 0.0f;
    for (size_t i = 0; i < overlaid.rgba.size(); i += 4) {
      const float a = overlaid.overlay[i + 3];
      if (a <= 0.0f || a >= 1.0f) continue;  // agree by construction
      ++partial;
      const float v = std::max({overlaid.rgba[i], overlaid.rgba[i + 1],
                                overlaid.rgba[i + 2]});
      if (v > 1.0f) ++kept_headroom;
      worst = std::max(worst, v);
    }
    if (partial == 0) {
      spdlog::error(
          "object_viewer: no partially-covered texel in the overlay -- this "
          "check would assert nothing about the case it exists for");
      return 1;
    }
    if (kept_headroom == 0) {
      spdlog::error(
          "object_viewer: all {} partially-covered texels are at or below SDR "
          "white (brightest {:.3f}) -- the overlay composite is clamping the "
          "scene's headroom wherever the UI touches it at all",
          partial, worst);
      return 1;
    }
    spdlog::info(
        "object_viewer: {} of {} partially-covered texels keep headroom "
        "(brightest {:.3f})", kept_headroom, partial, worst);
  }

  if (!(bright_peak > 1.0f)) {
    spdlog::error(
        "object_viewer: the brightest texel is {:.3f} -- nothing exceeds SDR "
        "white, so the scene was clamped before it reached the surface and the "
        "float target is buying nothing",
        bright_peak);
    return 1;
  }
  if (dim_peak > 1.0f) {
    spdlog::error(
        "object_viewer: a dim sun still peaks at {:.3f} -- values above 1 are "
        "not headroom, something is scaling the whole image",
        dim_peak);
    return 1;
  }
  return 0;
}

// THE PER-FRAME RING GATE: do consecutive frames write to DIFFERENT bytes?
//
// The hazard is a GPU read/write race, and it is invisible in a rendered image
// until frames actually overlap -- then it shows as intermittent smearing along
// triangle edges while the camera moves, which reads as a broken resolve rather
// than as a synchronisation bug. So it is asserted structurally instead: with
// three frames in flight, three consecutive frames must land on three distinct
// offsets, because the fourth is the first that is safe to reuse.
//
// rhi_lab asserts the same property for its uniforms via frame_offsets_seen and
// imgui_impl_rhi via ImGui_ImplRHI_LastIndexOffset; this is the third instance
// of one pattern, not a new one.
int RunFrameRingSelfTest(IRhiDevice& device, const Options& opt) {
  auto compiler = MakeCompiler();
  if (!compiler) return 1;
  auto pack = MakeConstantPack(device, badlands::object_viewer::TestPackValues{});
  if (!pack) return 1;
  auto vis = VisbufferPass::Create(device, *compiler, BuildPlaneMesh(),
                                   opt.width, opt.height);
  auto resolve = ResolvePass::Create(device, *compiler, *pack, kSceneFormat);
  auto output = OutputPass::Create(device, *compiler, Format::RGBA8Unorm);
  if (!vis || !resolve || !output) return 1;

  const Camera cam = CameraFor(Scene::Plane);
  const float aspect = float(opt.width) / float(opt.height);

  std::vector<uint32_t> vis_offsets, resolve_offsets, output_offsets;
  for (int i = 0; i < 3; ++i) {
    device.BeginFrame();
    vis->BeginFrame(device.CurrentFrame());
    resolve->BeginFrame(device.CurrentFrame());
    output->BeginFrame(device.CurrentFrame());
    // A DIFFERENT camera each frame, because a ring that reuses one slot is
    // only observable when the CONTENTS differ -- which is exactly the case
    // that corrupts, and exactly what moving the camera produces.
    Camera moved = cam;
    moved.position.z += float(i);
    vis->SetCamera(moved.View(), moved.Proj(aspect), moved.position);
    resolve->SetCamera(moved.View(), moved.Proj(aspect), moved.position,
                       moved.near_m, moved.far_m);

    RenderGraph graph(device);
    if (!vis->AddToGraph(graph)) { device.EndFrame(); return 1; }
    auto scene = MakeSceneTarget(device, opt.width, opt.height);
    if (!scene) { device.EndFrame(); return 1; }
    auto scene_h = graph.ImportTexture(scene.get(), ResourceState::Undefined,
                                       "scene");
    if (!scene_h.IsValid() ||
        !resolve->AddToGraph(graph, vis->VisbufferHandle(), vis->Visbuffer(),
                             vis->VerticesHandle(), vis->IndicesHandle(),
                             vis->DrawsHandle(), scene_h, vis->Vertices(),
                             vis->Indices(), vis->Draws())) {
      device.EndFrame();
      return 1;
    }
    // The output params too. Its value CHANGES per frame here (the tonemap
    // flag alternates), which is exactly the case that used to race: the flag
    // flips when the user picks a different debug view, and the write landed in
    // bytes two submitted frames were still reading.
    auto ui = MakeUiTarget(device, opt.width, opt.height);
    auto sink = device.CreateTexture({.width = opt.width,
                                      .height = opt.height,
                                      .format = Format::RGBA8Unorm,
                                      .usage = TextureUsage::RenderTarget,
                                      .label = "ring_sink"});
    if (!ui || !sink) { device.EndFrame(); return 1; }
    auto ui_h = graph.ImportTexture(ui.get(), ResourceState::Undefined, "ui");
    auto sink_h =
        graph.ImportTexture(sink.get(), ResourceState::Undefined, "sink");
    const float transparent[4] = {0, 0, 0, 0};
    graph.AddRasterPass("ui_clear")
        .ColorTarget(ui_h, LoadOp::Clear, StoreOp::Store, transparent)
        .Execute([](const RasterContext&) {});
    if (!ui_h.IsValid() || !sink_h.IsValid() ||
        !output->AddToGraph(graph, scene_h, scene.get(), ui_h, ui.get(),
                            sink_h, rhi::ColorSpace::DisplayP3, i % 2 == 0)) {
      device.EndFrame();
      return 1;
    }

    vis_offsets.push_back(vis->LastFrameOffset());
    resolve_offsets.push_back(resolve->LastFrameOffset());
    output_offsets.push_back(output->LastFrameOffset());
    device.EndFrame();
  }
  device.WaitIdle();

  auto distinct = [&](const std::vector<uint32_t>& o, const char* what) {
    for (size_t a = 0; a + 1 < o.size(); ++a) {
      for (size_t b = a + 1; b < o.size(); ++b) {
        if (o[a] == o[b]) {
          spdlog::error(
              "object_viewer: the {} uniform used offset {} on both frame {} "
              "and frame {} -- with 3 frames in flight, the GPU may still be "
              "reading the first when the second is written",
              what, o[a], a, b);
          return false;
        }
      }
    }
    return true;
  };
  if (!distinct(vis_offsets, "visbuffer")) return 1;
  if (!distinct(resolve_offsets, "resolve")) return 1;
  if (!distinct(output_offsets, "output params")) return 1;
  spdlog::info(
      "object_viewer: 3 frames used distinct ring offsets -- visbuffer {} {} "
      "{}, resolve {} {} {}, output {} {} {}",
      vis_offsets[0], vis_offsets[1], vis_offsets[2], resolve_offsets[0],
      resolve_offsets[1], resolve_offsets[2], output_offsets[0],
      output_offsets[1], output_offsets[2]);
  return 0;
}


// The sphere chart, asserted through the ROUGHNESS debug view.
//
// One render proves two mechanisms at once, which is why this is the check
// rather than "some texels changed":
//
//   * the INSTANCE ID reached the packing -- fourteen spheres are one instanced
//     draw, so if the fragment shader still wrote slot 0 every sphere would
//     report the same material;
//   * the OVERRIDE MASK is honoured -- the pack is constant, so any roughness
//     other than the pack's own can only have come from DrawInfo.
//
// A sweep that is present but flat, or shifted by one column, fails here. A
// blank frame fails harder.
int RunSphereGridCheck(IRhiDevice& device, const Options& opt) {
  namespace ov = badlands::object_viewer;
  auto compiler = MakeCompiler();
  if (!compiler) return 1;

  Options local = opt;
  local.scene = Scene::Spheres;
  local.view = DebugView::Roughness;
  // The constant pack, so the ONLY source of a varying roughness is the
  // per-instance override. With a real pack the ARM map would vary too and the
  // assertion could not tell the two apart.
  local.pack = "test";
  Frame frame;
  if (!RenderOnce(device, local, local.present, *compiler, frame)) return 1;

  const float aspect = float(local.width) / float(std::max(1u, local.height));
  // THE SAME camera RenderOnce just used. Recomputed from the same function
  // rather than a fixed one, because the framing depends on the aspect.
  const Camera cam = SphereCamera(aspect);
  const glm::mat4 vp = cam.Proj(aspect) * cam.View();

  int failures = 0;
  for (uint32_t row = 0; row < ov::kMetallicSteps; ++row) {
    float previous = -1.0f;
    for (uint32_t col = 0; col < ov::kRoughnessSteps; ++col) {
      // Camera-OFFSET, the same convention the passes use.
      const glm::vec3 centre =
          ov::SphereGridCenter(col, row) - cam.position;
      const glm::vec4 clip = vp * glm::vec4(centre, 1.0f);
      if (clip.w <= 0.0f) {
        spdlog::error("object_viewer: sphere ({},{}) is behind the camera", col,
                      row);
        return 1;
      }
      const float ndc_x = clip.x / clip.w;
      const float ndc_y = clip.y / clip.w;
      const int px = int((ndc_x * 0.5f + 0.5f) * float(local.width));
      const int py = int((1.0f - (ndc_y * 0.5f + 0.5f)) * float(local.height));
      if (px < 0 || py < 0 || px >= int(local.width) ||
          py >= int(local.height)) {
        spdlog::error("object_viewer: sphere ({},{}) projects off screen to "
                      "({},{}) -- the chart does not fit the framing",
                      col, row, px, py);
        return 1;
      }

      // The roughness view emits the channel as a CODE VALUE, so the byte is
      // the roughness. Sampled at the sphere's centre, which faces the camera.
      const float got = frame.rgba[(size_t(py) * local.width + px) * 4];
      const float want = ov::SphereRoughness(col);
      if (std::abs(got - want) > 0.04f) {
        spdlog::error(
            "object_viewer: sphere ({},{}) shows roughness {:.3f} at ({},{}), "
            "expected {:.3f} -- the instance id or the override mask is not "
            "reaching the resolve",
            col, row, got, px, py, want);
        ++failures;
      }
      if (got <= previous) {
        spdlog::error(
            "object_viewer: roughness did not increase across row {} at column "
            "{} ({:.3f} after {:.3f}) -- the sweep is flat or mis-ordered",
            row, col, got, previous);
        ++failures;
      }
      previous = got;
    }
  }
  if (failures > 0) return 1;
  spdlog::info(
      "object_viewer: the sphere chart sweeps roughness across {} columns and "
      "{} rows, {} instances in one draw",
      ov::kRoughnessSteps, ov::kMetallicSteps, ov::kSphereCount);

  if (opt.out.empty()) return 0;

  // The ASSERTION above is always the roughness sweep; the IMAGE is whatever
  // was asked for. Writing the roughness frame under --debug-view lit would be
  // a flag accepted and ignored -- and the one that silently hands back a
  // different picture than the one requested is the worst kind, because the
  // picture looks fine.
  if (opt.view != DebugView::Roughness || opt.pack != local.pack) {
    Options render = opt;
    render.scene = Scene::Spheres;
    if (!RenderOnce(device, render, render.present, *compiler, frame)) return 1;
  }
  std::vector<uint8_t> pixels(frame.rgba.size(), 0);
  for (size_t i = 0; i < frame.rgba.size(); ++i) {
    pixels[i] = badlands::color::ToByte(frame.rgba[i]);
  }
  badlands_write_png(opt.out.c_str(), pixels.data(), opt.width, opt.height);
  return 0;
}

int RunHeadless(IRhiDevice& device, const Options& opt) {
  if (opt.self_test_output) return RunOutputSelfTest(device, opt);
  if (opt.self_test_frame_ring) return RunFrameRingSelfTest(device, opt);
  if (opt.self_test_hdr) return RunHdrSelfTest(device, opt);
  if (opt.self_test_gradients) return RunGradientSelfTest(device, opt);
  if (opt.self_test_visbuffer) return RunPlaneVisbufferCheck(device, opt);
  if (opt.scene == Scene::Plane) return RunDebugViewCheck(device, opt);
  if (opt.scene == Scene::Spheres) return RunSphereGridCheck(device, opt);

  auto compiler = MakeCompiler();
  if (!compiler) return 1;
  Frame frame;
  if (!RenderOnce(device, opt, opt.present, *compiler, frame)) return 1;

  // Back to bytes for the assertions and the PNG. The extended-range sink can
  // exceed 1.0, which saturates here -- acceptable, because a PNG cannot hold
  // it either and the two-sink test is what checks that range.
  std::vector<uint8_t> pixels(frame.rgba.size(), 0);
  for (size_t i = 0; i < frame.rgba.size(); ++i) {
    pixels[i] = badlands::color::ToByte(frame.rgba[i]);
  }

  // THE ASSERTION, and the reason the headless ctest means anything. Exit
  // status IS the check; there is no test framework around this, and writing a
  // PNG and exiting 0 would pass just as well against a graph that recorded no
  // pass at all.
  //
  // Predicted THROUGH THE OUTPUT TRANSFORM rather than from the authored value:
  // the clear colour is written into the scene target in encoded space and then
  // converted, so under DisplayP3 it does not arrive as `clear * 255`. An
  // oracle that ignored that would pin the transform to the identity and quietly
  // stop testing it.
  const auto want_rgba = badlands::color::ExpectedSinkByte(
      {opt.clear[0], opt.clear[1], opt.clear[2]}, opt.clear[3],
      ToCpuMode(opt.present));
  const uint8_t want[4] = {want_rgba.r, want_rgba.g, want_rgba.b, want_rgba.a};
  auto at = [&](uint32_t x, uint32_t y) {
    return &pixels[(size_t(y) * opt.width + x) * 4];
  };
  auto is_clear = [&](const uint8_t* p) {
    for (int c = 0; c < 4; ++c) {
      if (std::abs(int(p[c]) - int(want[c])) > 1) return false;
    }
    return true;
  };

  if (opt.scene == Scene::Clear) {
    for (size_t i = 0; i < pixels.size(); i += 4) {
      if (!is_clear(&pixels[i])) {
        spdlog::error(
            "object_viewer: texel {} is rgba({},{},{},{}) but the graph was "
            "asked to clear to rgba({},{},{},{})",
            i / 4, pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3],
            want[0], want[1], want[2], want[3]);
        return 1;
      }
    }
    spdlog::info("object_viewer: every texel is rgba({},{},{},{})", want[0],
                 want[1], want[2], want[3]);
  } else if (opt.scene == Scene::Grid) {
    // The grid is what the viewer actually shows, so it is rendered rather than
    // trusted. Its assertion is deliberately weak -- it is a picture, not a
    // measurement -- but "a lot of texels changed and the origin is lit" still
    // separates a drawn grid from a blank frame.
    size_t lit = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
      if (!is_clear(&pixels[i])) ++lit;
    }
    if (lit < pixels.size() / 4 / 100) {
      spdlog::error("object_viewer: only {} lit texels -- the grid is missing",
                    lit);
      return 1;
    }
    spdlog::info("object_viewer: grid drew {} lit texels", lit);
  } else {
    // The segment spans world x in [-5, 5] at y = 0, under an orthographic
    // camera of half-height 10. So it covers the middle ROW and the middle
    // HALF of the width, and nothing else -- three claims, each falsifiable.
    const uint32_t mid_y = opt.height / 2;
    const uint32_t centre_x = opt.width / 2;
    size_t lit = 0;
    for (uint32_t x = 0; x < opt.width; ++x) {
      if (!is_clear(at(x, mid_y))) ++lit;
    }
    const uint8_t* centre = at(centre_x, mid_y);
    if (is_clear(centre)) {
      spdlog::error(
          "object_viewer: the centre texel is still the clear colour -- the "
          "line pass drew nothing");
      return 1;
    }
    // MEASURED, not eyeballed. The segment is pure red at alpha 1, so its core
    // texel is exactly what the output transform makes of (1,0,0) -- the one
    // strongly chromatic value in the suite, which makes this the assertion
    // that pins the sRGB->P3 matrix.
    //
    // Measured against the loose "is it reddish" check this replaces
    // (centre[0] >= 128 && centre[1] <= 64): that check DID catch a transposed
    // matrix, which turns the red green-ward to (234,117,0). It did NOT catch
    // the matrix being dropped altogether -- (255,0,0) satisfies it perfectly,
    // and dropping it is the far likelier mistake.
    const auto want_core = badlands::color::ExpectedSinkByte(
        {1.0f, 0.0f, 0.0f}, 1.0f, ToCpuMode(opt.present));
    const uint8_t core_want[4] = {want_core.r, want_core.g, want_core.b,
                                  want_core.a};
    for (int c = 0; c < 4; ++c) {
      if (std::abs(int(centre[c]) - int(core_want[c])) > 1) {
        spdlog::error(
            "object_viewer: the centre texel is rgba({},{},{},{}) but the "
            "segment's red reaches a {} surface as rgba({},{},{},{})",
            centre[0], centre[1], centre[2], centre[3], ToString(opt.present),
            core_want[0], core_want[1], core_want[2], core_want[3]);
        return 1;
      }
    }
    // Half the width, within the antialias fringe on either end.
    const size_t want_lit = opt.width / 2;
    if (lit < want_lit - 4 || lit > want_lit + 4) {
      spdlog::error(
          "object_viewer: {} lit texels across the middle row, expected about "
          "{} for a segment spanning half the view",
          lit, want_lit);
      return 1;
    }
    // THE FRINGE, which is the only place blending is observable. The shader
    // emits a 1px alpha ramp at the quad's edge; blended, that composites
    // against the clear colour and the target stays opaque. Without blending
    // the shader's own alpha is written straight through, so the fringe texel
    // comes back with alpha 128 instead of 255 -- and every other assertion in
    // this scene passes either way, which is exactly why this one is here.
    bool found_fringe = false;
    for (uint32_t y = 0; y < opt.height; ++y) {
      const uint8_t* p = at(centre_x, y);
      if (is_clear(p)) continue;
      // RECOGNISED THROUGH THE OUTPUT TRANSFORM, not against raw red.
      //
      // This used to be `p[0] > 250 && p[1] < 8 && p[2] < 8`, which described
      // the segment's colour before any conversion. Under the default P3
      // present the core arrives as (234,51,35), so NO texel matched -- the
      // core texels themselves were counted as fringe, found_fringe was always
      // true, and the hard-edge refusal below could never fire. The check that
      // exists to be the one place blending is observable had stopped being
      // able to fail.
      const bool core = std::abs(int(p[0]) - int(core_want[0])) <= 1 &&
                        std::abs(int(p[1]) - int(core_want[1])) <= 1 &&
                        std::abs(int(p[2]) - int(core_want[2])) <= 1 &&
                        p[3] == 255;
      if (core) continue;
      found_fringe = true;
      // THE COLOUR, not the alpha.
      //
      // There used to be an `alpha != 255` check here, described as the one
      // place blending was observable. It is dead twice over: the output pass
      // writes alpha 1.0 unconditionally, so the sink's alpha is always 255 --
      // and the overlay is cleared TRANSPARENT, so a single premultiplied draw
      // with blending disabled writes exactly what blending would have
      // produced. Only OVERLAPPING draws distinguish the two, which is why the
      // overlay's blend state is asserted in imgui_rhi_tests instead.
      //
      // What this checks is narrower and still worth having: that a fringe
      // EXISTS and is a gradient between the background and the core, which is
      // what catches the alpha ramp regressing to a hard edge.
      const bool between_bg_and_core =
          p[0] > want[0] && p[0] < core_want[0];
      if (!between_bg_and_core) {
        spdlog::error(
            "object_viewer: fringe texel at y={} is rgba({},{},{},{}), which is "
            "not between the background red {} and the line's core red {} -- "
            "it is not a blend of the two",
            y, p[0], p[1], p[2], p[3], want[0], core_want[0]);
        return 1;
      }
    }
    if (!found_fringe) {
      spdlog::error(
          "object_viewer: no antialias fringe anywhere in the centre column -- "
          "the line has hard edges");
      return 1;
    }

    // ...and the corners are untouched, so the line is a line and not a fill.
    for (auto [x, y] : {std::pair<uint32_t, uint32_t>{0, 0},
                        {opt.width - 1, 0},
                        {0, opt.height - 1},
                        {opt.width - 1, opt.height - 1}}) {
      if (!is_clear(at(x, y))) {
        spdlog::error("object_viewer: corner ({},{}) is not the clear colour",
                      x, y);
        return 1;
      }
    }
    spdlog::info("object_viewer: {} lit texels across the middle row, corners "
                 "clear", lit);
  }

  badlands_write_png(opt.out.c_str(), pixels.data(), opt.width, opt.height);
  spdlog::info("object_viewer: wrote {} ({}x{})", opt.out, opt.width,
               opt.height);
  return 0;
}

int RunWindowed(IRhiDevice& device, const Options& opt) {
  // BGRA, because that is what CAMetalLayer accepts. prefer_hdr lets the shell
  // upgrade to RGBA16Float / extended-linear P3 when the display reports HDR;
  // the SDR fallback is still tagged P3, so the viewer matches badlands_game on
  // the same display either way.
  // --present DRIVES THIS. It used to be hardcoded to P3 + prefer_hdr, so the
  // flag was honoured headless and silently ignored on screen -- there was no
  // way to look at the untagged path in a window.
  //
  //   srgb -> untagged 8-bit, no upgrade
  //   p3   -> Display P3 8-bit, no upgrade
  //   edr  -> Display P3, upgraded to extended-linear when the DISPLAY has HDR
  auto shell = rhi_app::AppShell::Create(
      device,
      {.title = "badlands object_viewer",
       .width = opt.width,
       .height = opt.height,
       .present_format = Format::BGRA8Unorm,
       .color_space = opt.present == rhi::ColorSpace::Srgb
                          ? rhi::ColorSpace::Srgb
                          : rhi::ColorSpace::DisplayP3,
       // Unasked, a window takes the display's best: P3, upgraded to
       // extended-linear when the display reports HDR. Asked, it gets exactly
       // what was asked for -- including staying SDR, which is the whole point
       // of being able to inspect the other paths on screen.
       .prefer_hdr =
           !opt.present_explicit ||
           opt.present == rhi::ColorSpace::ExtendedLinearDisplayP3});
  if (!shell) return 1;

  // READ BACK, never assumed: the shell may have picked a float format, and the
  // swapchain may have dropped off it again if the layer refused to tag. Only
  // the output pass's pipeline sees this -- every other pass targets the scene
  // texture, which is 8-bit encoded regardless of what is being presented.
  const Format surface_format = shell->SurfaceFormat();
  const rhi::ColorSpace present = shell->SurfaceColorSpace();
  spdlog::info("object_viewer: presenting {} / {}", ToString(surface_format),
               ToString(present));

  // The compiler and the line pass exist only if a scene needs them. --scene
  // clear paid for both before, which is the accepted-and-ignored trap from the
  // other direction: a flag that changed nothing still changed the cost.
  auto compiler = MakeCompiler();
  if (!compiler) return 1;
  std::unique_ptr<LinePass> lines;
  // THE SAME PREDICATE the headless path uses. This used to be
  // `opt.scene != Scene::Clear`, which drew the grid and axes over --scene
  // plane on screen while headless drew neither -- so the windowed frame was
  // not the frame any assertion covered.
  if (SceneHasLines(opt.scene)) {
    // The OVERLAY's format, not the scene's and not the surface's.
    lines = LinePass::Create(device, *compiler, kUiFormat);
    if (!lines) return 1;
  }
  auto output = OutputPass::Create(device, *compiler, surface_format);
  if (!output) return 1;

  // Recreated on resize, so it always matches the surface it is converted onto.
  auto scene = MakeSceneTarget(device, shell->Width(), shell->Height());
  auto ui = MakeUiTarget(device, shell->Width(), shell->Height());
  if (!scene || !ui) return 1;

  // The DEBUG UI surface, distinct from any in-world game UI. imgui_impl_sdl3
  // is the platform half (already vendored); imgui_impl_rhi is the renderer.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  if (!ImGui_ImplSDL3_InitForMetal(shell->Window())) {
    spdlog::error("object_viewer: ImGui_ImplSDL3_InitForMetal failed");
    return 1;
  }
  if (!ImGui_ImplRHI_Init({.device = &device,
                           .compiler = compiler.get(),
                           // The OVERLAY's format. ImGui draws into it in
                           // encoded space and never touches the surface.
                           .target_format = kUiFormat,
                           .framebuffer_width = shell->Width(),
                           .framebuffer_height = shell->Height()})) {
    return 1;
  }

  // The visibility-buffer chain, for --scene plane only.
  std::unique_ptr<MaterialPack> pack;
  std::unique_ptr<VisbufferPass> vis;
  std::unique_ptr<ResolvePass> resolve;
  PlaneChain chain;
  if (SceneUsesVisbuffer(opt.scene)) {
    pack = opt.pack == "test"
               ? MakeConstantPack(device,
                                  badlands::object_viewer::TestPackValues{})
               : (opt.pack == "checker"
                      ? MakeCheckerPack(device)
                      : LoadMaterialPack(device, opt.pack));
    if (!pack) return 1;
    vis = VisbufferPass::Create(device, *compiler, MeshFor(opt.scene),
                                shell->Width(), shell->Height());
    // The SCENE target's format: linear float, so the lit view keeps headroom.
    resolve = ResolvePass::Create(device, *compiler, *pack, kSceneFormat);
    if (!vis || !resolve) return 1;
  }

  Camera cam = CameraFor(opt.scene);
  // What the two debug windows drive. Nothing else in this app reads them, and
  // nothing else writes them -- which is what makes the windows the whole of
  // their interface.
  DebugView view = opt.view;
  SunSettings sun;
  sun.intensity = opt.sun_intensity;
  spdlog::info("object_viewer: WASD/QE to move, wheel to zoom, Esc to quit");

  rhi_app::AppShellCallbacks cb;
  cb.OnEvent = [&](const SDL_Event& e) {
    // Stands in for an ImGui widget holding focus: consume EVERYTHING. If
    // Escape still stops the loop, it is because the shell acted on it before
    // asking.
    if (opt.self_test_escape) return true;
    ImGui_ImplSDL3_ProcessEvent(&e);
    // ImGui gets first refusal on input it is using. Without this the camera
    // also acts on a drag inside a panel, which is the two-surfaces confusion
    // the debug/game UI split exists to avoid.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse &&
        (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL ||
         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
         e.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
      return true;
    }
    if (io.WantCaptureKeyboard && (e.type == SDL_EVENT_KEY_DOWN ||
                                   e.type == SDL_EVENT_KEY_UP)) {
      return true;
    }
    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
      cam.extent =
          std::clamp(cam.extent * (e.wheel.y > 0 ? 0.9f : 1.1f), 1.0f, 200.0f);
      return true;
    }
    return false;
  };
  cb.OnUpdate = [&](const rhi_app::FrameInfo& f) {
    if (opt.self_test_escape && f.index == 3) {
      SDL_Event esc{};
      esc.type = SDL_EVENT_KEY_DOWN;
      esc.key.scancode = SDL_SCANCODE_ESCAPE;
      SDL_PushEvent(&esc);
    }
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    const float step = cam.extent * std::min(f.dt, 0.1f);
    if (f.keys[SDL_SCANCODE_W]) cam.position.z -= step;
    if (f.keys[SDL_SCANCODE_S]) cam.position.z += step;
    if (f.keys[SDL_SCANCODE_A]) cam.position.x -= step;
    if (f.keys[SDL_SCANCODE_D]) cam.position.x += step;
    if (f.keys[SDL_SCANCODE_E]) cam.position.y += step;
    if (f.keys[SDL_SCANCODE_Q]) cam.position.y -= step;
  };
  cb.OnResize = [&](uint32_t w, uint32_t h) {
    // The scene target is sized to the surface, so it is rebuilt here rather
    // than sampled at the wrong size for a frame. Returning false stops the
    // loop: a viewer with no scene target has nothing sane to present.
    scene = MakeSceneTarget(device, w, h);
    ui = MakeUiTarget(device, w, h);
    if (!scene || !ui) {
      spdlog::error("object_viewer: could not rebuild the scene target at {}x{}",
                    w, h);
      return false;
    }
    // The visibility buffer and its depth are screen-sized too, and the
    // resolve's binding table names the visbuffer -- so a resize that rebuilt
    // one and not the other would sample a destroyed texture.
    if (vis && !vis->Resize(w, h)) return false;
    return true;
  };
  cb.OnFrameBegin = [&](uint64_t frame_index) {
    // After BeginFrame, so a SKIPPED frame still recycles its slot.
    if (lines) lines->BeginFrame(frame_index);
    if (vis) vis->BeginFrame(frame_index);
    if (resolve) resolve->BeginFrame(frame_index);
    output->BeginFrame(frame_index);
    ImGui_ImplRHI_NewFrame(frame_index);
  };
  cb.OnRender = [&](ITextureView* target, const rhi_app::FrameInfo& f) {
    const float aspect = float(f.width) / float(std::max(1u, f.height));
    if (lines) {
      // The SAME scene the headless run of this flag would render, so what is
      // on screen and what a test asserts cannot drift.
      const DebugLineBuffer scene =
          opt.scene == Scene::Lines ? LineScene() : GridScene();
      if (!lines->Upload(scene, cam.View(), cam.Proj(aspect),
                         {float(f.width), float(f.height)}, cam.position)) {
        return false;
      }
    }
    // Rebuilt per frame because the drawable is a different texture each time.
    // Cheap at this size, and the alternative -- caching a graph keyed on a
    // resource that changes every frame -- is how a stale view gets rendered
    // into.
    ImGui_ImplRHI_SetFramebufferSize(f.width, f.height);
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    // THE DEBUG UI, and it contains exactly what was approved and nothing else.
    // CLAUDE.md: never add anything to a UI without explicit approval -- no
    // stats block, no pack selector, no tonemap knob, no gizmo toggles. The
    // camera readout and zoom slider that used to live here were removed under
    // that rule; one frame-timing line stays.
    ImGui::Begin("object_viewer");
    ImGui::Text("%u x %u  |  %.1f fps", f.width, f.height,
                f.dt > 0.0f ? 1.0f / f.dt : 0.0f);
    ImGui::End();

    if (resolve) {
      ImGui::Begin("Graphics debug");
      // ONE radio group over the whole enum, driven by the same table the CLI
      // parses. A view added to the enum appears here automatically and gets a
      // --debug-view name for free, which is what stops a UI-only mode existing
      // that no headless assertion covers.
      for (size_t i = 0; i < DebugViews().size(); ++i) {
        // The group headings are labels on the radio list, not extra controls:
        // ten flat entries read as a list of unrelated things.
        if (i == size_t(DebugView::TriangleId)) ImGui::SeparatorText("Visbuffer");
        if (i == size_t(DebugView::Albedo)) ImGui::SeparatorText("Material");
        int current = int(view);
        if (ImGui::RadioButton(std::string(DebugViews()[i].label).c_str(),
                               &current, int(i))) {
          view = DebugView(i);
        }
      }
      ImGui::End();

      ImGui::Begin("Scene lighting");
      ImGui::SeparatorText("Directional");
      ImGui::SliderFloat("azimuth", &sun.azimuth_deg, 0.0f, 360.0f);
      ImGui::SliderFloat("elevation", &sun.elevation_deg, -90.0f, 90.0f);
      ImGui::ColorEdit3("color", &sun.color.x);
      ImGui::SliderFloat("intensity", &sun.intensity, 0.0f, 10.0f);
      ImGui::End();
    }
    ImGui::Render();

    // Rebuilt per frame because the drawable is a different texture each time.
    // Cheap at this size, and the alternative -- caching a graph keyed on a
    // resource that changes every frame -- is how a stale view gets rendered
    // into.
    //
    // The SAME BuildGraph the headless path calls, with the acquired drawable
    // as the sink instead of a plain texture. That is the whole of the sink
    // abstraction, and it is why the headless run is the real path.
    if (chain.Active() || (vis && resolve)) {
      vis->SetCamera(cam.View(), cam.Proj(aspect), cam.position);
      resolve->SetCamera(cam.View(), cam.Proj(aspect), cam.position, cam.near_m,
                         cam.far_m);
      resolve->SetSun(sun);
      resolve->SetView(view);
      chain = {.vis = vis.get(), .resolve = resolve.get()};
    }

    Options local = opt;
    local.present = present;
    // The window drives the view, so the tonemap decision has to read the LIVE
    // value rather than the one the command line started with.
    local.view = view;
    RenderGraph graph(device);
    if (!BuildGraph(graph, scene.get(), ui.get(), target->GetTexture(), local,
                    lines.get(), output.get(), ImGui::GetDrawData(), chain)) {
      return false;
    }
    auto encoder = device.CreateCommandEncoder("frame");
    graph.Execute(*encoder);
    encoder->Finish();
    device.Submit(*encoder);
    return true;
  };

  const auto stats = shell->Run(cb, opt.max_frames);
  if (opt.self_test_escape) {
    // It must have stopped on the Escape, well before the frame cap.
    if (stats.frames_begun >= opt.max_frames) {
      spdlog::error(
          "object_viewer self-test: ran all {} frames -- Escape was swallowed "
          "by the event consumer instead of stopping the loop",
          stats.frames_begun);
      ImGui_ImplRHI_Shutdown();
      ImGui_ImplSDL3_Shutdown();
      ImGui::DestroyContext();
      return 1;
    }
    spdlog::info("object_viewer self-test OK: Escape stopped the loop after {} "
                 "frames", stats.frames_begun);
  }
  ImGui_ImplRHI_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (stats.aborted) return 1;
  if (opt.max_frames > 0 && stats.frames_presented == 0) {
    spdlog::error("object_viewer: ran {} frames and presented none",
                  stats.frames_begun);
    return 1;
  }
  spdlog::info("object_viewer: {} frames, {} presented", stats.frames_begun,
               stats.frames_presented);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, opt)) return 1;

  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true,
                              .label = "object_viewer"});
  if (!device) return 1;

  return opt.headless ? RunHeadless(*device, opt) : RunWindowed(*device, opt);
}
