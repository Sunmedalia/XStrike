package main

// REST handlers for runtime listener management.
//
//   GET    /api/listeners                list current listeners
//   POST   /api/listeners                body {name,bind_ip,port,protocol} -> {id}
//   PUT    /api/listeners/{id}           body {name,bind_ip,port} -> update
//   POST   /api/listeners/{id}/start     restart a stopped listener
//   POST   /api/listeners/{id}/stop      stop a running listener
//   DELETE /api/listeners/{id}           stop + remove

import (
	"encoding/json"
	"net/http"
	"strings"
)

func listenersHandler(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, http.StatusOK, listenerMgr.ListInfo())
	case http.MethodPost:
		var body struct {
			Name     string `json:"name"`
			BindIP   string `json:"bind_ip"`
			Port     string `json:"port"`
			Protocol string `json:"protocol"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
			return
		}
		// protocol is always tcp for v1 (the only managed transport); accept
		// http/https/ws from the UI but coerce to tcp since only implant TCP
		// listeners are managed here.
		l, err := listenerMgr.Create(body.Name, body.BindIP, body.Port)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"id": l.ID, "status": "listening"})
	default:
		http.Error(w, "GET/POST only", http.StatusMethodNotAllowed)
	}
}

// /api/listeners/{id} with sub-actions: PUT (update), DELETE, /start, /stop.
func listenerHandler(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/api/listeners/")
	parts := strings.SplitN(rest, "/", 2)
	if len(parts) == 0 || parts[0] == "" {
		http.NotFound(w, r)
		return
	}
	id := parts[0]
	action := ""
	if len(parts) == 2 {
		action = parts[1]
	}

	switch {
	case action == "" && r.Method == http.MethodPut:
		var body struct {
			Name   string `json:"name"`
			BindIP string `json:"bind_ip"`
			Port   string `json:"port"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
			return
		}
		l, err := listenerMgr.Update(id, body.Name, body.BindIP, body.Port)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"id": l.ID, "status": "updated"})
	case action == "" && r.Method == http.MethodDelete:
		if err := listenerMgr.Delete(id); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"id": id, "status": "deleted"})
	case action == "start" && r.Method == http.MethodPost:
		if err := listenerMgr.Start(id); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"id": id, "status": "listening"})
	case action == "stop" && r.Method == http.MethodPost:
		if err := listenerMgr.Stop(id); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"id": id, "status": "stopped"})
	default:
		http.Error(w, "unsupported", http.StatusMethodNotAllowed)
	}
}
