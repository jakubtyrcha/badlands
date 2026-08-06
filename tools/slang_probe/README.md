# slang_probe

Throwaway harness for the RHI/Slang exploration
(`docs/superpowers/specs/2026-08-03-rhi-slang-exploration.md`). It answers three
questions and is expected to be deleted once they are settled:

- **Probe A** — is runtime Slang compilation viable, or must variants go offline?
- **Probe B** — does Slang reflection produce what the engine consumes, and does it
  survive a target change?
- Probe C (validation) needed no code; its findings are in the doc.

**Deliberately not part of the root build.** The main `CMakeLists.txt` must not gain
a Slang dependency while we are only probing.

## Setup

The Slang SDK is gitignored. Fetch the prebuilt macOS Apple Silicon release:

```sh
mkdir -p third_party/toolchains/slang && cd third_party/toolchains/slang
URL=$(curl -sL https://api.github.com/repos/shader-slang/slang/releases/latest \
  | python3 -c "import sys,json;d=json.load(sys.stdin);print([a['browser_download_url'] for a in d['assets'] if a['name'].endswith('macos-aarch64.tar.gz')][0])")
curl -sL "$URL" -o slang.tar.gz && tar xzf slang.tar.gz && rm slang.tar.gz
```

Probed against Slang **v2026.14.1**.

## Build and run

Run from the repo root — shader paths resolve relative to cwd.

```sh
cmake -S tools/slang_probe -B tools/slang_probe/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/slang_probe/build

./tools/slang_probe/build/slang_probe --timing     # probe A
./tools/slang_probe/build/slang_probe --reflect    # probe B
./tools/slang_probe/build/slang_probe --hotreload  # probe A follow-up
./tools/slang_probe/build/slang_probe --all
```

The WESL baseline in `--timing` is compiled in only when the main build has already
produced `build/libwesl_ffi.a`; it is whatever build type that was, so treat the
comparison as indicative.

Optionally precompile the shared modules first, to measure that path:

```sh
S=third_party/toolchains/slang/bin/slangc
for m in shaders/common/frame_uniforms shaders/common/gbuffer_encode \
         shaders/common/terrain_layers shaders/compute/instance_common; do
  $S -Itools/slang_probe/shaders/common -Itools/slang_probe/shaders/compute \
     tools/slang_probe/$m.slang -o tools/slang_probe/$m.slang-module
done
```

## Ported shaders

Hand-ported from WESL, faithful in structure and size rather than pixel-exact —
the probe measures compile time and reflection, not rendering.

| File | Ported from | Why this one |
|---|---|---|
| `shaders/material/terrain_cluster.slang` | `shaders/material/terrain_cluster.wesl` | The prototype's real target: texture arrays, splat, uniform buffer, four vertex inputs, three G-buffer outputs |
| `shaders/common/frame_uniforms.slang` | `shaders/common/frame.wesl` | The 592-byte frame UBO — checks reflected member offsets |
| `shaders/common/terrain_layers.slang` | `shaders/common/terrain_layers.wesl` | Heaviest function; dominates compile time |
| `shaders/common/gbuffer_encode.slang` | `shaders/common/gbuffer_encode.wesl` | Fragment-output reflection |
| `shaders/compute/instance_classify.slang` | `shaders/compute/instance_classify.wesl` | Compute, atomics, storage buffers, workgroup size |
| `shaders/compute/instance_common.slang` | `shaders/compute/instance_common.wesl` | Struct layouts mirrored in C++ |
| `shaders/material/binding_models.slang` | *(new)* | Probe B follow-up: does `ParameterBlock` give the (group, binding) model? |

### Porting gotchas hit

- `module frame;` collides with that module's own `frame` global — Slang reports an
  ambiguous reference. Renamed to `frame_uniforms`; the WESL original has no
  module-name concept, so this is a real porting hazard rather than style.
- `SampleGrad` takes the sampler as its first argument (HLSL convention), unlike
  WGSL's `textureSampleGrad(t, s, ...)` ordering.
- `[[vk::binding(b, set)]]` is **ignored** for the `metal` and `hlsl` targets. Use
  `ParameterBlock<T>` for grouping — see the doc's probe B findings.
