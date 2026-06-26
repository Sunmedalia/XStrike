//! ruststrike-loader: in-process BOF (COFF) loader for x64 Windows.
//!
//! Public entry point: [`run_bof`]. Parse + relocation layout logic lives in
//! [`coff`] and is cross-platform/testable; memory loading, symbol resolution
//! and execution are Windows-only.

pub mod coff;
#[cfg(windows)]
pub mod beacon;

#[cfg(windows)]
mod exec;

#[cfg(windows)]
pub use exec::run_bof;

#[cfg(not(windows))]
mod stub;

#[cfg(not(windows))]
pub use stub::run_bof;
