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
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// globals wired in main(); read by handlers/goroutines.
var (
	manager     *SessionManager
	bus         *EventBus
	boflib      *BofLib
	tasks       *TaskStore
	store       *Store
	listenerMgr *ListenerManager
	logMu       sync.Mutex
	logOut      io.Writer = os.Stderr
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
	tasks = NewTaskStore()
	tasks.StartReaper()

	// SQLite store (logs + listener config + agents + artifacts). Pure-Go
	// modernc.org/sqlite — no cgo. Path via RUSTSTRIKE_DB env, else next to exe.
	dbPath := os.Getenv("RUSTSTRIKE_DB")
	st, err := NewStore(dbPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[core] store: %v\n", err)
		os.Exit(1)
	}
	store = st
	// Persist every bus event as a log row (non-blocking, best-effort).
	go func() {
		ch := bus.Subscribe()
		for ev := range ch {
			store.LogEnqueue(time.Now().Unix(), ev.ImplantID, ev.Type, ev.Data)
		}
	}()

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

	// Listener manager: owns runtime start/stop of TCP implant listeners. The
	// default boot listener is created through it so the whole TCP set is
	// uniform + persisted. Persisted listeners from a prior run are restored.
	listenerMgr = NewListenerManager(manager, store, bus)
	if rows, err := store.ListListeners(); err == nil {
		listenerMgr.Restore(rows)
	}
	// If no default listener exists yet, create the boot one.
	if !hasListenerOnPort(tcpPort) {
		if _, err := listenerMgr.Create("default", "0.0.0.0", tcpPort); err != nil {
			fmt.Fprintf(os.Stderr, "[core] tcp bind :%s: %v\n", tcpPort, err)
			os.Exit(1)
		}
	}
	fmt.Fprintf(os.Stderr, "[core] implant TCP listeners: %d\n", len(listenerMgr.ListInfo()))

	// HTTP/WS for operators.
	mux := http.NewServeMux()
	mountAPI(mux)
	fmt.Fprintf(os.Stderr, "[core] operator HTTP on 0.0.0.0:%s (try GET /api/implants, ws /ws)\n", httpPort)
	if err := http.ListenAndServe("0.0.0.0:"+httpPort, mux); err != nil {
		fmt.Fprintf(os.Stderr, "[core] http: %v\n", err)
		os.Exit(1)
	}
}

// hasListenerOnPort reports whether a listener on the given port already
// exists (restored from config), so we don't double-bind the boot port.
func hasListenerOnPort(port string) bool {
	for _, l := range listenerMgr.ListInfo() {
		if l.Port == port {
			return true
		}
	}
	return false
}
