# Example BOF

`hello.c` is the v1 test BOF. It calls `BeaconPrintf(CALLBACK_OUTPUT, "hello from bof")`
and exposes the standard `go(char* args, int alen)` entry point.

`beacon.h` declares the Beacon API and the `datap` struct. Its layout **must**
match `crates/loader/src/beacon.rs` (original/size/length/buffer at offsets
0/8/12/16). If you change one, change the other.

## Build the BOF (Windows, native)

The BOF is a C file compiled to an x64 COFF object. Build it with the MinGW
gcc toolchain (e.g. via MSYS2 / mingw-w64). `gcc` and `x86_64-w64-mingw32-gcc`
both work on a Windows host:

```sh
gcc -c examples/hello.c -o examples/hello.x64.o
# or
x86_64-w64-mingw32-gcc -c examples/hello.c -o examples/hello.x64.o
```

Do **not** pass `-g` unless you strip debug sections afterward — `.debug_*`
sections carry relocation types the loader skips (non-fatal, but noisy). The
loader is sensitive to the reloc type numbering emitted by this toolchain (see
"Toolchain note" in `CLAUDE.md`); use the same `x86_64-w64-mingw32-gcc` that
produced the tested object.

This produces an x64 COFF object (`hello.x64.o`, gitignored as `*.o`) that the
loader can parse and execute.

## End-to-end test (single Windows host)

1. Build everything natively (a `rust-toolchain.toml` pins stable MSVC):
   `cargo build --release`
2. Build the example BOF (above).
3. Run the server: `./target/release/ruststrike-server.exe 4444`
4. In another terminal, run the implant pointing back at the server:
   `./target/release/ruststrike-implant.exe 127.0.0.1 4444`
5. In the server console:
   - type `hello` → expect `[implant] hello: hello from implant`
   - type `load examples/hello.x64.o` → expect `[implant] output: hello from bof`

## v1 limitations

- `BeaconPrintf` only captures the first two variadic arguments (x64 r8/r9).
  Format strings with more substitutions emit the extras literally.
- Only the data/output Beacon APIs are stubbed; other `Beacon*` calls resolve to
  a no-op stub that records a note.
- x64 only; x86 BOFs are not supported.
