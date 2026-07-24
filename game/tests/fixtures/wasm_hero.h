#pragma once

// Shared fixture for wasm-driven hero tests. Task 4 deleted the C++ hero
// decision layer (town_think and friends) -- the shipping Nim/wasm brain
// (assets/brains/hero.wasm) is now the ONLY hero decision-maker, so every
// test that needs a hero to actually decide something drives a world built
// on it, through this one fixture, rather than each file growing its own
// copy (the convention every earlier wasm-brain test file used).

#include "badlands_sim.hpp"
#include "sim_internal.hpp"  // badlands::make_world

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <fstream>
#include <memory>
#include <vector>

namespace testfix {

// Reads the shipping brain artifact (LFS binary; scripts/brains/nim/hero.nim),
// repo-root-relative like every other asset path in this codebase (add_test
// sets WORKING_DIRECTORY to the repo root, see CMakeLists.txt).
inline std::vector<uint8_t> load_hero_wasm() {
    std::ifstream file("assets/brains/hero.wasm", std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    REQUIRE(file.read(reinterpret_cast<char*>(bytes.data()), size));
    REQUIRE_FALSE(bytes.empty());
    return bytes;
}

// A world driven by the real, shipping hero brain -- the sole hero decision
// layer now that the C++ reference (town_think) is gone. The wasm bytes need
// only outlive this call: make_world compiles the module (bh_load)
// synchronously and keeps no reference to the source buffer afterward.
inline std::unique_ptr<BadlandsGame> make_wasm_world() {
    const std::vector<uint8_t> bytes = load_hero_wasm();
    return badlands::make_world(
        badlands::BrainDesc{.wasm_bytes = bytes.data(), .wasm_len = bytes.size()});
}

}  // namespace testfix
