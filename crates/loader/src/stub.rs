//! Non-Windows stub. The loader only executes BOFs on Windows; on other
//! platforms `run_bof` returns an error so the workspace still compiles and the
//! pure parsing logic in [`crate::obj`] can be unit-tested.

#![cfg(not(windows))]

use anyhow::Result;

pub fn run_bof(_coff_bytes: &[u8], _args: &[u8]) -> Result<String> {
    anyhow::bail!("ruststrike-loader can only execute BOFs on Windows x64")
}
