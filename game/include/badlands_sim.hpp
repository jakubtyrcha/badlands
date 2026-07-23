// C++ API of the badlands game simulation (C++/EnTT + noiser brains).
//
// Data in, data out: scenarios are composed by spawning CharacterDesc rows;
// observers (renderer, tests) inspect CharacterState snapshots and SimStats.
// `badlands::Sim` owns the sim world and exposes tick/spawn/dispatch/snapshot
// as C++ methods. This C++ Sim API replaced the former extern-"C", data-only
// game_* ABI, which has been removed.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <entt/entt.hpp>  // entt::registry (the amalgamated single-include ships no fwd.hpp)

// The internal sim world, defined at global scope in game/src/game_state.h.
struct BadlandsGame;

namespace badlands {

// ---- enums (were GameBuildingKind / GameActionKind) ------------------------
enum class BuildingKind : int32_t {
    Castle = 0,
    FreeCompanyQuarters,
    HuntersCamp,
    ThievesDen,
    Scriptorium,
    Tavern,
    Apothecary,
    Watchtower,
    House,  // poppable
    Sewer,  // poppable
    Count
};

enum class ActionKind : int32_t {
    PlaceBuilding = 0,  // world_x/z, param_a = kind, param_b = rotation_index
    RecruitHero,        // target_id = guild building id
    DestroyBuilding,    // target_id = building id
    Count
};

// The placement/movement grid spans the WHOLE map: tile == 1 world unit == 1
// map metre, and the symbolic map is 256 m (64 blocks x 4 m), so the grid is
// [-128, 128). (It was 48 -- a 96 u window centred on the lake, which left the
// map's Forest/Plains ring unreachable; sim.cpp static_asserts this stays equal
// to the map span.)
inline constexpr int32_t kGridHalfExtentTiles = 128;  // was GAME_GRID_HALF_EXTENT_TILES

// Hero guild classes (the recruitable "class type id"). Unscoped + HERO_*
// enumerators to match the sim-internal usage this was promoted from (was
// heroes.h's HeroClassId); the numeric values are load-bearing (color table,
// registry HeroClass component .value). NB: distinct from the `HeroClass`
// EnTT *component* (game/src/components.h) -- this is the class enum.
enum HeroClassId : int32_t {
    HERO_MERCENARY = 0,
    HERO_HUNTER,
    HERO_GRAVE_ROBBER,
    HERO_APPRENTICE,
    HERO_CLASS_COUNT
};

// A building recruits at most this many distinct hero classes (usually 1). The
// per-kind recruit set lives in BuildingDef::recruits (the placement.cpp kDefs
// table is its single source of truth).
inline constexpr int32_t kMaxRecruitClasses = 3;

// Display name of a hero class ("Mercenary", ...). Empty string if out of range.
const char* HeroClassName(HeroClassId cls);

// Only characters on this team grant the player fog-of-war vision. Enemies run
// their own (future) vision and never reveal the map for the player.
inline constexpr int32_t kPlayerTeam = 0;

// Where make_world prebuilds the colony's Castle: on the plains south of the
// central lake (the map origin is water). The single source of truth for the
// colony seat -- the game's town + camera + fog-of-war centre on it.
inline constexpr float kCastleSpawnX = 0.0f;
inline constexpr float kCastleSpawnZ = 54.0f;

// The three fog-of-war knowledge levels (see the design doc). Cumulative:
// once Visible/Dormant, a texel never returns to Unknown.
enum class VisionLevel : int32_t {
    Unknown = 0,   // terra-incognita: never discovered (rendered black)
    Dormant = 1,   // discovered but not currently seen (rendered desaturated)
    Visible = 2,   // inside a player vision source right now (rendered normally)
};

// ---- POD result structs (field-for-field from badlands_game.h) -------------

// What KIND of thing is being spawned. Archetype is a spawn recipe: it decides
// which components and which brain the entity gets, and is not consulted again
// at think time (perception is relational -- friend/neutral/enemy + threat --
// never taxonomic).
enum class Archetype : int32_t {
    Hero = 0,   // needs, home, inventory, errands; the utility brain
    Townfolk,   // simple sequential goals (route, commute); no needs
    Critter,    // reactive: roam/graze, flee non-critters
    Monster,    // combat; no needs
};

// ---- activities: the AI's goal vocabulary ----------------------------------
//
// Every decision a brain can take is an ActivityId. One id space, shared by the
// sim, the command log (SetBehavior.param_a), the snapshot
// (CharacterState.behavior), the statistics histogram, and any future noiser
// brain -- so it is APPEND-ONLY: never renumber, never reuse.
//
// (game/src/town_brain.h aliases this as `badlands::Behavior`, the name the
// sim internals have always used.)
enum class ActivityId : int32_t {
    Idle = 0,
    Roam,
    Buy,
    GoHome,
    VisitTavern,
    Combat,
    Graze,
    VisitTax,
    Deposit,
    Hunt,
    Flee,        // bolting from a threat (was reported as Roam)
    Think,       // deliberating: an idle pause between discretionary goals
    Explore,     // walking toward terra incognita
    Chat,        // socializing with another hero (partial entertainment)
    RestUrgent,  // rest as survival, not as leisure
    Count
};
inline constexpr int32_t kActivityCount = static_cast<int32_t>(ActivityId::Count);

// Bands are the SHARED priority hierarchy every archetype obeys, and the one
// thing the weight table can never override: an applicable Danger activity
// always beats every Productive one, which always beats every Filler one. That
// is what makes safety structural rather than a tuning problem -- no filler
// weight, however large, can outbid a threat response.
//
// Within a band, `weight x considerations` decides (see ActivityWeights). An
// activity drops out of contention by scoring 0 (a veto), NOT by being
// out-prioritized -- which is how e.g. a productive errand yields to rest: it
// disqualifies itself when the hero is too tired, and the loop falls through to
// the Filler band. There is no per-band threshold knob: the threshold is 0.
enum class ActivityBand : int32_t {
    Danger = 0,  // threat response + survival: flee, fight, collapse
    Productive,  // errands, exploration, training
    Filler,      // rest, entertainment, socializing, wandering
    Fallback,    // the last resort (Idle); nothing competes for it
    Count
};

// One catalog row: an activity's stable identity, for inspection.
struct ActivityInfo {
    ActivityId id;
    const char* name;  // stable inspection-facing label ("GoHome")
    ActivityBand band;
};

// Every activity, indexed by id (ActivityCatalog()[i].id == ActivityId(i)).
// The single source of truth for names + bands: UIs and statistics read this
// instead of hardcoding a switch that silently rots when an activity is added.
std::span<const ActivityInfo> ActivityCatalog();
// Catalog lookup; out-of-range ids resolve to the Idle row.
const ActivityInfo& ActivityInfoOf(int32_t id);
// Convenience name lookup; "-" for an out-of-range id (e.g. a -1 "no decision
// yet" from a snapshot row).
const char* ActivityName(int32_t id);

// Per-(class, activity) preference, indexed by ActivityId. Utility is
// `weight * considerations`, compared only WITHIN a band -- so a weight
// expresses "hunters explore constantly, apprentices almost never" without ever
// letting exploration outrank danger. A weight of 0 removes the activity from
// that class entirely, which is how classes get unique activity sets without
// separate code paths (perception may skip its cost too).
struct ActivityWeights {
    float w[kActivityCount] = {};

    float of(ActivityId id) const {
        const int32_t i = static_cast<int32_t>(id);
        return (i >= 0 && i < kActivityCount) ? w[i] : 0.0f;
    }
    void set(ActivityId id, float value) {
        const int32_t i = static_cast<int32_t>(id);
        if (i >= 0 && i < kActivityCount) {
            w[i] = value;
        }
    }
};

// ---- tuning factors (data, not code) ---------------------------------------
// Per-archetype behaviour tuning. The sim ships the defaults below, so it is
// fully usable -- and unit-testable -- with no file present; an app may load
// assets/creatures/factors.json over them (src/game/factors_manifest.hpp) so
// designers can tune without a rebuild.
//
// Factors are INITIAL CONFIG in the determinism contract
// (state = f(seed, initial config, command log, N ticks)): a replay must use
// the same factors, and the command log does not carry them.
struct HeroFactors {
    float fatigue_go_home = 0.6f;  // tired enough to head home by day
    float fatigue_night = 0.2f;    // lower bar once it is night
    float boredom_tavern = 0.5f;   // bored enough to seek the tavern
    float roam_radius = 6.0f;      // world units around the roam anchor
    float hunt_sight_radius = 22.0f;  // how far a Hunter spots prey (deer)
    // How far a hero notices hostiles. Feeds WorldView's threat list, which
    // gates deliberation (you do not stand and think with a rat closing in).
    float threat_radius = 14.0f;
    // Deliberation pause between discretionary goal changes, drawn uniformly
    // from this range. The prototype day is 120 s, so an in-game minute is
    // ~83 ms of sim time and the default range is roughly 0-10 in-game minutes.
    // Setting think_max_millis to 0 disables deliberation entirely.
    int64_t think_min_millis = 0;
    int64_t think_max_millis = 833;
    // Per-class preference table (see ActivityWeights). Filled with the
    // compiled defaults by SimFactors' constructor; factors.json may override
    // any single entry. This is the primary dial for class personality.
    ActivityWeights weights[HERO_CLASS_COUNT];
};

// Critter (deer) tuning. Deer graze/roam in Forest/Plains and bolt from any
// non-critter that comes within sight.
struct CritterFactors {
    float sight_radius = 12.0f;    // notices non-critters within this range
    float flee_radius = 8.0f;      // bolts when one is this close
    float flee_distance = 12.0f;   // how far it runs from the threat
    float roam_radius = 14.0f;     // wander range around the home anchor
    float graze_fraction = 0.4f;   // fraction of each roam cycle spent grazing
    // Deer run the SAME banded selector and the same shared blocks as heroes --
    // only this table and their activity list differ. That is the shareability
    // of the core, made executable rather than asserted.
    ActivityWeights weights;
};

// Townfolk (tax collector) tuning + the town economy.
struct TownfolkFactors {
    int64_t spawn_interval_millis = 60000;  // a collector leaves the castle this often
    int32_t max_alive = 2;                  // cap on live collectors
    float move_speed = 2.2f;                // a plodding taxman
    uint32_t house_income_per_day = 50;     // each House accrues this each midnight
};

// Monster (rat) tuning. Rats spawn from the Sewer and attack the nearest hostile
// unit, falling back to gnawing the nearest targettable building.
struct MonsterFactors {
    int64_t spawn_interval_millis = 20000;  // a rat crawls out this often
    int32_t max_alive = 4;                  // cap on live rats
};

struct SimFactors {
    // Fills the activity weight tables with the compiled per-class defaults
    // (the scalar members above carry their own in-class defaults). Declared
    // rather than defaulted because the weight defaults are a table, not a
    // constant -- see game/src/activity_catalog.cpp.
    SimFactors();

    HeroFactors hero;
    CritterFactors critter;
    TownfolkFactors townfolk;
    MonsterFactors monster;
};

// Spawn input. pos is on the ground (XZ) plane, matching the renderer.
struct CharacterDesc {
    Archetype archetype = Archetype::Hero;
    float pos_x, pos_z;
    int32_t team;
    float hp;
    float move_speed;       // units/sec
    float attack_range;
    float attack_damage;
    float attack_cooldown;  // seconds between swings
    float size_x, size_y, size_z;
    float color_r, color_g, color_b;
    // Fog-of-war vision (only kPlayerTeam entities grant the player vision).
    // radius 0 => grants no vision. cone_half_angle_deg is the half-angle of
    // the forward vision cone (>= 180 => full circle). facing is the initial
    // XZ look direction; {0,0} => the model-forward default (kCharacterForward).
    float vision_radius = 0.0f;
    float vision_cone_half_angle_deg = 180.0f;
    float facing_x = 0.0f, facing_z = 0.0f;
};

// Per-living-entity snapshot row: the renderer reads pos/size/color, tests
// read team/hp.
struct CharacterState {
    uint32_t id;
    int32_t team;
    float pos_x, pos_z;
    float hp, max_hp;
    float size_x, size_y, size_z;
    float color_r, color_g, color_b;
    int32_t home_building_id;    // recruiting guild; -1 = homeless / not a hero
    int32_t inside_building_id;  // -1 = outside; >=0 => hidden (don't draw; list in panel)
    // Hero simulation/display state, for the inspector. Zeroed for non-heroes.
    float fatigue, boredom;  // drives, 0..1
    int32_t behavior;        // last decided badlands::Behavior; -1 = none yet
    char name[24];           // NUL-terminated display name; "" for non-heroes
    // Current goal + pathfinding state: what this entity is walking toward now
    // and how far along the route it is.
    int32_t goal_kind;       // MoveTarget::Kind: 0 None, 1 Point, 2 Entity, 3 Building
    float goal_x, goal_z;    // goal position in world XZ (0,0 when goal_kind == 0)
    int32_t path_waypoints;  // waypoints remaining on the planned route
    int32_t archetype;       // Archetype (Hero/Townfolk/Critter/Monster)
    // Unit XZ look direction (the character Transform's rotation applied to the
    // model-forward axis, projected to XZ). Drives the vision cone and the
    // render pose. Always normalized (defaults to kCharacterForward).
    float facing_x, facing_z;
    // Fog-of-war vision this entity grants (0 radius => none). The renderer draws
    // the cone debug overlay from these + facing.
    float vision_radius;
    float vision_cone_half_angle_deg;  // >= 180 => full circle
};

// Run counters. NB: NOT `Stats` — badlands::Stats already exists (a sim
// component, game/src/components.h:24). Use SimStats for the run counters.
struct SimStats {
    uint64_t ticks;
    uint64_t script_intents;  // intents delivered by noiser brains (0 when mocked)
    uint32_t noiser_bugs;     // failures that downgraded an entity to the mock brain
};

// Static per-kind footprint size (tiles), behavior flags, and recruit set.
struct BuildingDef {
    int32_t width_tiles, depth_tiles;
    bool poppable;            // auto-spawned (House/Sewer), never player-placed
    bool user_destructible;   // player may DESTROY (the 7 buildable kinds)
    bool enemy_targettable;   // future monster attacks may target (Castle/House)
    // Fog-of-war vision radius (world units) measured from the footprint EDGES
    // (a euclidean expansion of the footprint). 0 => grants no vision.
    float vision_radius;
    // Hero classes this kind recruits. recruit_count is 0 for non-guilds and 1
    // for today's guilds; the array has room for 2-3 (guilds are the only
    // kinds with recruit_count > 0). Declared per kind in placement.cpp kDefs.
    int32_t recruit_count;
    HeroClassId recruits[kMaxRecruitClasses];
};

// The world-space box a renderer should draw for a building of this kind and
// rotation. size is the local (pre-rotation) X/Z extent; apply yaw_radians
// about Y.
struct RenderBox {
    float size_x, size_z;
    float yaw_radians;
};

// Placement request: a raw (un-snapped) desired center + rotation. The sim
// snaps the center to the grid lattice for the kind's parity.
struct PlacementDesc {
    int32_t kind;
    int32_t rotation_index;
    float world_x, world_z;
};

// One grid triangle in a probe readout.
struct GridTriangle {
    int32_t tile_x, tile_z;  // tile min-corner, in [-H, H)
    uint32_t corner;         // 0=N(-Z) 1=E(+X) 2=S(+Z) 3=W(-X)
    uint32_t state;          // 0=free 1=blocked 2=would-block (blocked wins)
};

struct PlacementProbe {
    bool valid;  // the snapped footprint could be placed now
    float snapped_x, snapped_z;
};

// Generic player->world action trigger. Every player action crosses this one
// entry point (Sim::Dispatch).
struct Action {
    ActionKind kind;
    uint32_t target_id;  // building/entity id for id-addressed actions
    float world_x, world_z;
    int32_t param_a, param_b;
};

// Per-building snapshot row (Characters() pattern). ids are stable and dense.
struct BuildingState {
    uint32_t id;
    BuildingKind kind;
    float center_x, center_z;
    int32_t rotation_index;
    int32_t width_tiles, depth_tiles;
    uint32_t taxable_income;  // uncollected tax owed (Houses accrue at midnight)
    float hp, max_hp;         // structure health (rats chew it down)
};

// World-level scalars (gold, grid size, sprawl bookkeeping).
struct WorldState {
    uint32_t gold;
    int32_t grid_half_extent_tiles;
    uint32_t queued_poppables;  // owed but not yet placeable (crowded map)
    uint32_t urban_quarters;    // sprawl accumulator in quarter-units
    uint32_t guild_roster_cap;  // kGuildRosterCap (heroes per guild); UI mirrors it
    // The sim clock. world_millis is the authoritative integer time (advanced by
    // a compile-time constant per tick at a fixed 30 Hz); the rest are derived.
    int64_t world_millis;
    float time_of_day;  // 0..1 within the current day
    uint32_t day;       // whole days elapsed
    int32_t is_night;   // 0/1
};

// ---------------------------------------------------------------------------
// Command log (the trace of record)
//
// Every mutation of the sim -- player action and AI decision alike -- is a
// Command applied at a single point, in deterministic order, and appended here.
// (initial config, seed, command log) reproduces a run exactly, so this is both
// the replay input and what a debug panel shows.
// ---------------------------------------------------------------------------
enum class CommandKindId : int32_t {
    PlaceBuilding = 0,
    RecruitHero,
    DestroyBuilding,
    MoveTo,
    EnterBuilding,
    EnterHome,
    Buy,
    Attack,
    SetBehavior,
    CollectTax,
    Deposit,
    AttackBuilding,
    Shoot,
};

struct CommandRecord {
    CommandKindId kind;
    uint32_t actor;  // entity id; UINT32_MAX = player/global
    uint32_t target_id;
    float point_x, point_z;
    int32_t param_a, param_b;
    int64_t at_millis;  // sim time the command took effect
};

// Read-only snapshot of the published (front) fog-of-war field. The grid lives
// in the SIM coordinate frame; texel (i,j) covers the world square whose min
// corner is (world_min_x + i*texel_m, world_min_z + j*texel_m). `rg` is
// nx*nz*2 bytes, interleaved per texel: [2*k+0] = discovered (0 or 255),
// [2*k+1] = visible (0 or 255), k = j*nx + i. Pointer valid until the next
// Tick(); empty (rg==nullptr) until ConfigureVision() has been called.
struct VisionField {
    int32_t nx = 0, nz = 0;
    float world_min_x = 0.0f, world_min_z = 0.0f;
    float texel_m = 1.0f;
    const uint8_t* rg = nullptr;
};

// Injected Rust nav provider (was GamePathfinder) — kept as-is, by value. The
// engine delegates path *geometry* to this provider; obstacles mutate one
// building at a time so the provider maintains its graph incrementally.
struct Pathfinder {
    void* ctx = nullptr;
    void (*add_obstacle)(void* ctx, uint32_t building_id, const float* poly_xz,
                         int32_t n_verts) = nullptr;
    void (*remove_obstacle)(void* ctx, uint32_t building_id) = nullptr;
    int32_t (*find_path)(void* ctx, float sx, float sz, float gx, float gz, float radius,
                         uint32_t exempt_building, float* out_xz, int32_t cap) = nullptr;
};

// ---- the sim ---------------------------------------------------------------
class Sim {
   public:
    // brain_script_source: noiser source driving spawned entities' brains, or
    // nullptr for mock-brains-only. A script that fails to compile is recorded
    // as a noiser bug and the sim falls back to mock brains.
    explicit Sim(const char* brain_script_source = nullptr);
    ~Sim();
    Sim(Sim&&) noexcept;
    Sim& operator=(Sim&&) noexcept;
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    // Returns the entity id used in CharacterState rows.
    uint32_t Spawn(const CharacterDesc& desc);
    void Tick(float dt);
    // Recompiles the brain script; on failure the previous program is kept
    // (returns false). On success all brains restart on the new program.
    bool ReloadScript(const std::string& source);
    // Executes a player action. Returns >= 0 on success (a new building/hero
    // id, or 0 for id-less actions) and < 0 on error.
    int64_t Dispatch(const Action& action);
    // Registers the nav provider (copied by value) and back-fills every alive
    // building. Pass a default-constructed Pathfinder to clear.
    void SetPathfinder(const Pathfinder& pf);

    // --- Fog-of-war (vision) ----------------------------------------------
    // Sizes/anchors the vision grid (SIM frame). Must be called before the
    // field is meaningful; Tick() resolves vision only once configured. A
    // grid of ceil(world_size / texel_m) texels per axis is allocated.
    // Idempotent for the same params; re-sizing resets the discovered history.
    void ConfigureVision(float world_min_x, float world_min_z, float world_size_x,
                         float world_size_z, float texel_m);
    // Resolve + publish the vision field immediately, WITHOUT advancing the sim
    // (no brains/movement/combat, no tick-counter bump). Tick() also resolves at
    // its end; this is for populating the field before the first render when no
    // Tick has run yet (e.g. a headless single-frame --screenshot). No-op until
    // ConfigureVision().
    void ResolveVision();
    // The published (double-buffered) field. Read by the renderer to upload the
    // vision texture. Empty until ConfigureVision().
    VisionField GetVisionField() const;
    // Object-bounds query: the highest VisionLevel over the grid texels within
    // `radius` of (cx, cz) in the SIM frame (Visible wins over Dormant over
    // Unknown). Drives per-entity render decisions (e.g. hide units). Returns
    // Unknown when the vision grid is unconfigured or the bounds miss it.
    VisionLevel QueryVision(float cx, float cz, float radius) const;

    // Snapshot accessors — identical semantics to the old ABI, POD vectors.
    std::vector<CharacterState> Characters() const;  // was game_state
    std::vector<BuildingState> Buildings() const;    // was game_buildings
    // Out-param overloads: reuse the caller's buffer (out.clear() then fill),
    // avoiding a per-frame allocation on the render path. These are the
    // primitives; the value-returning versions delegate to them.
    void Characters(std::vector<CharacterState>& out) const;
    void Buildings(std::vector<BuildingState>& out) const;
    WorldState World() const;
    // Replaces the tuning factors (see SimFactors). Call before ticking; a
    // replay must use the same factors the recorded run used.
    void SetFactors(const SimFactors& factors);
    const SimFactors& Factors() const;
    // The applied-command trace, oldest-first (see CommandRecord).
    std::vector<CommandRecord> CommandLog() const;                         // was game_world
    // Dominant biome at a world XZ, as a mapgen::Biome index (0 Lake, 1 Swamp,
    // 2 Forest, 3 Plains, 4 Hills, 5 Mountain). The sim owns the terrain field;
    // callers (deer placement, later biome-weighted nav) query it here rather
    // than re-deriving the world<->map offset. Out of bounds clamps.
    int32_t BiomeAt(float world_x, float world_z) const;
    SimStats GetStats() const;                        // was game_stats
    // Placement preview; returns validity, fills out_triangles (was
    // game_probe_placement).
    PlacementProbe ProbePlacement(const PlacementDesc& desc,
                                  std::vector<GridTriangle>& out_triangles) const;

    // The shared world. Later increments read render/sim components off this.
    entt::registry& registry();
    const entt::registry& registry() const;

   private:
    std::unique_ptr<::BadlandsGame> world_;  // the EXISTING internal world, unchanged
};

// ---- handle-less helpers (were game_*; pure computations) ------------------
BuildingDef BuildingDefOf(BuildingKind kind);                     // was game_building_def
RenderBox RenderBoxOf(BuildingKind kind, int32_t rotation_index);  // was game_render_box
CharacterDesc MercenaryDesc(float pos_x, float pos_z);             // was game_desc_mercenary
CharacterDesc GoblinDesc(float pos_x, float pos_z);               // was game_desc_goblin

}  // namespace badlands
