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
// Only immediate danger sits in the Danger band. Everything else is Normal and
// competes on need -- there is no third category, and adding one would put the
// KIND of an activity back in charge of what a character does.
constexpr std::array<ActivityInfo, static_cast<size_t>(kActivityCount)> kCatalog{{
    {ActivityId::Idle, "Idle", ActivityBand::Normal},
    {ActivityId::Roam, "Roam", ActivityBand::Normal},
    {ActivityId::Buy, "Buy", ActivityBand::Normal},
    {ActivityId::GoHome, "GoHome", ActivityBand::Normal},
    {ActivityId::VisitTavern, "VisitTavern", ActivityBand::Normal},
    {ActivityId::Combat, "Combat", ActivityBand::Danger},
    {ActivityId::Graze, "Graze", ActivityBand::Normal},
    {ActivityId::VisitTax, "VisitTax", ActivityBand::Normal},
    {ActivityId::Deposit, "Deposit", ActivityBand::Normal},
    {ActivityId::Hunt, "Hunt", ActivityBand::Normal},
    {ActivityId::Flee, "Flee", ActivityBand::Danger},
    {ActivityId::Think, "Think", ActivityBand::Normal},
    {ActivityId::Explore, "Explore", ActivityBand::Normal},
    {ActivityId::Chat, "Chat", ActivityBand::Normal},
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
// A weight is a MULTIPLIER on an activity's [0,1] score, and the two kinds of
// score meet on equal terms here:
//
//   need-driven (GoHome, VisitTavern, Chat) score their need's URGENCY, which
//     starts at 0 and climbs toward 1 as the reserve empties -- so weight x
//     urgency rises past a flat rival only once the hero is depleted enough.
//   always-available (Roam, Buy, Idle, Hunt, Explore) score a flat 1, so their
//     weight is just their standing -- the baseline a need has to climb over.
//
// That crossover IS the ordering: a hero wanders (Roam) until fatigue urgency x
// GoHome-weight overtakes it, then goes home. No tier says rest beats wandering;
// being tired does. 0 removes an activity entirely.
ActivityWeights base_hero_weights() {
    ActivityWeights w;
    w.set(ActivityId::GoHome, 3.0f);       // urgency-scaled; wins once genuinely tired
    w.set(ActivityId::VisitTavern, 3.0f);  // urgency-scaled; a proper night out
    w.set(ActivityId::Chat, 2.0f);         // urgency-scaled; company, second to the tavern
    w.set(ActivityId::Buy, 1.5f);          // a flat errand: worth more than wandering
    w.set(ActivityId::Explore, 1.0f);      // flat, but gated by appetite + a frontier
    w.set(ActivityId::Roam, 1.0f);         // flat: the wander a hero falls back to
    w.set(ActivityId::Idle, 0.5f);         // flat: the true last resort
    w.set(ActivityId::Hunt, 0.0f);         // not a hunter: the activity does not exist
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
    // A hunter's job: a flat activity, weighted above the errands so an
    // untroubled hunter hunts, but below a full rest/entertainment NEED so a
    // depleted one still goes home. No other class perceives prey at all (a 0
    // weight skips the perception too).
    hero.weights[HERO_HUNTER].set(ActivityId::Hunt, 2.5f);

    // Personality, expressed purely as preference -- the numbers a designer is
    // meant to argue about; nothing structural rests on them. Chat is
    // urgency-scaled like the tavern but weighted below it, so company fills the
    // hours the tavern is shut rather than replacing a night out. A hunter is a
    // loner -- an explicit 0, not a low weight, since a low weight would read as
    // "rarely" and behave as "never".
    hero.weights[HERO_HUNTER].set(ActivityId::Chat, 0.0f);
    hero.weights[HERO_GRAVE_ROBBER].set(ActivityId::Chat, 2.2f);
    hero.weights[HERO_APPRENTICE].set(ActivityId::Chat, 2.5f);

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
