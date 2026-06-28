# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This file covers the **frontend** only. The repo-root `CLAUDE.md` covers the Rust
server/agents/BOF system and the cross-layer BOF command registration flow — read it
before changing anything that crosses the frontend↔server boundary. Frontend style: two
spaces, no semicolons; components are `PascalCase.vue`, composables `useName.ts`, stores
`camelCase.ts`.

## Commands

```bash
npm install
npm run typecheck        # vue-tsc --noEmit — there is NO test runner; this is the gate
npm run dev              # web dev server (proxies /api + /health → 127.0.0.1:8080)
npm run dev:client       # remote-client SPA dev (hash history, port 5174, no proxy)
npm run build            # typecheck + vite build → dist/        (embedded by the server)
npm run build:client     # typecheck + vite build → dist-client/ (standalone remote SPA)
```

`npm run build` and `build:client` both run `typecheck` first; type errors fail the
build. Keep `npm run typecheck` green before committing.

## Dual-mode build — one app, two entry points

The same Vue 3 source builds two ways. The mode is chosen by entry point + vite config,
not by runtime flags:

- **Web (server-embedded)** — `index.html` → `main.ts` → `createWebHistory(ROUTER_BASE)`.
  Served by the Ghost server under `/{url_prefix}/`. `ROUTER_BASE` comes from
  `window.__BASE_PATH__` (the random prefix injected by the server at HTML serve time).
- **Remote client** — `index.client.html` → `main.client.ts` → `createWebHashHistory()`.
  Output `dist-client/`. Hash routing so it works from any static host or `file://`.
  Always opens on the `/connect` page; never assumes a local server.

The two `main.*.ts` files duplicate the router + theme bootstrap on purpose (different
histories and guards). `vite.config.client.ts` has two custom plugins: `serveClientIndex`
(rewrite `/` → `/index.client.html` in dev) and `copyAsIndexHtml` (copy the built
`index.client.html` to `index.html` so Tauri/static hosts find it). Don't remove these.

## URL resolution — never hand-build API/WS paths

All API/WS URLs go through `src/runtime/env.ts`. It reads the `useConnectionStore` lazily
(not at import time — Pinia may not be initialized yet) and branches on connection mode:

- **local** → relative paths (`/api`), WS derived from `window.location`.
- **remote** → absolute URLs built from the connection store's `serverOrigin` /
  `wsOrigin` + the user-supplied random path prefix.

Public helpers: `getApiBaseUrl()`, `getApiUrl(path)`, `getWsUrl(path)`,
`getServerOrigin()`, `getDefaultCallbackHost()`, `getAuthHeaders()`, `asset(path)`.
For raw `fetch` use `runtime/network.ts::apiFetch` (adds auth headers). The axios instance
in `services/api.ts` re-evaluates `getApiBaseUrl()` in a request interceptor so
local↔remote switches take effect without recreating the instance. `services/base.ts`
duplicates only the `BASE_PATH`/`ROUTER_BASE`/`asset` constants for the few modules that
must avoid importing the connection store.

## Connection store & auth redirect

`stores/connection.ts` is the source of truth for local-vs-remote mode and remote
coordinates (host/port/path/username). It persists to `localStorage` under
`ghost-connection` (never passwords; username stripped when "remember" is off). The JWT
lives separately in `localStorage` under `token`.

`services/api.ts` holds a lazy `_router` reference (set via `setRouter()` from each entry
point — never import `router/index.ts` at module top level, it creates a circular
`Dashboard → stores → api.ts → router → Dashboard` chain that breaks the client build).
On a `401`, the interceptor clears the token and redirects to `/connect` (remote) or
`/login` (local).

## Backend transport — mock (demo) vs real (Wails desktop)

There is **no real HTTP server** in the Wails desktop build. Every `api.*` call
the Ghost UI makes is intercepted by a custom axios adapter installed in
`services/api.ts`:

- `services/mockMode.ts` — `isMockMode()`: **browser** OR `?demo=1` /
  `localStorage.ghost-demo='1'` → mock; **desktop (Wails)** → real.
  `isRealMode()` is the complement. The Login page has an "Enter demo console"
  button that sets the demo flag.
- `services/mockAdapter.ts` — the **unified axios adapter** (installed in both
  modes via `installBackendAdapter(api)`). It branches per-request:
  - mock → answers from `services/mockData.ts` (sample sessions, BOFs, logs).
  - real → calls the Wails Go bindings (`services/wailsBindings.ts`) which proxy
    the Go service core, which drives the real Rust implant.
- `services/wailsBindings.ts` — typed shim over `window.go.main.App.*` +
  `window.runtime.EventsOn`. **Never** statically imports `../wailsjs/...` so
  the build is robust without regenerated bindings (the `wailsjs/` dir is
  gitignored). Methods: `ListImplants`, `ListBofs`, `RunBofByName`/`RunBofByB64`
  (each returns a `task_id`), `GetTaskResult(taskID)`, `Hello`, `DropImplant`,
  `UploadBof`, `wailsEventsOn`.
- `services/realBackend.ts` — client-side event log fed by the `core:event`
  stream; returned for the store's `GET /logs` call (the real core has no `/logs`
  REST endpoint — output arrives as async events).

Real-mode endpoint mapping (Ghost UI → RustStrike), all preserving the
`{ success, data, error }` envelope so the response interceptor (toasts, 401
redirect) keeps working:

| Ghost UI call | Real action |
|---|---|
| `GET /nodes` | `ListImplants()` (sparse beacon: only id+addr known) |
| `GET /listeners` | `[]` (no listener concept in RustStrike) |
| `GET /bof`, `GET /bof/commands` | `ListBofs()` (+ synthesise `BofCommandMeta`) |
| `POST /bof/execute` | `RunBofByName`/`RunBofByB64` → returns `task_id` |
| `GET /tasks/{id}` | `GetTaskResult(id)` (returns `null` while running) |
| `POST /bof/upload` | `UploadBof(name, b64)` |
| `GET /logs` | client-side `realBackend` log buffer |
| `POST /nodes/{id}/stop|delete`, `/nodes/batch/*` | `DropImplant(id)` |
| `POST /auth/login` | demo token (no real auth) |

The whole UI (stores, command registry, Terminal, modals) runs unchanged
against the real backend — only the transport is swapped. To return to live
HTTP (if a real Ghost server is ever wired in), remove the
`installBackendAdapter(api)` call in `services/api.ts`.

## Event stream — three modes

`composables/useEventStream.ts` subscribes to real-time updates. Branches on
mode:

- **real (desktop)** — subscribes to the Wails `core:event` stream
  (`wailsEventsOn`), forwarded from the Go core's WebSocket. Each event is
  mapped to a log entry (`realBackend.eventToLog`) and dispatched to the store
  callbacks: `implant_connected`/`hello`/`output` → `onBeaconUpdated` +
  `onTaskCompleted`; `implant_disconnected` → `onBeaconDeleted`; `error` →
  `onTaskCompleted(failed)`. No EventSource/ticket.
- **mock (browser demo)** — a 15s `setInterval` emits a synthetic check-in.
- **legacy SSE** — the original `EventSource('/api/events?ticket=')` path is
  retained for the non-Wails web build, but is dead code in the desktop app.

`Dashboard.vue` is the only caller: it instantiates `useEventStream()`, wires
the four callbacks to `appStore.handle*`, and calls `connect()` in `onMounted`.

## Console command registry — the central extensibility surface

`services/commandRegistry.ts` is the console. Three command `type`s: `'local'` (no
network), `'api'` (REST), `'bof'` (execute on a beacon). `dispatch()` tokenizes input
(respecting double-quoted strings), resolves via `findCommand()` (name + aliases,
case-insensitive), enforces `-h`/`--help`, target requirement, and `--force` for
destructive commands before calling `execute`.

**BOF commands are registered dynamically at startup**, not hardcoded:

1. `loadBofCommands()` fetches `GET /api/bof/commands` (plus plugin manifests), then
   `registerBofCommand(meta)` parses `args_json` and builds an encoder via
   `buildEncoder()` keyed on `encode_type`:
   - `none` → no args
   - `beacon_string` → one length-prefixed (2-byte LE + null) UTF-8 string
   - `beacon_string_multi` → each token encoded separately
   - `raw_hex` → 4-byte LE length + raw bytes
   - `raw_hex_short` → 2-byte LE length + raw bytes (ShellcodeExecutor's
     framing for `shellcode_exec`/`shellcode_exec_nt`)
   - `raw_string` → RAW UTF-8 bytes, no length prefix (RustStrike convention:
     `cmd_exec`, `powershell_exec`, `ls`, `download` read the args buffer
     verbatim as text)

   The component-driven BOFs (`proc_kill`/`file_list`/`file_download`/
   `screenshot`/`shellcode_exec*`) are looked up by name from the BOF library
   by their respective views and driven directly via `POST /bof/execute` —
   the components build their own args (`encodeBeaconString` / 2-byte-LE),
   independent of this console encoder. `mockAdapter.ts::bofCommandMetas`
   declares the `encode_type` for each BOF so the **console** path also
   registers them with the right encoder.
2. Each BOF becomes a `CommandDef` with `requiresTarget: true`. `clearBofCommands()`
   rebuilds the set on reload (keeps system/manage commands).
3. Dispatch flows `bofExecute(name, encoder)` → `POST /api/bof/execute` →
   `RunBofByName` (real) / mock (demo) → returns a `task_id` the caller polls.

So a newly uploaded BOF is immediately usable in the console with no frontend code
change. Call `reloadBofCommands()` after upload/delete/edit. New built-in (non-BOF)
commands are added with `def({...})` near the top of the file.

## API response convention (frontend side)

Most endpoints return HTTP 200 with `{ success, data, error }` **even on business
errors**. The axios response interceptor in `services/api.ts` checks for
`success === false` and rejects (toast shown) — so callers must still handle the reject.
Blob error bodies (e.g. failed binary downloads) are parsed as JSON before toasting.
Pass `{ silentError: true }` in the axios config to suppress toasts (used by background
polls in `stores/app.ts`).

## State

Pinia. `stores/app.ts` is the core (beacons, listeners, bofs, logs) with normalizers
(`normalizeBeacon` etc.) that tolerate field-name drift from non-standard agents. Other
stores: `connection`, `toast`, `modal`, `contextMenu`, `plugin`. Views: `Login`,
`Connect` (remote-server connection form), `Dashboard` (the main console workspace).
