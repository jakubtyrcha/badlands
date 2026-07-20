#pragma once

namespace badlands {

// Which visual proxy a game object renders with. Driven by the USE_BLOCKOUT_MODE
// env toggle (see main_game.cpp); the SceneComposer attaches Blockout (debug
// solid colors / flat water) vs Detailed (PBR) material components accordingly.
enum class RenderMode { Detailed, Blockout };

}  // namespace badlands
