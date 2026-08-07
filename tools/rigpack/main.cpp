// rigpack: convert an animation import intermediate into a runtime rig asset.
//
//   rigpack --recipe assets/characters/0ad/pack.json --intermediate /tmp/animexport
//   rigpack --all --out /tmp/every-rig --intermediate /tmp/animexport
//
// A single-family recipe writes beside itself; a `families` recipe writes one
// subdirectory per family. See README.md.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "rigpack/intermediate.hpp"
#include "rigpack/pack.hpp"
#include "rigpack/recipe.hpp"

namespace {

using badlands::rigpack::PackReport;

void Usage() {
  std::cerr << "usage: rigpack --recipe <pack.json> --intermediate <dir>\n"
               "       rigpack --all --out <dir> --intermediate <dir>\n"
               "\n"
               "  --recipe        the pack recipe; rigs are written beside it\n"
               "  --intermediate  the exporter's output root, holding manifest.json\n"
               "  --all           pack EVERY family the manifest carries (needs --out)\n"
               "  --out           output root for --all, one subdirectory per family\n";
}

// The per-clip table, printed only for a single-family run. Across 31 rigs it
// would be 940 lines and nobody would read the warnings underneath it.
void PrintClips(const PackReport& report) {
  std::cout << "\n  clip                             frames  seconds  even  "
               "root(payload)  root(removed)\n";
  for (const badlands::rigpack::ClipReport& clip : report.clips) {
    std::printf("  %-32s %6d  %7.3f  %4s  %13.4f  %13.4f\n", clip.logical.c_str(),
                clip.frames, clip.duration_seconds, clip.uniform ? "yes" : "NO",
                clip.root_translation, clip.root_motion_declared);
  }
}

void PrintWarnings(const PackReport& report, const char* indent) {
  if (!report.dropped_collisions.empty()) {
    std::cout << indent << report.dropped_collisions.size()
              << " sockets dropped (a joint, or an earlier socket, already holds "
                 "the name):\n";
    for (const std::string& name : report.dropped_collisions) {
      std::cout << indent << "  " << name << "\n";
    }
  }
  for (const std::string& warning : report.warnings) {
    std::cout << indent << warning << "\n";
  }
  if (report.max_socket_drift > 0.01f) {
    std::printf("%sWARNING: worst surviving socket drifts %.3f; a socket is frozen "
                "to one offset, so it should be ~0\n",
                indent, report.max_socket_drift);
  }
  int with_root_motion = 0;
  for (const badlands::rigpack::ClipReport& clip : report.clips) {
    if (clip.root_translation > 0.01f) ++with_root_motion;
  }
  if (with_root_motion > 0) {
    std::cout << indent << "WARNING: " << with_root_motion
              << " clips carry root motion; the sim owns movement, so these will "
                 "move characters twice.\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string recipe_path;
  std::string intermediate_root;
  std::string out_root;
  bool all = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--recipe") == 0 && i + 1 < argc) {
      recipe_path = argv[++i];
    } else if (std::strcmp(argv[i], "--intermediate") == 0 && i + 1 < argc) {
      intermediate_root = argv[++i];
    } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_root = argv[++i];
    } else if (std::strcmp(argv[i], "--all") == 0) {
      all = true;
    } else {
      Usage();
      return 2;
    }
  }
  if (intermediate_root.empty() || (all ? out_root.empty() : recipe_path.empty())) {
    Usage();
    return 2;
  }

  std::string error;
  badlands::rigpack::Recipe recipe;
  std::vector<std::string> override_families;

  if (all) {
    // No recipe: take the manifest's own family list, whole-family each.
    std::optional<std::vector<badlands::rigpack::FamilyEntry>> manifest =
        badlands::rigpack::LoadManifest(intermediate_root, &error);
    if (!manifest) {
      std::cerr << "rigpack: " << error << "\n";
      return 1;
    }
    for (const badlands::rigpack::FamilyEntry& entry : *manifest) {
      override_families.push_back(entry.family);
    }
    recipe.all_clips = true;
    recipe.out_dir = out_root;
  } else {
    std::optional<badlands::rigpack::Recipe> loaded =
        badlands::rigpack::LoadRecipe(recipe_path, &error);
    if (!loaded) {
      std::cerr << "rigpack: " << error << "\n";
      return 1;
    }
    recipe = *loaded;
  }

  // One family: the detailed view, because there is room for it.
  if (!all && recipe.families.empty()) {
    const PackReport report = badlands::rigpack::Pack(recipe, intermediate_root);
    if (!report.ok) {
      std::cerr << "rigpack: " << report.error << "\n";
      return 1;
    }
    std::cout << report.family << " -> " << report.out_dir.string() << "\n"
              << "  " << report.joints << " joints, " << report.sockets_kept
              << " sockets, " << report.clips.size() << " clips\n";
    if (!report.rest_pose_from.empty()) {
      std::cout << "  rest pose from " << report.rest_pose_from << " frame 0\n";
    }
    PrintClips(report);
    std::cout << "\n";
    PrintWarnings(report, "  ");
    return 0;
  }

  // Many families: one line each, then everything worth acting on underneath.
  const std::vector<PackReport> reports =
      badlands::rigpack::PackFamilies(recipe, intermediate_root, override_families);

  int failures = 0, clips = 0;
  std::printf("%-34s %6s %7s %7s\n", "family", "joints", "sockets", "clips");
  for (const PackReport& report : reports) {
    if (!report.ok) {
      ++failures;
      std::printf("%-34s  FAILED: %s\n", report.family.c_str(), report.error.c_str());
      continue;
    }
    clips += static_cast<int>(report.clips.size());
    std::printf("%-34s %6d %7d %7zu\n", report.family.c_str(), report.joints,
                report.sockets_kept, report.clips.size());
  }
  std::printf("\n%zu families, %d clips, %d failed\n", reports.size(), clips, failures);

  for (const PackReport& report : reports) {
    if (!report.ok) continue;
    const bool quiet = report.dropped_collisions.empty() && report.warnings.empty() &&
                       report.max_socket_drift <= 0.01f;
    if (quiet) continue;
    std::cout << "\n" << report.family << ":\n";
    PrintWarnings(report, "    ");
  }

  return failures > 0 ? 1 : 0;
}
