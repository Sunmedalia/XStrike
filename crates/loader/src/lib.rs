//! ruststrike-loader: in-process BOF (COFF) loader for x64 Windows.
//!
//! Public entry point: [`run_bof`]. Parse + relocation layout logic lives in
//! [`coff`] and is cross-platform/testable; memory loading, symbol resolution
//! and execution are Windows-only.

// The Beacon API stubs and CRT helpers are `extern "C"` FFI entry points whose
// addresses are patched into BOF relocations and invoked by raw BOF code. They
// take raw pointers and dereference them by design (BOFs pass C pointers, not
// Rust references); marking them `unsafe` would be cosmetic since they are
// never called through Rust's safety gate. Silence clippy's
// `not_unsafe_ptr_arg_deref` crate-wide rather than littering each stub.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

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
