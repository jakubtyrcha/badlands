# Render graph + object_viewer: design

`object_viewer` is the next iteration of `badlands_viewer`, built on the native RHI
instead of Dawn. It is also the render graph's first client, and the graph does not
exist yet — so the two are specified together, with the app deliberately held to three
passes so the graph is shaped by a real caller without over-fitting to one.

Prior art and constraints: `docs/superpowers/specs/2026-08-03-rhi-slang-exploration.md`
(D7 fixes one-encoder-many-passes; R3 says the graph derives barriers rather than the
RHI tracking state), and `src/engine/rhi/CLAUDE.md` for the rules this inherits.

## Scope

**In:** an empty screen, debug line drawing, and Dear ImGui — each as one graph pass.
Alpha blending in the RHI, because two of those three need it.

**Out, and named so it is not assumed:** transient-resource aliasing, async compute,
multi-queue, bindless, mesh shaders, and any port of the deferred renderer. The graph
gets the interface for transient resources but pools nothing yet.

**Unchanged:** `badlands_viewer` stays alive and Dawn-based until `object_viewer`
catches up. Dawn is still pinned, and the old viewer is ~1,100 lines of working
model/character/LOD tooling that nothing here replaces.

## What transfers, and what does not

Checked rather than assumed:

| Piece | State |
|---|---|
| `rhi_lab`'s windowed loop | Transfers. Swapchain, `AcquireStatus`, `SurfaceSizeTracker`, frame pacing. |
| `DebugLineBuffer` / `ExpandDebugLines` | Transfers untouched. Pure CPU, Dawn-free, already unit-tested. |
| `orbit_camera_controller`, `imgui_mouse_input`, `sim_clock` | Transfer untouched. Dawn-free. |
| `imgui_impl_sdl3` (platform half) | Already built and linked. |
| `imgui_impl_wgpu_custom.cpp` (render half) | Does NOT transfer. Dawn-only; needs an RHI twin. |
| `screenshot_recorder` | Does NOT transfer. Dawn-coupled; headless tests need an RHI readback. |
| `thick_line.wesl` | Does NOT transfer. Needs a Slang port. |

The RHI has no vertex buffers by design — vertices are pulled from a storage buffer by
vertex id. Both ImGui and debug lines work that way, so nothing here needs vertex input
layouts.

## D1. The sink is not a graph concept

The constraint was: the graph must not own the frame loop, must run headless, and should
abstract its output target.

**All three fall out of one decision: the graph never sees a swapchain.** Its output is
an *imported* texture, and the frame loop decides where that texture came from.

```
windowed   acquire the swapchain image -> ImportTexture(image) -> Execute()
headless   create a texture            -> ImportTexture(tex)   -> Execute() -> read back
```

That is the entire abstraction. There is no `ISink`, no `IPresentTarget`, and no
swapchain-shaped hole in the graph — which also means the graph cannot grow a dependency
on `AcquireStatus`, `Skip`/`Lost` handling, or present cadence. Those stay in the loop
that already handles them.

The payoff is testability: **the same graph, with the same passes, runs both ways.** A
headless run is not a special mode, it is the identical code path with a different
imported texture, so a test asserts on the real thing rather than a stand-in.

## D2. Barriers are derived from declared use, and checkable with no GPU

R3 already settled that the graph derives barriers rather than the RHI tracking state.
What makes it *verifiable* is that the RHI already checks declared intent: `Transition`
is a no-op on Metal, and the validation decorator checks it as bookkeeping over the
command stream.

So the graph's hardest correctness property — did it declare the right transitions, in
the right order? — is assertable **on Null, with no GPU, in the fast suite.** A graph
that forgets a transition fails a validation scope; it does not merely render wrong on a
future DX12 machine.

This is the main reason the graph is worth building before DX12 rather than after.

## D3. Passes declare, then record

```cpp
class RenderGraph {
 public:
  // External resources the graph did not create. `state` is what the resource is
  // in on entry, so the first transition is derived rather than assumed.
  ResourceHandle ImportTexture(ITexture* tex, ResourceState state);
  ResourceHandle ImportBuffer(IBuffer* buf, ResourceState state);

  // Graph-owned, lifetime bounded by the graph. Pooled later; allocated
  // per-Compile for now, and that is stated rather than implied.
  ResourceHandle CreateTexture(const TextureDesc& desc);

  RasterPassBuilder AddRasterPass(std::string_view name);
  ComputePassBuilder AddComputePass(std::string_view name);

  // Orders passes, derives transitions, and refuses (after logging) on a cycle,
  // a read of something never written, or a write to an imported read-only.
  bool Compile();

  // Records into a CALLER-OWNED encoder. The graph never submits, never begins
  // a frame, and never presents.
  void Execute(ICommandEncoder& encoder);
};
```

`Execute` taking the caller's encoder is what keeps the frame loop's. One encoder, many
passes, one submit is already the fixed shape (D7).

## D4. Binding is by reflected name

A stated goal was to use reflection instead of manual binding. The RHI already carries
`ShaderReflection` with `FindBinding(name)`, so the graph resolves names to slots and
calls the existing shared `ResolveBindingTable`:

```cpp
pass.Bind("visbuffer", handle)     // resolved against the pipeline's reflection
    .ColorTarget(target, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1})
    .Execute([&](RasterPassContext& ctx) { ctx.pass->Draw(3); });
```

A name the shader does not declare is refused at `Compile()`, not at record time — the
same creation-time-refusal principle as rule 13, for the same reason: the record path
must not be able to encounter an unresolvable binding.

`Bind` also supplies the resource's declared use, which is where the transition in D2
comes from. Declaring a binding and declaring a barrier are therefore the same act, and
cannot drift apart.

## D5. Blend state: full, and fully covered

Chosen shape: per-attachment factors and ops, matching WebGPU's vocabulary (D2 of the
RHI spec). Rule 4 says no advertised-but-unimplemented surface, so **every enumerator
listed here gets a test**, and enumerators with no implementation are not listed.

```cpp
enum class BlendFactor : uint8_t {
  Zero, One,
  Src, OneMinusSrc, SrcAlpha, OneMinusSrcAlpha, SrcAlphaSaturated,
  Dst, OneMinusDst, DstAlpha, OneMinusDstAlpha,
};

enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

struct BlendComponent {
  BlendFactor src = BlendFactor::One;
  BlendFactor dst = BlendFactor::Zero;
  BlendOp op = BlendOp::Add;
};

struct BlendState {
  bool enabled = false;   // false == the current behaviour, exactly
  BlendComponent color;
  BlendComponent alpha;
};
```

`RenderPipelineDesc` gains `std::vector<BlendState> blend_states`, parallel to
`color_formats`. Empty means every attachment is opaque, so existing call sites are
untouched.

**`Constant` / `OneMinusConstant` are deliberately absent.** They require a
blend-constant setter on the render pass, which is interface surface with no caller —
exactly the "exists for later" trap rule 4 forbids. They land with their first user.

**`Min` and `Max` ignore their factors** on both Metal and D3D12. That is a real
asymmetry, so it gets a test asserting the factors are ignored rather than a comment
hoping the reader knows.

### Coverage plan

A pairwise sweep is 11 × 11 × 5 and not worth running. What rule 4 actually demands is
that no exposed enumerator is untested, so:

- **Every factor**, once each, against a fixed partner factor and `Add`.
- **Every op**, once each, with fixed factors.
- **Separate color/alpha components**, so a backend that wires alpha from the colour
  component is caught.
- **`enabled = false`** produces bit-identical output to a pipeline with no blend state
  at all — the regression that matters most, since every existing pass takes that path.

Roughly 30 cases, table-driven, each compared against a **CPU evaluation of the blend
equation** — the same oracle pattern that pinned the splat count. Target is `RGBA8Unorm`
with operands chosen as exact multiples of 1/255 and a ±1 LSB tolerance, because that is
the format ImGui and the line pass actually use.

### Where the checks live

Per rule 13: an invalid blend configuration (a state count that does not match the
attachment count) is refused at `CreateRenderPipeline` on **both** backends, because it
cannot be encoded. Pixel correctness is Metal-only and says so — Null runs no shaders,
so it asserts only that the state reached the backend intact.

## D6. ImGui goes through the RHI

A custom `imgui_impl_rhi`, not the stock `imgui_impl_metal` / `imgui_impl_dx12`.

The stock backends would need the RHI to hand out `id<MTLRenderCommandEncoder>` and
`ID3D12GraphicsCommandList*`, which `src/engine/rhi/CLAUDE.md` makes a compile error on
purpose — and it would put ImGui outside the graph, so ordering it against the game-UI
surface stops being the graph's business. One RHI backend serves Metal, DX12 and Null.

Precedent: `imgui_impl_wgpu_custom.cpp` is 564 lines of exactly this shape for Dawn, so
the work is known rather than estimated.

Mapping:

| ImGui needs | RHI provides |
|---|---|
| index buffer | `BufferUsage::Index` + `SetIndexBuffer` |
| vertex buffer | storage buffer, pulled by `SV_VertexID` |
| font atlas | `SampledTexture` + `Sampler` |
| per-command clip rect | `SetScissor` |
| per-frame growing memory | `FrameAllocator` (usage is caller-specified) |
| alpha blending | D5 |

**ImGui is assertable, and will be asserted.** `GetBackgroundDrawList()->AddRectFilled`
at fixed coordinates plus a pixel readback is a real test, not an eyeball — a rect of a
known colour must land on known pixels, and the scissor must clip it where declared.

## Staging

Each stage adds exactly one pass and one graph capability, and each lands with tests.

**Stage 0 — blend state.** RHI only, no graph, no app. Both backends, the coverage plan
above, plus the `enabled = false` bit-identity regression.

**Stage 1 — empty screen.** `src/engine/graph/` with import, one raster pass, `Compile`,
`Execute`. `src/executables/object_viewer/` with the window, swapchain and loop forked
from `rhi_lab`.
*Tests:* the same graph run headless clears to a known colour and reads back; a
validation scope over `Execute` is clean; removing a declared transition makes it dirty.

**Stage 2 — debug draw.** Second pass, reusing `ExpandDebugLines` unchanged, plus the
Slang port of `thick_line`. First user of `Bind`-by-name and of blending.
*Tests:* a segment between two known world points covers known pixels; an empty buffer
draws nothing; the existing expander tests keep running as they are.

**Stage 3 — ImGui.** `imgui_impl_rhi`, third pass, font atlas, per-command scissor.
*Tests:* a fixed-coordinate filled rect lands on the right pixels in the right colour;
a clip rect actually clips; two windows produce two scissor rects.

## Risks, stated as choices

- **The graph is designed against one small client.** Deliberate: three passes is enough
  to force the sink, the transition derivation and name-based binding, and not enough to
  bake in the deferred renderer's shape. The alternative — designing it against zero
  clients — is what usually gets the sink wrong.
- **Full blend state is more surface than stage 3 needs.** Accepted, on the condition
  above: every enumerator exposed is tested, and the two that cannot be (`Constant`) are
  not exposed.
- **`imgui_impl_rhi` is code we own and upstream does not.** That is the cost of keeping
  the seam sealed; the Dawn version already proved the shape, and it is pinned ImGui, so
  the interface it targets does not move under us.
