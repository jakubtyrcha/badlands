#include "game/brain_asset.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <ios>

namespace badlands {

std::vector<uint8_t> LoadBrainWasm(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.good()) {
        spdlog::warn("LoadBrainWasm: '{}' unreadable -- heroes will idle (no brain loaded)", path);
        return {};
    }
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        spdlog::warn("LoadBrainWasm: '{}' failed to read -- heroes will idle (no brain loaded)",
                     path);
        return {};
    }
    return bytes;
}

}  // namespace badlands
