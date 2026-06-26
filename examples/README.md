# Example BOF

`hello.c` is the v1 test BOF. It calls `BeaconPrintf(CALLBACK_OUTPUT, "hello from bof")`
and exposes the standard `go(char* args, int alen)` entry point.

`beacon.h` declares the Beacon API and the `datap` struct. Its layout **must**
match `crates/loader/src/beacon.rs` (original/size/length/buffer at offsets
0/8/12/16). If you change one, change the other.

## Build (Linux cross-compile)

```sh
x86_64-w64-mingw32-gcc -c examples/hello.c -o examples/hello.x64.o
```

This produces an x64 COFF object (`hello.x64.o`) that the loader can parse and
execute.

## End-to-end test (requires a Windows host for the implant)

1. Build the server (Linux): `cargo build -p ruststrike-server`
2. Cross-build the implant: `cargo build -p ruststrike-implant --target x86_64-pc-windows-gnu`
3. Build the example BOF (above).
4. Run the server: `./target/debug/ruststrike-server 4444`
5. On the Windows host, run the implant pointing at the server:
   `ruststrike-implant.exe <server-ip> 4444`
6. In the server console:
   - type `hello` → expect `[implant] hello: hello from implant`
   - type `load examples/hello.x64.o` → expect `[implant] output: hello from bof`

## v1 limitations

- `BeaconPrintf` only captures the first two variadic arguments (x64 r8/r9).
  Format strings with more substitutions emit the extras literally.
- Only the data/output Beacon APIs are stubbed; other `Beacon*` calls resolve to
  a no-op stub that records a note.
- x64 only; x86 BOFs are not supported.
