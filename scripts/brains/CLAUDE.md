# scripts/brains/ — Nim→WASM brains

`nim/` holds the hero brain sources, compiled to LFS-committed `.wasm` under
`assets/brains/`. The hero brain is the **only** hero decision layer; there is no
C++ or mock fallback.

- **The wire contract is `game/src/brain_abi.h`**, mirrored by `nim/abi.nim`. Both sides change together or the wasm silently misreads its arguments.
- **`scripts/build_brains.sh` rebuilds the artifacts** (needs Nim and a pinned wasi-sdk, auto-fetched). The `.wasm` files are committed, so a normal build never needs the toolchain.
- **`.wasm` is git LFS.** Stage the artifact path deliberately after a rebuild.
- **The brain intends; the game validates and executes.** A brain adopts intentions and enqueues actions — it never reaches into game state, and a refused intention comes back as an event to reconsider from.
- A world with no wasm bytes loaded idles its heroes, which is a legitimate test configuration rather than a failure.
