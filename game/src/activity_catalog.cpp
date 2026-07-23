// The AI's vocabulary tables: the activity catalog (names + bands), the
// compiled default weight tables, and the hero class names.
//
// All of it is pure DATA about identity, deliberately kept out of both the
// behaviour library and the sim: blocks describe HOW an activity is scored and
// acted on, this file describes WHAT activities exist, where they sit in the
// shared band hierarchy, and how much each class cares. UIs and the statistics
// histogram read the catalog instead of hardcoding a switch, so adding an
// activity is a row here rather than an edit in every consumer.
//
// This TU depends on nothing but badlands_sim.hpp, which is what lets lean
// consumers (the factors manifest loader and its tests) link the naming tables
// without pulling in the registry, the map, or the noiser VM.

#include "badlands_sim.hpp"

#include <array>

namespace badlands {

namespace {

// Indexed by ActivityId; the static_assert below pins the two in step.
constexpr std::array<ActivityInfo, static_cast<size_t>(kActivityCount)> kCatalog{{
    {ActivityId::Idle, "Idle", ActivityBand::Fallback},
    {ActivityId::Roam, "Roam", ActivityBand::Filler},
    {ActivityId::Buy, "Buy", ActivityBand::Filler},
    {ActivityId::GoHome, "GoHome", ActivityBand::Filler},
    {ActivityId::VisitTavern, "VisitTavern", ActivityBand::Filler},
    {ActivityId::Combat, "Combat", ActivityBand::Danger},
    {ActivityId::Graze, "Graze", ActivityBand::Filler},
    {ActivityId::VisitTax, "VisitTax", ActivityBand::Productive},
    {ActivityId::Deposit, "Deposit", ActivityBand::Productive},
    {ActivityId::Hunt, "Hunt", ActivityBand::Filler},
    {ActivityId::Flee, "Flee", ActivityBand::Danger},
    {ActivityId::Think, "Think", ActivityBand::Filler},
    {ActivityId::Explore, "Explore", ActivityBand::Productive},
    {ActivityId::Chat, "Chat", ActivityBand::Filler},
    {ActivityId::RestUrgent, "RestUrgent", ActivityBand::Danger},
}};

// The catalog must stay dense and in id order -- every consumer indexes it.
constexpr bool catalog_is_dense() {
    for (size_t i = 0; i < kCatalog.size(); ++i) {
        if (static_cast<int32_t>(kCatalog[i].id) != static_cast<int32_t>(i)) {
            return false;
        }
    }
    return true;
}
static_assert(catalog_is_dense(), "ActivityCatalog must be indexed by ActivityId");

// --- default preference tables ---------------------------------------------
// Phase 1 note: these weights reproduce the pre-band priority chain exactly
// (GoHome > Hunt > Buy > VisitTavern > Roam > Idle), because every activity
// currently scores a binary 0/1 applicability -- so `weight x score` collapses
// to the old tiers. Class personality (and the softer considerations that make
// these numbers matter) arrives as those scores gain real curves.
ActivityWeights base_hero_weights() {
    ActivityWeights w;
    w.set(ActivityId::RestUrgent, 1.0f);  // alone in Danger; survival, not taste
    w.set(ActivityId::GoHome, 5.0f);
    w.set(ActivityId::Buy, 4.0f);
    w.set(ActivityId::VisitTavern, 3.0f);
    // Below the tavern, above wandering: heroes prefer a proper night out, but
    // company beats pacing about -- so chatting shows up mostly at night, when
    // the tavern block scores 0 anyway.
    w.set(ActivityId::Chat, 2.5f);
    w.set(ActivityId::Roam, 2.0f);
    w.set(ActivityId::Idle, 1.0f);
    w.set(ActivityId::Hunt, 0.0f);  // not a hunter: the activity does not exist
    // Alone in the Productive band, so this weight only says "the class has
    // it". HOW OFTEN a class explores is HeroFactors::explore_chance, because a
    // weight ranks rivals within a band and there are none to rank against.
    w.set(ActivityId::Explore, 1.0f);
    return w;
}

}  // namespace

const char* HeroClassName(HeroClassId cls) {
    switch (cls) {
        case HERO_MERCENARY:    return "Mercenary";
        case HERO_HUNTER:       return "Hunter";
        case HERO_GRAVE_ROBBER: return "Grave Robber";
        case HERO_APPRENTICE:   return "Apprentice";
        default:                return "";
    }
}

std::span<const ActivityInfo> ActivityCatalog() { return kCatalog; }

const ActivityInfo& ActivityInfoOf(int32_t id) {
    if (id < 0 || id >= kActivityCount) {
        return kCatalog[static_cast<size_t>(ActivityId::Idle)];
    }
    return kCatalog[static_cast<size_t>(id)];
}

const char* ActivityName(int32_t id) {
    if (id < 0 || id >= kActivityCount) {
        return "-";  // "no decision yet" (snapshot rows start at -1)
    }
    return kCatalog[static_cast<size_t>(id)].name;
}

SimFactors::SimFactors() {
    for (int32_t c = 0; c < HERO_CLASS_COUNT; ++c) {
        hero.weights[c] = base_hero_weights();
    }
    // A hunter's job. Weighted between rest and the errands, so a tired hunter
    // still goes home but an idle one hunts before shopping -- and no other
    // class perceives prey at all (weight 0 skips the perception too).
    hero.weights[HERO_HUNTER].set(ActivityId::Hunt, 4.5f);

    // Personality, expressed purely as preference. These are the numbers a
    // designer is expected to argue about; nothing structural depends on them.
    // A hunter is happier in its own company; an apprentice is the most social
    // and the most easily bored into seeking someone out.
    hero.weights[HERO_HUNTER].set(ActivityId::Chat, 1.0f);
    hero.weights[HERO_APPRENTICE].set(ActivityId::Chat, 3.5f);
    hero.weights[HERO_GRAVE_ROBBER].set(ActivityId::Chat, 1.5f);

    // Appetite for the unknown, heavily skewed: a hunter ranges out as a matter
    // of course, a grave robber now and then, and the town-dwellers only rarely.
    hero.explore_chance[HERO_MERCENARY] = 0.05f;
    hero.explore_chance[HERO_HUNTER] = 0.85f;
    hero.explore_chance[HERO_GRAVE_ROBBER] = 0.15f;
    hero.explore_chance[HERO_APPRENTICE] = 0.03f;

    // Deer: bolt (Danger band) over graze over wander.
    critter.weights.set(ActivityId::Flee, 1.0f);
    critter.weights.set(ActivityId::Graze, 3.0f);
    critter.weights.set(ActivityId::Roam, 2.0f);
    critter.weights.set(ActivityId::Idle, 1.0f);
}

}  // namespace badlands
