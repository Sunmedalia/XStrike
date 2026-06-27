package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
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
