# src/crates/ — Rust feature-libs

Each crate sits behind a narrow C ABI and is linked into the C++ build via Corrosion.

| Crate | Owns |
|---|---|
| `wesl` | `.wesl` → WGSL compilation + naga reflection |
| `assets` | JPEG decode, glTF parse, PNG write |
| `nav` | `GamePathfinder` pathfinding |
| `ui` | game-UI layout (`panes`) + text (`fontdue`) |
| `brainhost` | wasmtime host for brain wasm, behind a C ABI |

- **FFI is data-only and mockable.** The seams are contracts — narrow C ABIs, tested across the boundary. Keep them low-level; no game concepts leak into the Rust libs.

## Tests — use `--lib`
Bare `cargo test` here prints only the empty doctest target.
```sh
cargo test --manifest-path src/crates/wesl/Cargo.toml      --lib
cargo test --manifest-path src/crates/assets/Cargo.toml    --lib
cargo test --manifest-path src/crates/nav/Cargo.toml       --lib
cargo test --manifest-path src/crates/ui/Cargo.toml        --lib
cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib
#   single test: append its name, e.g. ... --lib write_png_roundtrip
```

## Corrosion quirks
- **Each `Cargo.toml` needs an empty `[workspace]` table**, so cargo doesn't walk up the tree looking for a parent workspace.
- **Crate profiles set `panic="abort"`, and Corrosion overrides to `-Cpanic=unwind` at link** — that is what makes the extern-"C" `catch_unwind` thunks actually catch.
- **The `wesl` crate's Cargo *target* is named `wesl_ffi`**, to avoid colliding with the `wesl` dependency.
