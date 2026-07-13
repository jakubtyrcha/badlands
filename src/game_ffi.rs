// Safe wrapper over the C++ game simulation (game/include/badlands_game.h).

use std::ffi::CString;
use std::os::raw::c_char;

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
    fn game_place_building(game: *mut BadlandsGame, desc: *const GamePlacementDesc) -> u32;
    fn game_buildings(game: *const BadlandsGame, out: *mut GameBuildingState, cap: u32) -> u32;
    fn game_world(game: *const BadlandsGame, out: *mut GameWorldState);
    fn game_building_def(kind: i32) -> GameBuildingDef;
    fn game_render_box(kind: i32, rotation_index: i32) -> GameRenderBox;
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

// Canonical Stage-2 duelists, defined once in C++ and shared with the tests.
pub fn mercenary_desc(pos_x: f32, pos_z: f32) -> GameCharacterDesc {
    unsafe { game_desc_mercenary(pos_x, pos_z) }
}

pub fn goblin_desc(pos_x: f32, pos_z: f32) -> GameCharacterDesc {
    unsafe { game_desc_goblin(pos_x, pos_z) }
}

pub struct Game {
    handle: *mut BadlandsGame,
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
        Game {
            handle: unsafe { game_create(ptr) },
        }
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

    /// Places a building; returns its id, or None if the snapped footprint is
    /// invalid (sim state left untouched).
    pub fn place_building(&mut self, desc: &GamePlacementDesc) -> Option<u32> {
        let id = unsafe { game_place_building(self.handle, desc) };
        (id != u32::MAX).then_some(id)
    }
}

impl Drop for Game {
    fn drop(&mut self) {
        unsafe { game_destroy(self.handle) };
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
