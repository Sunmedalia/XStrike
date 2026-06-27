# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

RustStrike is a BOF (Beacon Object File) C2 skeleton: a server console and a Windows implant that communicate over newline-delimited JSON over TCP. The implant receives a base64-encoded COFF object, loads it **in-process** (no `LoadLibrary`/separate loader), resolves its relocations and external symbols, and executes its `go` entry point. x64 Windows only for execution; the COFF parser is cross-platform and unit-testable anywhere.

## Build & test

A `rust-toolchain.toml` pins **stable MSVC** for this repo — required, because
the default on the dev machine was an old 2025-05 nightly on which `serde_json`'s
transitive dep `zmij` fails to compile (`std::hint::select_unpredictable` was
unstable then). The pin uses the full triple `stable-x86_64-pc-windows-msvc`
because rustup's host detection under MSYS2/Git Bash mis-reports the host as
`x86_64-unknown-linux-gnu`.

Everything builds natively on Windows — no cross-compile target is needed:

```sh
cargo build --release             # whole workspace (server + implant + loader + protocol)
cargo test --release              # protocol + coff + beacon + server + in-process BOF exec
cargo test --release -p ruststrike-loader exec::tests::runs_example_bof -- --nocapture  # the real BOF exec test
cargo test --release -p ruststrike-protocol
cargo clippy --workspace --all-targets
```

The example BOF is a C file that must be compiled to a COFF object before the
loader/exec tests can exercise it (with the MinGW gcc toolchain, e.g. MSYS2):

```sh
gcc -c examples/hello.c -o examples/hello.x64.o        # or: x86_64-w64-mingw32-gcc
```

`examples/hello.x64.o` is gitignored (`*.o`). The `parses_example_bof` and
`runs_example_bof` tests and the end-to-end flow depend on it existing; absent,
tests skip gracefully.

End-to-end (single Windows host): run `ruststrike-server.exe 4444`, then
`ruststrike-implant.exe 127.0.0.1 4444`, then type `hello` / `load examples/hello.x64.o`
at the server console.

## Architecture

Four crates in a single workspace. Dependency direction: `protocol` ← `server`; `protocol` + `loader` ← `implant`.

- **`protocol`** — `ServerMessage` / `ImplantMessage` enums (`#[serde(tag="type", rename_all="lowercase")]`), newline-JSON wire format, and `encode_bof`/`decode_bof` base64 helpers. The single source of truth for the wire contract; both binaries depend on it.
- **`loader`** — the in-process COFF loader. The only crate with platform-specific code:
  - `coff.rs` — **pure, no platform deps.** Parses the PE/COFF header, section table, symbol table (incl. aux records, MS `/nnn/` and GNU/GAS `\0`-prefixed string-table name encodings), and relocations. Computes per-section `image_offset` layout (16-byte aligned). This is the part that's unit-tested on any OS.
  - `exec.rs` (`#[cfg(windows)]`) — `run_bof`: `VirtualAlloc` RWX image, copy sections, resolve externals, write a **thunk** per external symbol, apply AMD64 relocations, `transmute` the `go` symbol to `extern "C" fn(*const u8, i32)` and call it. Also defines Rust implementations of CRT helpers (`memcpy`/`memset`/`strlen`/`__chkstk` etc.) so BOFs don't pull in msvcrt, plus the `LIBRARY$function` and common-DLL symbol resolution.
  - `beacon.rs` (`#[cfg(windows)]`) — the Beacon API stubs (`BeaconPrintf`, `BeaconDataParse`, …) exposed as `extern "C"` and patched into relocations. Output is captured to a thread-local `OUTPUT` buffer that `run_bof` returns.
  - `stub.rs` (`#[cfg(not(windows))]`) — `run_bof` always errors; lets the workspace compile off-Windows so parsing logic stays testable.
- **`server`** — TCP listener (single implant), stdin console (`hello`, `load <path> [args...]`, `quit`). Reader thread decodes `ImplantMessage` lines; main loop encodes commands. `load` reads the file and base64-encodes it.
- **`implant`** — Windows reverse-connect binary (native MSVC build). Reader thread dispatches `ServerMessage` → `handle()`; `Bof` decodes and calls `run_bof`, returning `Output`/`Error`.

### Critical cross-file invariant

The `datap` struct layout in **`examples/beacon.h`** (offsets original=0, size=8, length=12, buffer=16) **must match** `crates/loader/src/beacon.rs` (`DATAP_*` constants). BOFs compile against `beacon.h`; the loader interprets the bytes. Change one, change the other.

### v1 limitations to respect when extending

- `BeaconPrintf` captures only the **first two variadic args** (x64 r8/r9); further specifiers emit literally. The `format_bof` interpreter handles `%d %i %u %x %X %s %c %p`.
- Only data/output Beacon APIs are stubbed; any other `Beacon*` symbol resolves to a no-op that records a note. Add new stubs in `beacon.rs` and register them in `build_external_map` (in `exec.rs`).
- Executable BOF image memory is intentionally **not freed** after execution (kept for process lifetime in v1).
- x64 only; x86 COFFs are rejected at parse time.

### Loader internals worth knowing (non-obvious)

- **Aux records must occupy slots.** COFF symbol-table aux records are kept as
  zeroed placeholder `Symbol { is_aux: true }` entries so that relocation symbol
  indices (which index into the full on-disk table, aux included) line up with
  `Coff::symbols`. Skipping them compacts the vec and breaks every reloc. The
  lookup helpers (`find_defined`, `external_names`) skip `is_aux`.
- **External symbols need thunks.** `VirtualAlloc` routinely returns BOF image
  memory > 2 GB from the implant's code, so a BOF's 32-bit-relative `call`/
  `jmp`/`lea` to an external (e.g. `BeaconPrintf`) cannot span the distance —
  the REL32 displacement truncates and the call jumps into unmapped memory.
  `run_bof` therefore writes one in-image thunk (`jmp [rip+0]; <8-byte abs
  target>`) per external and points REL32 fixups at the thunk. Section-symbol
  targets (e.g. `lea` to `.rdata`) are in-image already and need no thunk.
- **Toolchain reloc-type numbering.** The mingw/binutils-2.46 toolchain
  (`x86_64-w64-mingw32-gcc`) emits AMD64 relocation type values **shifted +1
  from Microsoft's winnt.h**: it writes `3` for ADDR32NB and `4` for REL32
  (winnt.h: `2` and `3`). This was confirmed by linking a BOF with the system
  linker and reading back the resolved displacements — a type-4 fixup resolves
  as REL32 (reference = `fixup_va + 4`), type-3 as ADDR32NB (RVA). `apply_relocation`
  matches the toolchain numbering (3=ADDR32NB, 4=REL32, 5..9=REL32_1..5,
  10/11/12=SECTION/SECREL/SECREL7) and also accepts standard `2` for ADDR32NB.
  Unknown types (e.g. in `.debug_*` sections) are skipped with a note rather
  than failing the load. If you switch BOF compilers (e.g. MSVC `cl`), the
  numbering reverts to winnt.h and this mapping will need adjustment.
- **`add_fn` stores function addresses, not pointer-to-local.** External stubs
  are registered by casting each `extern "C" fn` to `usize`; never revert to
  `&f as *const _`, which captures a dangling stack address.

## Conventions

- Release profile is `opt-level = "z"`, `lto`, `strip` — the implant is meant to be small.
- Workspace deps are pinned in the root `Cargo.toml` `[workspace.dependencies]`; crates reference them with `{ workspace = true }`.
- Keep `unsafe` confined to `exec.rs`/`beacon.rs` (Windows exec path); `coff.rs` and `protocol` are safe.
