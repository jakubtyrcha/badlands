mod app;
mod assets;
mod game;
mod game_ffi;
mod gpu;
mod scene;
mod ui;

// The C++ game static lib (build.rs) references the noiser C FFI; depending on
// the crates here guarantees their rlibs are on the final link line.
use noiser_compiler as _;
use noiser_vm as _;

use winit::event_loop::EventLoop;

use crate::app::{App, RunConfig};

fn parse_args() -> RunConfig {
    let mut config = RunConfig::default();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--frames" => {
                config.max_frames = Some(
                    args.next()
                        .and_then(|value| value.parse().ok())
                        .expect("--frames requires a number"),
                );
            }
            "--screenshot" => {
                config.screenshot_path = Some(
                    args.next().expect("--screenshot requires a path").into(),
                );
            }
            other => panic!("unknown argument: {other}"),
        }
    }
    if config.screenshot_path.is_some() && config.max_frames.is_none() {
        log::warn!("--screenshot without --frames: defaulting to --frames 10");
        config.max_frames = Some(10);
    }
    config
}

fn main() {
    env_logger::init();
    let config = parse_args();
    let event_loop = EventLoop::new().expect("failed to create event loop");
    let mut app = App::new(config);
    event_loop.run_app(&mut app).expect("event loop failed");
}
