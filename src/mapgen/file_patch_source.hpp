#pragma once

// A patch read from a directory of rasters (mapgen/patch_io.hpp).
//
// THIS SOURCE IGNORES THE REQUEST'S GEOMETRY, and that is not a shortcut. A
// directory is a finished artifact, not a queryable world: it holds one patch at
// one origin, one extent and one resolution, and there is nothing in it from
// which a different cut could be produced. Fetch therefore returns what the file
// holds. `native_request()` reports that geometry so a caller can discover it
// rather than guess. The source that genuinely answers arbitrary requests is the
// coarse-world one.
//
// RIVERS ARE READ, NOT DERIVED. This source used to route the loaded bed,
// extract the network and cull it inline -- a temporary bridge for when the
// on-disk form did not carry the graph. It now does (patch_io.hpp's
// rivers.bin), so Fetch is nothing but a read: load_patch already fills
// PatchData::rivers, and this source hands the result straight through.
// NOTHING ELSE ABOUT THE INTERFACE MOVED when that changed -- exactly the
// property the whole split is for.
//
// Construction is where failure is reported (a missing directory, a manifest
// that contradicts its rasters). By the time a PatchSource exists, it works.

#include <memory>
#include <string>

#include "mapgen/patch_source.hpp"

namespace badlands::mapgen {

class FilePatchSource final : public PatchSource {
 public:
  // Ignores `req` -- see the header note. Cheap: the work happened at load.
  PatchData Fetch(const PatchRequest& req) const override;

  // The geometry this directory actually holds.
  const PatchRequest& native_request() const { return native_; }

  // Provenance line from the manifest, or empty.
  const std::string& source() const { return source_; }

 private:
  friend std::unique_ptr<FilePatchSource> LoadFilePatchSource(const std::string&,
                                                              std::string*);
  PatchData patch_;
  PatchRequest native_;
  std::string source_;
};

// Reads `dir` and derives its river network. Returns nullptr with a reason in
// `error` if the directory is missing, incomplete, or contradicts its manifest.
std::unique_ptr<FilePatchSource> LoadFilePatchSource(const std::string& dir,
                                                     std::string* error = nullptr);

}  // namespace badlands::mapgen
