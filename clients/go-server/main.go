// ruststrike-go-server: the operator-facing service core.
//
// Two listeners:
//   - TCP (default :4444): the implant transport — newline-delimited JSON,
//     same wire contract as crates/protocol. Each implant becomes a Session.
//   - HTTP (default :8080): REST + WebSocket for the GUI/operator.
//
// Usage:
//   go-server [tcp-port] [http-port]
//   go-server 4444 8080
//
// REST:
//   GET  /api/implants                      list sessions
//   POST /api/implants/{id}/hello           link check
//   POST /api/implants/{id}/bof             body {bof, args} (bof = lib name or b64)
//   DELETE /api/implants/{id}               drop session
//   GET  /api/bofs                          list BOF library
//   POST /api/bofs                          upload {name, file_b64}
//   POST /api/bofs/{name}/run?implant=<id>  body {args} (b64)
// WS:
//   /ws  -> live event stream (implant_connected/disconnected/hello/output/error)
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sync"
)

// globals wired in main(); read by handlers/goroutines.
var (
	manager *SessionManager
	bus     *EventBus
	boflib  *BofLib
	logMu   sync.Mutex
	logOut  io.Writer = os.Stderr
)

// logf returns a writer safe for concurrent logging lines.
func logf() io.Writer { return logOut }

func main() {
	tcpPort := "4444"
	httpPort := "8080"
	if len(os.Args) > 1 {
		tcpPort = os.Args[1]
	}
	if len(os.Args) > 2 {
		httpPort = os.Args[2]
	}

	bus = NewEventBus()
	manager = NewSessionManager(bus)
	// BOF library dir: RUSTSTRIKE_BOFS env var, else ./bofs next to the exe
	// (so it works regardless of the current working directory).
	bofDir := os.Getenv("RUSTSTRIKE_BOFS")
	if bofDir == "" {
		if exe, err := os.Executable(); err == nil {
			bofDir = filepath.Join(filepath.Dir(exe), "bofs")
		} else {
			bofDir = "bofs"
		}
	}
	boflib = NewBofLib(bofDir)
	fmt.Fprintf(os.Stderr, "[core] BOF library: %s\n", bofDir)

	// TCP listener for implants.
	ln, err := net.Listen("tcp", "0.0.0.0:"+tcpPort)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[core] tcp bind :%s: %v\n", tcpPort, err)
		os.Exit(1)
	}
	go acceptImplants(ln)
	fmt.Fprintf(os.Stderr, "[core] implant TCP on 0.0.0.0:%s\n", tcpPort)

	// HTTP/WS for operators.
	mux := http.NewServeMux()
	mountAPI(mux)
	fmt.Fprintf(os.Stderr, "[core] operator HTTP on 0.0.0.0:%s (try GET /api/implants, ws /ws)\n", httpPort)
	if err := http.ListenAndServe("0.0.0.0:"+httpPort, mux); err != nil {
		fmt.Fprintf(os.Stderr, "[core] http: %v\n", err)
		os.Exit(1)
	}
}

func acceptImplants(ln net.Listener) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			fmt.Fprintf(os.Stderr, "[core] accept: %v\n", err)
			return
		}
		manager.Accept(conn)
	}
}
