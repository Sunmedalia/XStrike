# zig-implant

A Zig port of `crates/implant` — the RustStrike Windows BOF agent. Functionally
identical to the Rust implant: reverse-connects to the Go core over TCP, speaks
the newline-JSON wire protocol, runs BOFs (COFF object files) **in-process**
via its own COFF loader, returns captured output, and can act as a CS-style TCP
pivot/relay. x64 Windows only.

Two build targets, sharing one root source:

- `zig-implant.exe` — console subsystem (dev; shows a cmd window)
- `zig-implant-silent.exe` — GUI subsystem (`subsystem = .windows`); no console
  window, runs hidden. Operator-deployed form, like `ruststrike-implant-silent`.

## Why Zig 0.16 needs this layout

Zig 0.16 removed `std.net` and `std.fs` file ops (moved into the experimental
`std.Io` framework) and `std.process.argsAlloc`/`GeneralPurposeAllocator`. Since
the implant is Windows-only, this project sidesteps `std.Io` entirely and talks
Win32 directly:

- **Networking** — raw Winsock (`ws2_32`) via `@extern`: `getaddrinfo`/`socket`/
  `connect`/`bind`/`listen`/`accept`/`recv`/`send`. `src/net.zig`.
- **File I/O** — `CreateFileA`/`ReadFile`/`GetFileSizeEx`/`SetFilePointerEx`
  (trailer config + exe path). `src/winapi.zig`.
- **Args** — `GetCommandLineA` + a small tokenizer. `src/agent.zig::parseCliArgs`.
- **Allocator** — `std.heap.smp_allocator` (thread-safe; relay spawns threads).
- **Mutex** — `std.atomic.Mutex` (spinlock; 0.16 has no blocking `Thread.Mutex`).
- **Inline asm** (`__chkstk`) — AT&T syntax (`movq %%rax, %%r11`, …).

## Build

Requires Zig 0.16.0 (MSVC target — the default on Windows).

```sh
zig build                              # debug, both exes
zig build -Doptimize=ReleaseSmall      # release (~140 KB each, stripped)
```

Artifacts: `zig-out/bin/zig-implant.exe`, `zig-out/bin/zig-implant-silent.exe`.

## Run

```sh
# 1. Go core (implant TCP :4444, operator HTTP/WS :8091)
RUSTSTRIKE_AUTH_TOKEN=... RUSTSTRIKE_BOFS=./bofs ./clients/server/server.exe 4444 8091
# 2. Zig implant (reverse-connects to the core)
./zig-out/bin/zig-implant.exe 127.0.0.1 4444
```

Callback host/port resolution (same as the Rust implant):

1. appended-config trailer on the exe (opaque magic + `host\0port\0` near the
   end — read from the last 512 bytes; magic matches `tools/stubbuilder` + the
   Go core `stub_patcher.go`),
2. CLI args: `zig-implant <host> [port]`,
3. defaults `127.0.0.1 4444`.

## Layout

| File | Mirrors | Purpose |
|------|---------|---------|
| `src/main.zig` | `crates/implant/src/bin/*` | entry — calls `agent.run()` |
| `src/agent.zig` | `crates/implant/src/lib.rs` | `run()` loop, trailer + CLI resolution, dispatch hello/bof/relay |
| `src/protocol.zig` | `crates/protocol/src/lib.rs` | wire messages, snake_case `type` discriminator, base64, JSON escaper |
| `src/net.zig` | `std::net` (replaced) | Winsock TCP client + listener + growable line reader |
| `src/relay.zig` | `crates/implant/src/lib.rs` relay | pivot/relay: bind, accept, splice child↔core |
| `src/winapi.zig` | — | Win32 externs (VirtualAlloc, LoadLibrary, file I/O, exe path, cmdline) |
| `src/loader/coff.zig` | `crates/loader/src/obj.rs` | pure COFF parser (sections, symbols incl. aux, relocs, MS + GNU name encoding) |
| `src/loader/exec.zig` | `crates/loader/src/exec.rs` | `runBof`: VirtualAlloc RWX, section copy, external slots (thunk + ptr), AMD64 relocs (mingw +1 numbering), call `go` |
| `src/loader/beacon.zig` | `crates/loader/src/stubs.rs` | Beacon API stubs (CS 4.x `datap`), `BeaconPrintf` format interpreter (r8/r9) |
| `src/loader/crt.zig` | `crates/loader/src/exec.rs` rt_* | memcpy/memset/memmove/memcmp/strlen/`__chkstk` |
| `boftest.zig` | — | standalone loader test harness (not in build.zig) |

## BOF loader test harness

`boftest.zig` runs a `.o` through the same `exec.runBof` path as the implant,
isolated from networking. Handy for debugging the loader:

```sh
zig build-exe boftest.zig -target x86_64-windows-msvc -ODebug
./boftest.exe ../examples/hello.x64.o
./boftest.exe ../examples/sysinfo.x64.o
./boftest.exe ../examples/cmd_exec.x64.o whoami.args   # raw-text arg file
```

## Verified end-to-end (against the Go core)

- reverse-connect + `hello` link check
- BOF exec: `hello`, `sysinfo` (auto-collected on connect, populates the agent
  table), `cmd_exec whoami`, `proc_list`
- operator-driven BOF via REST task-poll (`POST /api/implants/{id}/bof`,
  `GET /api/tasks/{id}`)
- pivot/relay: `relay_listen` on port 0 → `relay_started` with the bound port; a
  child implant connecting through the relay appears at the core as a normal new
  implant (transparent splice)

## Invariants shared with the Rust loader

- **`datap` is CS 4.x** (`beacon.zig`: original=0, buffer=8, length=16, size=20)
  — must match `examples/beacon.h`.
- **Toolchain reloc numbering** — mingw/binutils emits AMD64 types shifted +1
  from winnt.h (3=ADDR32NB, 4=REL32, 5..9=REL32_1..5). `exec.zig::applyRelocation`
  matches the toolchain; section-symbol REL32 targets fall back to the resolved
  in-image address (no thunk slot).
- **`__imp_` imports** — stripped before resolution; `__imp_NAME` gets an 8-byte
  pointer cell, plain `NAME` gets a `jmp [rip+0]; <abs>` thunk.
- **`__chkstk` preserves `rax`** — `crt.zig::rtChkstk` probes 4 KiB pages then
  restores `rax` (the caller does `sub rsp, rax` after).
- **Trailer magic** — `0x7C 0x53 0x9A 0x2E 0xD1 0x04 0xB8 0x6F 0x11 0xA3`,
  shared with `tools/stubbuilder` + Go core `stub_patcher.go`. Change in all.
