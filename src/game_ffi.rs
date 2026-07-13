// Safe wrapper over the C++ game simulation (game/include/badlands_game.h).

use crate::nav;
use glam::Vec2;
use std::ffi::CString;
use std::os::raw::{c_char, c_void};

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameCharacterDesc {
    pub pos_x: f32,
    pub pos_z: f32,
    pub team: i32,
    pub hp: f32,
    pub move_speed: f32,
    pub attack_range: f32,
    pub attack_damage: f32,
    pub attack_cooldown: f32,
    pub size_x: f32,
    pub size_y: f32,
    pub size_z: f32,
    pub color_r: f32,
    pub color_g: f32,
    pub color_b: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameCharacterState {
    pub id: u32,
    pub team: i32,
    pub pos_x: f32,
    pub pos_z: f32,
    pub hp: f32,
    pub max_hp: f32,
    pub size_x: f32,
    pub size_y: f32,
    pub size_z: f32,
    pub color_r: f32,
    pub color_g: f32,
    pub color_b: f32,
    pub home_building_id: i32,   // recruiting guild; -1 = homeless / not a hero
    pub inside_building_id: i32, // -1 = outside; >=0 => hidden (don't draw)
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameStats {
    pub ticks: u64,
    pub script_intents: u64,
    pub noiser_bugs: u32,
}

// Grid half-extent in tiles (mirror of GAME_GRID_HALF_EXTENT_TILES; the sim is
// authoritative — cross-checked against game_world() in a test).
pub const GRID_HALF_EXTENT_TILES: i32 = 48;

// Mirror of GameBuildingKind. Discriminants must match the C enum order.
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BuildingKind {
    Castle = 0,
    FreeCompanyQuarters,
    HuntersCamp,
    ThievesDen,
    Scriptorium,
    Tavern,
    Apothecary,
    Watchtower,
    House,
    Sewer,
}

impl BuildingKind {
    // The buildings a player can place from the sidebar (excludes the prebuilt
    // castle and the auto-spawned poppables).
    pub const PLAYER_PLACEABLE: [BuildingKind; 7] = [
        BuildingKind::FreeCompanyQuarters,
        BuildingKind::HuntersCamp,
        BuildingKind::ThievesDen,
        BuildingKind::Scriptorium,
        BuildingKind::Tavern,
        BuildingKind::Apothecary,
        BuildingKind::Watchtower,
    ];

    pub fn from_i32(kind: i32) -> BuildingKind {
        match kind {
            0 => BuildingKind::Castle,
            1 => BuildingKind::FreeCompanyQuarters,
            2 => BuildingKind::HuntersCamp,
            3 => BuildingKind::ThievesDen,
            4 => BuildingKind::Scriptorium,
            5 => BuildingKind::Tavern,
            6 => BuildingKind::Apothecary,
            7 => BuildingKind::Watchtower,
            8 => BuildingKind::House,
            _ => BuildingKind::Sewer,
        }
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameBuildingDef {
    pub width_tiles: i32,
    pub depth_tiles: i32,
    pub poppable: u32,
    pub user_destructible: u32,
    pub enemy_targettable: u32,
}

// Mirror of GameActionKind. Discriminants must match the C enum order (asserted
// by a test below and pinned C-side with static_assert in game.cpp).
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GameActionKind {
    PlaceBuilding = 0,
    RecruitHero = 1,
    DestroyBuilding = 2,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameAction {
    pub kind: i32,
    pub target_id: u32,
    pub world_x: f32,
    pub world_z: f32,
    pub param_a: i32,
    pub param_b: i32,
}

// The world-space box to draw for a building so the cuboid matches its grid
// footprint. size is local (pre-rotation) X/Z extent; apply yaw_radians about Y.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameRenderBox {
    pub size_x: f32,
    pub size_z: f32,
    pub yaw_radians: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GamePlacementDesc {
    pub kind: i32,
    pub rotation_index: i32,
    pub world_x: f32,
    pub world_z: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameGridTriangle {
    pub tile_x: i32,
    pub tile_z: i32,
    pub corner: u32,
    pub state: u32, // 0 = free, 1 = blocked, 2 = would-block
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GamePlacementProbe {
    pub valid: u32,
    pub snapped_x: f32,
    pub snapped_z: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameBuildingState {
    pub id: u32,
    pub kind: i32,
    pub center_x: f32,
    pub center_z: f32,
    pub rotation_index: i32,
    pub width_tiles: i32,
    pub depth_tiles: i32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GameWorldState {
    pub gold: u32,
    pub grid_half_extent_tiles: i32,
    pub queued_poppables: u32,
    pub urban_quarters: u32,
}

// Mirror of GamePathfinder: a pluggable path-geometry provider. We construct
// one wrapping a Rust NavContext and register it via game_set_pathfinder.
#[repr(C)]
pub struct GamePathfinder {
    pub ctx: *mut c_void,
    pub add_obstacle: unsafe extern "C" fn(*mut c_void, u32, *const f32, i32),
    pub remove_obstacle: unsafe extern "C" fn(*mut c_void, u32),
    pub find_path:
        unsafe extern "C" fn(*mut c_void, f32, f32, f32, f32, f32, u32, *mut f32, i32) -> i32,
}

#[repr(C)]
struct BadlandsGame {
    _opaque: [u8; 0],
}

unsafe extern "C" {
    fn game_create(brain_script_source: *const c_char) -> *mut BadlandsGame;
    fn game_destroy(game: *mut BadlandsGame);
    fn game_spawn(game: *mut BadlandsGame, desc: *const GameCharacterDesc) -> u32;
    fn game_tick(game: *mut BadlandsGame, dt: f32);
    fn game_state(game: *const BadlandsGame, out: *mut GameCharacterState, cap: u32) -> u32;
    fn game_stats(game: *const BadlandsGame, out: *mut GameStats);
    fn game_reload_script(game: *mut BadlandsGame, source: *const c_char) -> bool;
    fn game_desc_mercenary(pos_x: f32, pos_z: f32) -> GameCharacterDesc;
    fn game_desc_goblin(pos_x: f32, pos_z: f32) -> GameCharacterDesc;

    fn game_probe_placement(
        game: *const BadlandsGame,
        desc: *const GamePlacementDesc,
        out: *mut GamePlacementProbe,
        out_triangles: *mut GameGridTriangle,
        cap: u32,
    ) -> u32;
    fn game_dispatch(game: *mut BadlandsGame, action: *const GameAction) -> i64;
    fn game_buildings(game: *const BadlandsGame, out: *mut GameBuildingState, cap: u32) -> u32;
    fn game_world(game: *const BadlandsGame, out: *mut GameWorldState);
    fn game_building_def(kind: i32) -> GameBuildingDef;
    fn game_render_box(kind: i32, rotation_index: i32) -> GameRenderBox;
    fn game_set_pathfinder(game: *mut BadlandsGame, pathfinder: *const GamePathfinder);
}

// Static footprint size + poppable flag for a kind (no game handle needed).
pub fn building_def(kind: BuildingKind) -> GameBuildingDef {
    unsafe { game_building_def(kind as i32) }
}

// The box to render for a (kind, rotation) so the drawn cuboid matches the grid
// footprint. Handles the diagonal lattice-diamond spans, which are not (w,d).
pub fn render_box(kind: i32, rotation_index: i32) -> GameRenderBox {
    unsafe { game_render_box(kind, rotation_index) }
}

// Point-in-oriented-box test in the ground (XZ) plane. `half` is the box's
// half-extents; `yaw` matches the renderer's Mat4::from_rotation_y, so a pick
// agrees with what is drawn. Inverts R_y to test in the box's local frame.
pub fn point_in_oriented_box(center: Vec2, half: Vec2, yaw: f32, p: Vec2) -> bool {
    let d = p - center;
    let (s, c) = yaw.sin_cos();
    let local = Vec2::new(c * d.x - s * d.y, s * d.x + c * d.y);
    local.x.abs() <= half.x && local.y.abs() <= half.y
}

// The topmost building whose drawn oriented box contains `world`, or None. Later
// (last-placed) buildings win on overlap, matching draw order.
pub fn building_at_world(buildings: &[GameBuildingState], world: Vec2) -> Option<u32> {
    for b in buildings.iter().rev() {
        let bx = render_box(b.kind, b.rotation_index);
        let center = Vec2::new(b.center_x, b.center_z);
        let half = Vec2::new(bx.size_x * 0.5, bx.size_z * 0.5);
        if point_in_oriented_box(center, half, bx.yaw_radians, world) {
            return Some(b.id);
        }
    }
    None
}

// Canonical Stage-2 duelists, defined once in C++ and shared with the tests.
pub fn mercenary_desc(pos_x: f32, pos_z: f32) -> GameCharacterDesc {
    unsafe { game_desc_mercenary(pos_x, pos_z) }
}

pub fn goblin_desc(pos_x: f32, pos_z: f32) -> GameCharacterDesc {
    unsafe { game_desc_goblin(pos_x, pos_z) }
}

pub struct Game {
    handle: *mut BadlandsGame,
    // Rust path-geometry provider handed to the sim via game_set_pathfinder.
    // Owned here (raw box) so it outlives the sim; freed in Drop after the sim.
    nav: *mut nav::NavContext,
}

impl Game {
    /// `brain_script` of None runs mock brains only. A bad script — compile
    /// failure (recorded C++-side) or an interior NUL byte — degrades to
    /// mocks instead of failing.
    pub fn new(brain_script: Option<&str>) -> Game {
        let source = brain_script.and_then(|s| {
            CString::new(s)
                .inspect_err(|_| {
                    log::error!("brain script contains a NUL byte; running mock brains only");
                })
                .ok()
        });
        let ptr = source.as_ref().map_or(std::ptr::null(), |s| s.as_ptr());
        let handle = unsafe { game_create(ptr) };

        // Register the Rust nav path service. game_set_pathfinder copies the
        // vtable by value and back-fills any already-placed buildings (castle).
        let nav = Box::into_raw(Box::new(nav::NavContext::new()));
        let pathfinder = GamePathfinder {
            ctx: nav as *mut c_void,
            add_obstacle: nav::nav_add_obstacle,
            remove_obstacle: nav::nav_remove_obstacle,
            find_path: nav::nav_find_path,
        };
        unsafe { game_set_pathfinder(handle, &pathfinder) };

        Game { handle, nav }
    }

    pub fn spawn(&mut self, desc: &GameCharacterDesc) -> u32 {
        unsafe { game_spawn(self.handle, desc) }
    }

    pub fn tick(&mut self, dt: f32) {
        unsafe { game_tick(self.handle, dt) }
    }

    /// Snapshot of the living entities. game_state returns the total living
    /// count, so one retry with a grown buffer always suffices.
    pub fn state(&self) -> Vec<GameCharacterState> {
        let mut rows = vec![GameCharacterState::default(); 256];
        loop {
            let total =
                unsafe { game_state(self.handle, rows.as_mut_ptr(), rows.len() as u32) } as usize;
            if total <= rows.len() {
                rows.truncate(total);
                return rows;
            }
            rows.resize(total, GameCharacterState::default());
        }
    }

    pub fn stats(&self) -> GameStats {
        let mut stats = GameStats::default();
        unsafe { game_stats(self.handle, &mut stats) };
        stats
    }

    /// Recompiles the brain script; keeps the previous program on failure.
    pub fn reload_script(&mut self, source: &str) -> bool {
        let Ok(source) = CString::new(source) else {
            log::error!("brain script contains a NUL byte; keeping the previous program");
            return false;
        };
        unsafe { game_reload_script(self.handle, source.as_ptr()) }
    }

    /// Snapshot of every placed building (stable, dense ids), grown like state().
    pub fn buildings(&self) -> Vec<GameBuildingState> {
        let mut rows = vec![GameBuildingState::default(); 64];
        loop {
            let total =
                unsafe { game_buildings(self.handle, rows.as_mut_ptr(), rows.len() as u32) } as usize;
            if total <= rows.len() {
                rows.truncate(total);
                return rows;
            }
            rows.resize(total, GameBuildingState::default());
        }
    }

    /// World scalars (gold, grid size, sprawl bookkeeping).
    pub fn world(&self) -> GameWorldState {
        let mut world = GameWorldState::default();
        unsafe { game_world(self.handle, &mut world) };
        world
    }

    /// Read-only placement preview. Fills `triangles` with the grid readout and
    /// returns the snapped center + validity.
    pub fn probe_placement(
        &self,
        desc: &GamePlacementDesc,
        triangles: &mut Vec<GameGridTriangle>,
    ) -> GamePlacementProbe {
        if triangles.is_empty() {
            triangles.resize(4096, GameGridTriangle::default());
        }
        let mut probe = GamePlacementProbe::default();
        loop {
            let total = unsafe {
                game_probe_placement(
                    self.handle,
                    desc,
                    &mut probe,
                    triangles.as_mut_ptr(),
                    triangles.len() as u32,
                )
            } as usize;
            if total <= triangles.len() {
                triangles.truncate(total);
                return probe;
            }
            triangles.resize(total, GameGridTriangle::default());
        }
    }

    /// The single player->world action entry point. Returns >= 0 on success
    /// (a new building/hero id, or 0 for id-less actions), < 0 on error.
    pub fn dispatch(&mut self, action: GameAction) -> i64 {
        unsafe { game_dispatch(self.handle, &action) }
    }

    /// Places a building; returns its id, or None if the snapped footprint is
    /// invalid (sim state left untouched). Thin builder over dispatch.
    pub fn place_building(&mut self, desc: &GamePlacementDesc) -> Option<u32> {
        let r = self.dispatch(GameAction {
            kind: GameActionKind::PlaceBuilding as i32,
            world_x: desc.world_x,
            world_z: desc.world_z,
            param_a: desc.kind,
            param_b: desc.rotation_index,
            ..Default::default()
        });
        (r >= 0).then_some(r as u32)
    }

    /// Recruits a hero at the given guild building; returns the new hero id or
    /// None on rejection (not a guild, roster full, no free approach tile).
    pub fn recruit(&mut self, building_id: u32) -> Option<u32> {
        let r = self.dispatch(GameAction {
            kind: GameActionKind::RecruitHero as i32,
            target_id: building_id,
            ..Default::default()
        });
        (r >= 0).then_some(r as u32)
    }

    /// Destroys a user-destructible building; returns true on success.
    pub fn destroy_building(&mut self, building_id: u32) -> bool {
        self.dispatch(GameAction {
            kind: GameActionKind::DestroyBuilding as i32,
            target_id: building_id,
            ..Default::default()
        }) >= 0
    }
}

impl Drop for Game {
    fn drop(&mut self) {
        // Destroy the sim first (it holds a copy of the vtable + ctx), then
        // free the nav box the ctx pointed at.
        unsafe { game_destroy(self.handle) };
        if !self.nav.is_null() {
            drop(unsafe { Box::from_raw(self.nav) });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dummy_desc() -> GameCharacterDesc {
        GameCharacterDesc {
            hp: 10.0,
            move_speed: 1.0,
            attack_range: 1.0,
            attack_damage: 1.0,
            attack_cooldown: 1.0,
            size_x: 1.0,
            size_y: 1.0,
            size_z: 1.0,
            ..Default::default()
        }
    }

    #[test]
    fn nul_in_script_falls_back_to_mock_brains() {
        // A script with an interior NUL must degrade like any other bad
        // script (mock brains), not crash.
        let mut game = Game::new(Some("pub fn main() -> f32 { 0.0 }\0"));
        game.spawn(&dummy_desc());
        game.tick(1.0 / 30.0);
        assert_eq!(game.state().len(), 1);
    }

    #[test]
    fn reload_with_nul_is_rejected() {
        let mut game = Game::new(None);
        assert!(!game.reload_script("x\0y"));
    }

    #[test]
    fn state_reports_every_living_entity() {
        // game_spawn is unbounded, so the snapshot must be too.
        let mut game = Game::new(None);
        for _ in 0..300 {
            game.spawn(&dummy_desc());
        }
        assert_eq!(game.state().len(), 300);
    }

    #[test]
    fn fresh_game_starts_with_the_castle_and_starting_gold() {
        let game = Game::new(None);
        let buildings = game.buildings();
        assert_eq!(buildings.len(), 1);
        assert_eq!(BuildingKind::from_i32(buildings[0].kind), BuildingKind::Castle);

        let world = game.world();
        assert_eq!(world.gold, 1000);
        assert_eq!(world.grid_half_extent_tiles, GRID_HALF_EXTENT_TILES);
    }

    #[test]
    fn game_action_kind_discriminants_match_the_c_enum() {
        // Pinned C-side with static_assert in game.cpp; both sides key off 0/1/2.
        assert_eq!(GameActionKind::PlaceBuilding as i32, 0);
        assert_eq!(GameActionKind::RecruitHero as i32, 1);
        assert_eq!(GameActionKind::DestroyBuilding as i32, 2);
    }

    #[test]
    fn dispatch_places_and_rejects_via_the_generic_trigger() {
        let mut game = Game::new(None);
        // PLACE_BUILDING round-trips like the old game_place_building.
        let placed = game.place_building(&GamePlacementDesc {
            kind: BuildingKind::Watchtower as i32,
            rotation_index: 0,
            world_x: 24.0,
            world_z: 24.0,
        });
        let id = placed.expect("placement should succeed");

        // Unknown action kind -> error.
        assert!(game.dispatch(GameAction { kind: 999, ..Default::default() }) < 0);
        // Recruit is not wired until Phase 5.
        assert!(game.recruit(id).is_none());
        // Castle (id 0) is not user-destructible.
        assert!(!game.destroy_building(0));
        // The placed watchtower is destructible.
        assert!(game.destroy_building(id));
        assert!(!game.destroy_building(id)); // already gone
    }

    #[test]
    fn building_def_flags_are_decoupled() {
        let castle = building_def(BuildingKind::Castle);
        assert_eq!((castle.user_destructible, castle.enemy_targettable), (0, 1));
        let guild = building_def(BuildingKind::FreeCompanyQuarters);
        assert_eq!((guild.user_destructible, guild.enemy_targettable), (1, 0));
    }

    #[test]
    fn glam_rotation_y_sign_is_pinned() {
        use glam::{Mat4, Vec3};
        // Oriented-box picking inverts this rotation; if glam ever flips the
        // handedness, picks would silently mis-rotate. (1,0,0) about +90deg -> (0,0,-1).
        let r = Mat4::from_rotation_y(std::f32::consts::FRAC_PI_2) * Vec3::X.extend(1.0);
        assert!(r.x.abs() < 1e-5);
        assert!((r.z + 1.0).abs() < 1e-5);
    }

    #[test]
    fn point_in_oriented_box_axis_and_diamond() {
        use std::f32::consts::FRAC_PI_4;
        let center = Vec2::new(5.0, -3.0);
        let half = Vec2::new(1.0, 0.5);
        // Axis-aligned.
        assert!(point_in_oriented_box(center, half, 0.0, center + Vec2::new(0.9, 0.4)));
        assert!(!point_in_oriented_box(center, half, 0.0, center + Vec2::new(1.1, 0.0)));
        assert!(!point_in_oriented_box(center, half, 0.0, center + Vec2::new(0.0, 0.6)));
        // 45deg: local axes now point diagonally in world space.
        let along_x = Vec2::new(FRAC_PI_4.cos(), -FRAC_PI_4.sin());
        let along_y = Vec2::new(FRAC_PI_4.sin(), FRAC_PI_4.cos());
        assert!(point_in_oriented_box(center, half, FRAC_PI_4, center + along_x * 0.9));
        assert!(!point_in_oriented_box(center, half, FRAC_PI_4, center + along_x * 1.2));
        assert!(point_in_oriented_box(center, half, FRAC_PI_4, center + along_y * 0.4));
        assert!(!point_in_oriented_box(center, half, FRAC_PI_4, center + along_y * 0.6));
    }

    #[test]
    fn building_at_world_picks_the_footprint_and_misses_outside() {
        let mut game = Game::new(None);
        let id = game
            .place_building(&GamePlacementDesc {
                kind: BuildingKind::FreeCompanyQuarters as i32,
                rotation_index: 0,
                world_x: 24.0,
                world_z: -8.0,
            })
            .unwrap();
        let buildings = game.buildings();
        let b = buildings.iter().find(|b| b.id == id).unwrap();
        assert_eq!(
            building_at_world(&buildings, Vec2::new(b.center_x, b.center_z)),
            Some(id)
        );
        assert_eq!(building_at_world(&buildings, Vec2::new(40.0, 40.0)), None);
    }

    #[test]
    fn probe_then_place_round_trips_and_rejects_overlap() {
        let mut game = Game::new(None);
        let desc = GamePlacementDesc {
            kind: BuildingKind::Tavern as i32,
            rotation_index: 0,
            world_x: 20.0,
            world_z: 20.0,
        };

        let mut triangles = Vec::new();
        let probe = game.probe_placement(&desc, &mut triangles);
        assert_eq!(probe.valid, 1);
        assert!(!triangles.is_empty());

        // Placing at the probed spot succeeds; the snapshot grows by one.
        assert!(game.place_building(&desc).is_some());
        assert_eq!(game.buildings().len(), 2 + game.world().urban_quarters as usize / 4);

        // A second building on the same footprint is rejected.
        assert!(game.place_building(&desc).is_none());
    }
}
