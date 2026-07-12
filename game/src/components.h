// ECS components of the badlands simulation. All POD; positions live on the
// ground (XZ) plane to match the renderer's convention.

#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace badlands {

struct Position {
    glm::vec2 pos;  // XZ
};

struct Stats {
    float move_speed;
    float attack_range;
    float attack_damage;
    float attack_cooldown;
};

struct Health {
    float hp;
    float max_hp;
};

struct Team {
    int32_t id;
};

struct CooldownTimer {
    float remaining;
};

struct RenderShape {
    glm::vec3 size;
    glm::vec3 color;
};

// Transient per-tick output of a brain (script host calls or the mock brain).
// kind: 0 = idle, 1 = move along dir, 2 = attack nearest enemy.
struct Intent {
    int kind;
    glm::vec2 dir;
};

}  // namespace badlands
