#include "mapgen/coarse_io.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace badlands::mapgen {

namespace {

// std::numeric_limits<float>::max_digits10 == 9: enough decimal digits that
// parsing the printed text back reproduces the exact same float, which is
// what an EXACT round-trip promise requires (patch_io.cpp's map.txt does not
// need this because its tests only ever write round numbers).
std::string fmt_float(float v) {
  std::ostringstream os;
  os << std::setprecision(9) << v;
  return os.str();
}

}  // namespace

std::optional<CoarseManifest> load_coarse_manifest(const std::string& dir,
                                                    std::string* error) {
  const std::string path = dir + "/world.txt";
  std::ifstream f(path);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return std::nullopt;
  }
  CoarseManifest m;
  bool have_res = false, have_size = false;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ls(line);
    std::string key;
    if (!(ls >> key) || key.empty() || key[0] == '#') continue;
    if (key == "resolution") {
      have_res = static_cast<bool>(ls >> m.resolution);
    } else if (key == "world_size_m") {
      have_size = static_cast<bool>(ls >> m.world_size_m);
    } else if (key == "seed") {
      ls >> m.seed;
    } else if (key == "runoff_m_per_yr") {
      ls >> m.runoff_m_per_yr;
    } else if (key == "steps") {
      ls >> m.steps;
    } else if (key == "soil_cut_mountain_m") {
      ls >> m.soil_cut_mountain_m;
    } else if (key == "soil_cut_hills_m") {
      ls >> m.soil_cut_hills_m;
    }
    // Unknown keys -- including "texel_m", which is written for OTHER readers
    // only (see the header) -- are ignored on purpose: the writer may add
    // fields without breaking older readers.
  }
  if (!have_res || m.resolution <= 0) {
    if (error) *error = path + ": missing or invalid 'resolution'";
    return std::nullopt;
  }
  if (!have_size || !(m.world_size_m > 0.0f)) {
    if (error) *error = path + ": missing or invalid 'world_size_m'";
    return std::nullopt;
  }
  // Derived, not read: see the header comment on CoarseManifest::texel_m.
  m.texel_m = m.world_size_m / static_cast<float>(m.resolution);
  return m;
}

bool write_coarse_manifest(const std::string& dir, const CoarseManifest& m,
                           std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    if (error) *error = "cannot create " + dir + ": " + ec.message();
    return false;
  }
  const std::string path = dir + "/world.txt";
  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot open " + path + " for writing";
    return false;
  }
  const float texel_m =
      m.resolution > 0 ? m.world_size_m / static_cast<float>(m.resolution)
                       : 0.0f;
  f << "resolution " << m.resolution << "\n";
  f << "world_size_m " << fmt_float(m.world_size_m) << "\n";
  f << "texel_m " << fmt_float(texel_m) << "\n";
  f << "seed " << m.seed << "\n";
  f << "runoff_m_per_yr " << fmt_float(m.runoff_m_per_yr) << "\n";
  f << "steps " << m.steps << "\n";
  f << "soil_cut_mountain_m " << fmt_float(m.soil_cut_mountain_m) << "\n";
  f << "soil_cut_hills_m " << fmt_float(m.soil_cut_hills_m) << "\n";
  if (!f) {
    if (error) *error = "short write on " + path;
    return false;
  }
  return true;
}

}  // namespace badlands::mapgen
