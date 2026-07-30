#pragma once

// Brain wasm artifacts loaded as data (app layer), the counterpart to the
// *_manifest loaders next to it: those read JSON over compiled defaults, this
// reads the raw module bytes a badlands::BrainDesc points at.
//
// App-layer on purpose -- the sim takes bytes (BrainDesc, badlands_sim.hpp) and
// never touches the filesystem, so both apps do the reading and share it here
// rather than each keeping a copy of the failure policy below.

#include <cstdint>
#include <vector>

namespace badlands {

// The shipping hero brain (LFS-committed; built from scripts/brains/nim/hero.nim
// by scripts/build_brains.sh). cwd-relative, like shaders/ and assets/.
inline constexpr const char* kHeroBrainPath = "assets/brains/hero.wasm";

// Reads a brain wasm artifact. An unreadable file returns empty and warns:
// that is a PACKAGING problem (the artifact was not built or not shipped), not
// a brain bug, and an empty BrainDesc is a legitimate world -- heroes simply
// idle, since the wasm brain is the only hero decision layer left (see
// game/src/wasm_brain.h).
//
// A file that DOES load but then fails to compile or instantiate is the
// opposite case and is NOT handled here: WasmBrainRuntime::create treats it as
// fatal, because bytes were actually handed to bh_load.
//
// The returned bytes need only outlive the Sim construction that consumes them
// -- bh_load compiles the module eagerly and keeps no reference to the input.
std::vector<uint8_t> LoadBrainWasm(const char* path);

}  // namespace badlands
