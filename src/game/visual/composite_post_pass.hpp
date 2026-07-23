#pragma once

// Runs several ScenePostPasses in order behind the engine's single post_pass
// slot. GameView chains the fog-of-war overlay and the vision-cone debug overlay
// through one of these.

#include <vector>

#include "engine/rendering/scene_post_pass.hpp"

namespace badlands {

class CompositePostPass : public ScenePostPass {
 public:
  void Add(ScenePostPass* pass) { passes_.push_back(pass); }
  void Clear() { passes_.clear(); }

  void Execute(const PostSceneContext& ctx) override {
    for (ScenePostPass* p : passes_) {
      if (p != nullptr) {
        p->Execute(ctx);
      }
    }
  }

 private:
  std::vector<ScenePostPass*> passes_;
};

}  // namespace badlands
