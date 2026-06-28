package main

// GET /api/logs?limit=500&implant=<id> -> persisted log history from SQLite.
//
// Replaces the GUI's client-side-only ring buffer as the source of truth:
// history now survives a core restart. The GUI's live event stream still
// appends to its in-memory tail for real-time updates.

import (
	"net/http"
	"strconv"
)

func logsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	implantID, _ := strconv.ParseUint(r.URL.Query().Get("implant"), 10, 64)
	if store == nil {
		writeJSON(w, http.StatusOK, []LogEntry{})
		return
	}
	entries, err := store.Logs(implantID, limit)
	if err != nil {
		http.Error(w, "logs: "+err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, entries)
}
