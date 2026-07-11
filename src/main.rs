mod app;
mod game;
mod gpu;
mod scene;
mod ui;

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
    config
}

fn main() {
    env_logger::init();
    let config = parse_args();
    let event_loop = EventLoop::new().expect("failed to create event loop");
    let mut app = App::new(config);
    event_loop.run_app(&mut app).expect("event loop failed");
}
