#include "mapgen/file_patch_source.hpp"

#include "mapgen/patch_io.hpp"

namespace badlands::mapgen {

PatchData FilePatchSource::Fetch(const PatchRequest& /*req*/) const {
  return patch_;
}

std::unique_ptr<FilePatchSource> LoadFilePatchSource(const std::string& dir,
                                                     std::string* error) {
  std::optional<PatchData> loaded = load_patch(dir, error);
  if (!loaded) return nullptr;

  const std::optional<PatchManifest> man = load_patch_manifest(dir, error);
  if (!man) return nullptr;

  auto src = std::unique_ptr<FilePatchSource>(new FilePatchSource());
  src->patch_ = std::move(*loaded);
  src->native_.origin_m = man->origin_m;
  src->native_.world_size_m = man->world_size_m;
  src->native_.resolution = man->resolution;
  src->source_ = man->source;
  // `rivers` arrives already populated by load_patch (rivers.bin, if the
  // directory has one) -- there is nothing left to derive here.
  return src;
}

}  // namespace badlands::mapgen
