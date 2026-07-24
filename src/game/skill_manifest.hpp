#pragma once

// Optional JSON overrides for the skill template catalog (SkillCatalog):
// assets/skills/skills.json, an object keyed by skill NAME ("Calcify"). Keys
// are identity (unknown names fail loudly); absent fields keep the compiled
// defaults; "_"-prefixed keys are comments. Returns false (out untouched) on
// any open/parse/shape error -- creature-manifest policy.

#include <string>

#include "badlands_sim.hpp"

namespace badlands {

bool LoadSkillCatalog(const std::string& path, SkillCatalog& out);

}  // namespace badlands
