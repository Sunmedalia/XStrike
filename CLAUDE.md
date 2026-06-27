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

The `datap` struct layout in **`examples/beacon.h`** (Cobalt Strike 4.x: original=0, buffer=8, length=16, size=20) **must match** `crates/loader/src/beacon.rs` (`DATAP_*` constants). BOFs compile against `beacon.h`; the loader interprets the bytes. Change one, change the other.

### Wire contract (for non-Rust servers/clients)

The protocol is newline-delimited JSON over a single TCP stream. `crates/protocol/src/lib.rs` is the source of truth. When implementing the server in another language (Go/Java/…), match these exactly:

- Discriminator is `type`, **lowercase** (`serde(tag="type", rename_all="lowercase")`). Variants: `hello`, `bof` (server→implant); `hello`, `output`, `error` (implant→server).
- Server→implant `bof`: `{"type":"bof","file":"<b64 COFF>","args":"<b64 raw arg buffer>"}`. **`args` is REQUIRED** — the implant's serde `Bof` has no `#[serde(default)]`, so omitting it fails deserialization (`bad server message (missing field args)`). Send `"args":""` for no args. `file`/`args` are base64 (standard alphabet); `args` is the binary CS packed format, not text.
- `hello`: `{"type":"hello"}`. Extra fields on the `hello` variant are tolerated (serde ignores unknown fields), but keep it minimal.
- Implant→server replies: `{"type":"hello|output|error","data":"<utf8 string>"}`.
- Each message is one line terminated by `\n`. Implant output can be large — use a generous line buffer.
- A reference Go server lives in `clients/go-server/` and is verified to drive the Rust implant (hello + load + loadb, incl. the Extension-Kit nbtscan BOF).

### Go service core (`clients/go-server/`)

A standalone operator-facing service (step C of the multi-language plan; the GUI in step B talks to this). Two listeners:
- **TCP** (default `:4444`) — implant transport, same newline-JSON wire contract.
- **HTTP/WS** (default `:8091`) — REST + WebSocket for the GUI.

Run: `go-server [tcp-port] [http-port]`. BOF library dir: `RUSTSTRIKE_BOFS` env, else `./bofs` next to the exe. Build: `cd clients/go-server && go build`.

Files: `session.go` (multi-implant session manager, one reader goroutine each), `bus.go` (event pub/sub), `ws.go` (dependency-free RFC6455 server, pushes `Event`s), `api.go` (REST), `boflib.go`+`boflib_api.go` (BOF library index + upload/run), `task.go` (BOF task-result store), `protocol.go` (wire types mirroring `crates/protocol`).

REST: `GET /api/implants`, `POST /api/implants/{id}/hello`, `POST /api/implants/{id}/bof` (body `{bof, args}` — `bof` is a library name or raw base64 COFF, `args` base64; returns `{task_id}`), `DELETE /api/implants/{id}`, `GET/POST /api/bofs`, `POST /api/bofs/{name}/run?implant=<id>` (body `{args}`; returns `{task_id}`), `GET /api/tasks/{id}` (poll a BOF task → `{id, implant_id, status, output}`; `status` is `running` until the implant's output/error arrives, then `completed`/`failed`). WS: `GET /ws` streams `{type, implant_id, data}` events (`implant_connected`/`implant_disconnected`/`hello`/`output`/`error`).

**BOF task correlation (`task.go`).** RustStrike BOFs are synchronous and one-shot — the implant runs the BOF to completion, then sends exactly one `output` (or `error`) message. So a BOF run creates a task (`tasks.Create(implantID)`, BEFORE `Send` so a fast reply can't race it), and the NEXT `output`/`error` event for that implant completes it (`tasks.Feed`, called from `session.go` readLoop). The GUI polls `GET /api/tasks/{id}` to pull the result — this bridges RustStrike's fire-and-forget event model onto the request/response task-polling the Ghost UI expects. One active task per implant at a time (Create supersedes the prior); a reaper trims tasks older than 10 min.

Verified end-to-end with the Rust implant: session list, hello, BOF-by-b64, BOF-by-name (nbtscan/ps/ls/cmd_exec/download/upload, plus the component-driven proc_list/proc_kill/file_list/file_download/screenshot/powershell_exec/shellcode_exec/shellcode_exec_nt), task-poll output, and live WebSocket event push all work.

### Wails GUI (`clients/wails-gui/`)

Step B: a desktop operator console. Go backend via Wails v2; the frontend is a self-contained Vue 3 + TS + Pinia + vue-router console ("Ghost UI") that was integrated from `frontend/` into `clients/wails-gui/frontend/`. In desktop mode it drives the **real** Rust implant via the Go core; a mock/demo mode is available for browser preview.

- `app.go` — Wails-bound methods the frontend calls via the shim: `ListImplants`, `Hello`, `RunBofByName`/`RunBofByB64` (each returns a `task_id` string), `GetTaskResult(taskID)` (polls a BOF task → `TaskResult{status, output}`), `DropImplant`, `ListBofs`, `UploadBof`. Each wraps a core REST call.
- `core.go` — connects to the core's `/ws`, re-emits every event to the frontend as a Wails `core:event` (auto-reconnects). Core address via `RUSTSTRIKE_CORE` (default `http://127.0.0.1:8091`).
- `frontend/` — the integrated console. See `frontend/CLAUDE.md` for the full frontend contract. **Real-backend wiring:** `services/wailsBindings.ts` (typed shim over `window.go.main.App.*` + `window.runtime.EventsOn`, no static wailsjs imports so the build is robust without regenerated bindings), `services/realBackend.ts` (client-side event log fed by `core:event`, returned for `/logs`), `services/mockAdapter.ts` (unified axios adapter: branches mock vs real per-request — real mode maps every Ghost-UI axios call to a Wails binding), `services/mockMode.ts` (`isMockMode()`: browser OR `?demo=1`/`ghost-demo` flag → mock; desktop → real). The whole UI (stores, command registry, Terminal, modals) runs unchanged against the real backend — only the transport is swapped. `composables/useEventStream.ts` subscribes to the Wails `core:event` stream in real mode (no SSE/ticket).
- **Mock / demo mode** — browser dev (`npm run dev`) and `?demo=1`/`localStorage.ghost-demo='1'` run off `services/mockData.ts` so the console is usable with zero backend. The Login page has an "Enter demo console" button. Clear the flag (or run the desktop exe) for real mode.

Run dev (hot reload): `cd clients/wails-gui && wails dev` (vite pinned to `127.0.0.1:5173` strictPort in `frontend/vite.config.ts` to match `wails.json`). Build single exe: `wails build` → `build/bin/wails-gui.exe`. Wails CLI: `go install github.com/wailsapp/wails/v2/cmd/wails@latest`.

Verified: `npm run build` (typecheck + vite) clean, `wails build` produces `wails-gui.exe`, and the real backend chain (core task store + implant + ps/ls/cmd_exec/download/upload) is verified end-to-end via REST. Visual/interactive GUI verification requires running `wails dev` on a desktop session.

### Running the whole stack

```sh
# 1. Go service core (implant TCP :4444, operator HTTP/WS :8091)
RUSTSTRIKE_BOFS=./clients/go-server/bofs ./clients/go-server/go-server.exe 4444 8091
# 2. Rust implant (reverse-connects to core)
./target/release/ruststrike-implant.exe 127.0.0.1 4444
# 3. GUI (desktop console over the core's API)
cd clients/wails-gui && wails dev   # or: build/bin/wails-gui.exe
```

### v1 limitations to respect when extending

- `BeaconPrintf` captures only the **first two variadic args** (x64 r8/r9); further specifiers emit literally. The `format_bof` interpreter handles `%d %i %u %x %X %s %c %p`.
- Only data/output Beacon APIs are stubbed; any other `Beacon*` symbol resolves to a no-op that records a note. Add new stubs in `beacon.rs` and register them in `build_external_map` (in `exec.rs`).
- Executable BOF image memory is intentionally **not freed** after execution (kept for process lifetime in v1).
- x64 only; x86 COFFs are rejected at parse time.
- **Two arg conventions coexist** — don't conflate them. The loader's
  `BeaconDataExtract` reads a **4-byte big-endian** length (CS packed, used by
  `upload` and third-party BOFs like `nbtscan`). But the RustStrike frontend's
  `encodeBeaconString` (`commandRegistry.ts`) frames strings as **2-byte
  little-endian length + UTF-8 + null** — a *different* framing the loader's
  Beacon APIs can't walk. The component-driven BOFs (`proc_list`/`proc_kill`/
  `file_list`/`file_download`/`screenshot`/`shellcode_exec*`) parse that
  2-byte-LE framing with an inline reader, **not** `BeaconDataExtract`. This is
  intentional (matches the components); don't "fix" them to use CS packed. The
  Terminal-driven BOFs (`cmd_exec`/`powershell_exec`/`ls`/`download`) keep the
  raw-text convention (no prefix). See `examples/README.md` "Arg formats".

### BOF compatibility (third-party BOFs)

The loader runs Cobalt-Strike-4.x / AdaptixC2-style BOFs. Verified end-to-end
with an AdaptixC2 [Extension-Kit](https://github.com/Adaptix-Framework/Extension-Kit)
BOF (`SAR-BOF/nbtscan`). To use a third-party BOF:

1. Build it with mingw (`x86_64-w64-mingw32-gcc`), the same toolchain whose
   reloc numbering the loader matches (see below). Extension-Kit builds via its
   `make` / CMake, or per-BOF: `gcc -I Extension-Kit/_include -DBOF -c bof.c -o bof.x64.o`.
2. The BOF's externals are `__imp_LIBRARY$function` imports (kernel32/ws2_32/
   msvcrt/…) and `__imp_Beacon*` — the loader resolves both via `LoadLibrary`/
   `GetProcAddress` and the Beacon stubs. Any `Beacon*` API not stubbed falls
   back to a no-op note.
3. BOF args are **binary** (the CS packed format: big-endian length-prefixed
   blobs + ints walked by `BeaconDataParse`/`Extract`/`Int`). Transport encodes
   them base64. From the server console use `loadb <bof.o> <args.bin>` (reads a
   raw arg file); `load <bof.o> [text]` is for BOFs that treat args as text.
4. Only the data/output Beacon APIs and basic CRT are stubbed. BOFs that call
   `BeaconUseToken`/`BeaconGetSpawnTo`/`Beacon*VirtualAlloc`/`BeaconInformation`
   etc. will hit the no-op stub and misbehave — implement those in `beacon.rs`
   + `build_external_map` as needed. `format`/token/spawn-inject APIs are not
   implemented at all.

### Loader internals worth knowing (non-obvious)

- **Aux records must occupy slots.** COFF symbol-table aux records are kept as
  zeroed placeholder `Symbol { is_aux: true }` entries so that relocation symbol
  indices (which index into the full on-disk table, aux included) line up with
  `Coff::symbols`. Skipping them compacts the vec and breaks every reloc. The
  lookup helpers (`find_defined`, `external_names`) skip `is_aux`.
- **External symbols need in-image slots.** `VirtualAlloc` routinely returns
  BOF image memory > 2 GB from the implant's code, so a BOF's 32-bit-relative
  reference to an external (e.g. `BeaconPrintf`) cannot span the distance — the
  REL32 displacement truncates and jumps into unmapped memory. `run_bof` writes
  one in-image slot per external and points REL32 fixups at it. Two slot kinds,
  chosen by symbol name:
  - `__imp_NAME` (mingw import pointer — how AdaptixC2 Extension-Kit / CS-style
    BOFs reference imports): the BOF does `mov rax,[rip+disp]; call rax`, so
    `disp` must land on an **8-byte cell holding the function address** (a data
    pointer, not code). `resolve_symbol` strips the `__imp_` prefix first.
  - plain `NAME` (direct call): the BOF does `call rel32`, so the slot must be
    **executable** — a `jmp [rip+0]; <8-byte abs target>` thunk.
  Section-symbol targets (e.g. `lea` to `.rdata`) are in-image already and need
  no slot.
- **`datap` layout is Cobalt Strike 4.x.** `beacon.rs` uses the CS 4.x `datap`
  layout (`original`+0, `buffer`+8, `length`+16, `size`+20). `examples/beacon.h`
  matches. An earlier RustStrike-only layout (original/size/length/buffer) was
  non-standard and broke real BOFs — keep these in sync with CS.
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
