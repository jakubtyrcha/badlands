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
}
