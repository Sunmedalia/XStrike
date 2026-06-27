# Example BOFs

RustStrike ships a small BOF library under `examples/`. Each is a C file
compiled to an x64 COFF object and exposes the standard `go(char* args, int alen)`
entry point.

| BOF | Args | What it does |
|---|---|---|
| `hello.c` | none | Test BOF. `BeaconPrintf(CALLBACK_OUTPUT, "hello from bof")`. |
| `cmd_exec.c` | raw text (the command) | Runs `cmd.exe /c <cmd>`, captures stdout/stderr. Converts the console's OEM/ANSI codepage (GBK/CP936) to UTF-8 before returning, so Chinese output renders cleanly in the GUI. |
| `ps.c` | none | Lists processes via `CreateToolhelp32Snapshot` (`PID PPID NAME`). |
| `ls.c` | raw text path (optional, default `.`) | Lists a directory via `FindFirstFileA` (`TYPE SIZE NAME`). |
| `download.c` | raw text path | Reads a file (≤2 MB) and prints it base64-encoded; the operator decodes to recover the file. Binary-safe via `BeaconOutput(len)`. |
| `upload.c` | CS packed: two `BeaconDataExtract` blobs (remote path, then file contents) | Writes `content` to `remote path` (`CREATE_ALWAYS`). Build the arg buffer with `tools/upload_args.py <path> <local_file>`. |

`beacon.h` declares the Beacon API and the `datap` struct. Its layout **must**
match `crates/loader/src/beacon.rs` — Cobalt Strike 4.x: `original`+0, `buffer`+8,
`length`+16, `size`+20. If you change one, change the other.

## Arg formats

Two BOF arg conventions are in use:

- **Raw text** (`cmd_exec`, `ls`, `download`): the `args` buffer is the UTF-8
  bytes of the string with **no length prefix**. Type the text directly in the
  GUI's text-args field, or POST `{"args":"<base64 of utf8>"}`.
- **CS packed** (`upload`): the standard Cobalt Strike format —
  `[4-byte big-endian length][blob]` per `BeaconDataExtract`. Use
  `tools/upload_args.py` (or the GUI's file-args source) to build the buffer.

The GUI's `services/mockAdapter.ts` knows each BOF's `encode_type`
(`none` / `raw_string`) and encodes typed console args accordingly; the
`raw_string` encoder is just `Array.from(new TextEncoder().encode(arg))`.

## Build the BOFs (Windows, native)

Each BOF is a C file compiled to an x64 COFF object with the MinGW gcc
toolchain (e.g. MSYS2 / mingw-w64). `gcc` and `x86_64-w64-mingw32-gcc` both work:

```sh
gcc -c examples/hello.c -o examples/hello.x64.o
gcc -c examples/cmd_exec.c -o examples/cmd_exec.x64.o
gcc -c examples/ps.c    -o examples/ps.x64.o
gcc -c examples/ls.c    -o examples/ls.x64.o
gcc -c examples/download.c -o examples/download.x64.o
gcc -c examples/upload.c   -o examples/upload.x64.o
```

Do **not** pass `-g` unless you strip debug sections afterward — `.debug_*`
sections carry relocation types the loader skips (non-fatal, but noisy). The
loader is sensitive to the reloc type numbering emitted by this toolchain (see
"Toolchain note" in `CLAUDE.md`); use the same `x86_64-w64-mingw32-gcc` that
produced the tested objects.

`.x64.o` files are gitignored (`*.o`). To stage them in the Go core's BOF
library (so the GUI can list/run them by name), copy into
`clients/go-server/bofs/`:

```sh
cp examples/*.x64.o clients/go-server/bofs/
```

## End-to-end test (single Windows host)

The Go core (`clients/go-server`) + Rust implant is the canonical E2E path
(the Wails GUI drives the same REST API):

```sh
# 1. Build everything (a rust-toolchain.toml pins stable MSVC)
cargo build --release
cd clients/go-server && go build -o go-server.exe .
# 2. Build + stage the BOFs (above)
# 3. Start the core (implant TCP :4444, operator HTTP :8091)
RUSTSTRIKE_BOFS=./bofs ./go-server.exe 4444 8091
# 4. Run the implant (reverse-connects to the core)
./target/release/ruststrike-implant.exe 127.0.0.1 4444
# 5. Drive via REST (returns a task_id; poll /api/tasks/{id} for output):
curl -s http://127.0.0.1:8091/api/implants
curl -X POST "http://127.0.0.1:8091/api/bofs/ps/run?implant=1" -H 'Content-Type: application/json' -d '{"args":""}'
curl -X POST "http://127.0.0.1:8091/api/bofs/cmd_exec/run?implant=1" -H 'Content-Type: application/json' \
  -d "{\"args\":\"$(printf 'whoami' | base64 -w0)\"}"
# 6. Or launch the GUI against the core:
cd clients/wails-gui && wails dev   # desktop console in real mode
```

The reference Rust server (`crates/server`) also works for a quick single-implant
smoke: `./target/release/ruststrike-server.exe 4444`, then `hello` /
`load examples/hello.x64.o` / `loadb examples/cmd_exec.x64.o examples/cmd_args.bin`
at its console.

## v1 limitations

- `BeaconPrintf` only captures the first two variadic arguments (x64 r8/r9).
  Format strings with more substitutions emit the extras literally. Use
  `BeaconOutput(type, data, len)` (binary-safe, no format) for large/structured
  output, as `ps`/`ls`/`download` do.
- Only the data/output Beacon APIs and basic CRT are stubbed; other `Beacon*`
  calls resolve to a no-op stub that records a note.
- x64 only; x86 BOFs are not supported.
- `download` is capped at 2 MB so the base64 fits the core's 4 MB line buffer.
