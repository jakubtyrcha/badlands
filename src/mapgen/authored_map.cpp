#include "mapgen/authored_map.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

#include "badlands_assets.h"
#include "mapgen/biomes.hpp"
#include "mapgen/mapgen_constants.hpp"

namespace badlands::mapgen {

namespace {

std::string png_path(const std::string& dir, const char* stem, int w) {
  return dir + "/" + stem + "_" + std::to_string(w) + ".png";
}

// RAII for the C-ABI buffers: every failure path below returns early, and the Rust side
// hands us an owned allocation each time.
struct Image16 {
  BadlandsImage16 img;
  explicit Image16(const std::string& p) : img(badlands_decode_image16(p.c_str())) {}
  ~Image16() { badlands_image16_free(img); }
  Image16(const Image16&) = delete;
  Image16& operator=(const Image16&) = delete;
};

struct Image8 {
  BadlandsImage img;
  explicit Image8(const std::string& p) : img(badlands_decode_image(p.c_str())) {}
  ~Image8() { badlands_image_free(img); }
  Image8(const Image8&) = delete;
  Image8& operator=(const Image8&) = delete;
};

bool dims_match(uint32_t w, uint32_t h, const AuthoredMapMeta& m, const std::string& p,
                std::string& err) {
  if (static_cast<int>(w) != m.width || static_cast<int>(h) != m.height) {
    err = "authored map: '" + p + "' is " + std::to_string(w) + "x" + std::to_string(h) +
          " but map_meta.json declares " + std::to_string(m.width) + "x" +
          std::to_string(m.height);
    return false;
  }
  return true;
}

}  // namespace

bool read_authored_meta(const std::string& dir, AuthoredMapMeta& out, std::string& err) {
  const std::string path = dir + "/map_meta.json";
  std::ifstream file(path);
  if (!file) {
    err = "authored map: missing '" + path + "'";
    return false;
  }
  nlohmann::json j;
  try {
    file >> j;
  } catch (const nlohmann::json::exception& e) {
    err = "authored map: unparseable '" + path + "': " + e.what();
    return false;
  }

  AuthoredMapMeta m;
  try {
    m.width = j.at("width").get<int>();
    m.height = j.at("height").get<int>();
    m.meters_per_sample = j.at("meters_per_sample").get<float>();
    m.height_min_m = j.at("height_min_m").get<float>();
    m.height_max_m = j.at("height_max_m").get<float>();
    m.water_level_m = j.value("water_level_m", 0.0f);
  } catch (const nlohmann::json::exception& e) {
    err = "authored map: bad '" + path + "': " + e.what();
    return false;
  }

  if (m.width <= 0 || m.height <= 0) {
    err = "authored map: non-positive dimensions " + std::to_string(m.width) + "x" +
          std::to_string(m.height);
    return false;
  }
  // Integer division downstream (sections.cpp, terrain_mesh.cpp) drops any remainder
  // samples, so a bad size would silently shrink the map rather than fail.
  if (m.width % kSamplesPerBlock != 0 || m.height % kSamplesPerBlock != 0) {
    err = "authored map: " + std::to_string(m.width) + "x" + std::to_string(m.height) +
          " is not a multiple of kSamplesPerBlock (" + std::to_string(kSamplesPerBlock) +
          "); the remainder would fall off the block grid";
    return false;
  }
  if (!(m.height_max_m > m.height_min_m)) {
    err = "authored map: empty/inverted height range [" +
          std::to_string(m.height_min_m) + ", " + std::to_string(m.height_max_m) + "]";
    return false;
  }
  // kMetersPerSample is compile-time, so an asset authored at another density would be
  // silently stretched to the map grid rather than placed at its intended scale.
  if (m.meters_per_sample != static_cast<float>(kMetersPerSample)) {
    err = "authored map: meters_per_sample " + std::to_string(m.meters_per_sample) +
          " disagrees with the engine's kMetersPerSample " +
          std::to_string(kMetersPerSample);
    return false;
  }

  // The biome image stores raw Biome enum values, so the asset's biome ordering must
  // match the engine enum exactly -- otherwise every label silently maps to the wrong
  // biome (e.g. forest textured as lake) with no error. Fail loudly like the checks
  // above. (biome_name / kBiomeCount from biomes.hpp; same order-is-the-contract rule
  // ResolveBiomePacks enforces on the material manifest.)
  const auto biomes = j.find("biomes");
  if (biomes == j.end() || !biomes->is_array() ||
      biomes->size() != static_cast<size_t>(kBiomeCount)) {
    err = "authored map: '" + path + "' must list all " + std::to_string(kBiomeCount) +
          " biomes in enum order";
    return false;
  }
  for (int i = 0; i < kBiomeCount; ++i) {
    const std::string want(biome_name(static_cast<Biome>(i)));
    if (!(*biomes)[i].is_string() || (*biomes)[i].get<std::string>() != want) {
      err = "authored map: biome[" + std::to_string(i) + "] must be '" + want +
            "' to match the engine enum";
      return false;
    }
  }

  m.heights_png = png_path(dir, "heights", m.width);
  m.biome_png = png_path(dir, "biome", m.width);
  out = std::move(m);
  return true;
}

bool load_authored_heights(const AuthoredMapMeta& meta, Field2D<float>& out,
                           std::string& err) {
  const Image16 img(meta.heights_png);
  if (!img.img.luma) {
    err = "authored map: cannot decode '" + meta.heights_png + "' as 16-bit";
    return false;
  }
  if (!dims_match(img.img.width, img.img.height, meta, meta.heights_png, err)) return false;

  Field2D<float> h(meta.width, meta.height);
  const float span = meta.height_max_m - meta.height_min_m;
  const float inv = span / 65535.0f;
  for (int y = 0; y < meta.height; ++y) {
    for (int x = 0; x < meta.width; ++x) {
      const uint16_t v = img.img.luma[static_cast<size_t>(y) * meta.width + x];
      h.at(x, y) = meta.height_min_m + static_cast<float>(v) * inv;
    }
  }
  out = std::move(h);
  return true;
}

bool load_authored_biome(const AuthoredMapMeta& meta, Field2D<uint8_t>& out,
                         std::string& err) {
  const Image8 img(meta.biome_png);
  if (!img.img.rgba) {
    err = "authored map: cannot decode '" + meta.biome_png + "'";
    return false;
  }
  if (!dims_match(img.img.width, img.img.height, meta, meta.biome_png, err)) return false;

  Field2D<uint8_t> b(meta.width, meta.height);
  for (int y = 0; y < meta.height; ++y) {
    for (int x = 0; x < meta.width; ++x) {
      // Greyscale decoded to RGBA8: the label is in R (G/B carry the same value).
      const uint8_t v = img.img.rgba[(static_cast<size_t>(y) * meta.width + x) * 4];
      if (v >= kBiomeCount) {
        err = "authored map: '" + meta.biome_png + "' holds biome " + std::to_string(v) +
              " at (" + std::to_string(x) + "," + std::to_string(y) + "), but only 0.." +
              std::to_string(kBiomeCount - 1) + " exist";
        return false;
      }
      b.at(x, y) = v;
    }
  }
  out = std::move(b);
  return true;
}

namespace {

template <typename T>
Field2D<T> crop_field(const Field2D<T>& src, int x, int y, int w, int h) {
  Field2D<T> out(w, h);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) out.at(i, j) = src.at(x + i, y + j);
  return out;
}

}  // namespace

bool crop_authored_map(Field2D<float>& heights, Field2D<uint8_t>& biome, int x, int y,
                       int w, int h, std::string& err) {
  const int mw = heights.width, mh = heights.height;
  if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > mw || y + h > mh) {
    err = "authored map: crop region [" + std::to_string(x) + "," + std::to_string(y) +
          " " + std::to_string(w) + "x" + std::to_string(h) +
          "] does not fit inside the " + std::to_string(mw) + "x" + std::to_string(mh) +
          " map";
    return false;
  }
  if (w % kSamplesPerBlock != 0 || h % kSamplesPerBlock != 0) {
    err = "authored map: crop size " + std::to_string(w) + "x" + std::to_string(h) +
          " is not a multiple of kSamplesPerBlock (" + std::to_string(kSamplesPerBlock) +
          ")";
    return false;
  }
  heights = crop_field(heights, x, y, w, h);
  biome = crop_field(biome, x, y, w, h);
  return true;
}

}  // namespace badlands::mapgen
