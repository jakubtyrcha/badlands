// On-disk asset discovery shared by the shader and script loaders.

use std::path::{Path, PathBuf};

// Locates the asset directory `subdir` (e.g. "shaders") by probing for
// `marker` inside it: every ancestor of the executable first (installed
// layouts), then the crate root (cargo runs), then the working directory.
pub fn find_asset_dir(subdir: &str, marker: &str) -> Option<PathBuf> {
    let marker = Path::new(marker);
    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        let mut dir = exe.parent().map(Path::to_path_buf);
        while let Some(d) = dir {
            candidates.push(d.join(subdir));
            dir = d.parent().map(Path::to_path_buf);
        }
    }
    candidates.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(subdir));
    candidates.push(PathBuf::from(subdir));
    candidates
        .into_iter()
        .find(|candidate| candidate.join(marker).is_file())
}
