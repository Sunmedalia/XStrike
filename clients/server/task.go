package main

import (
	"fmt"
	"sync"
	"time"
)

// TaskStore correlates BOF executions to the output/error they produce.
//
// RustStrike BOFs are synchronous and one-shot: the implant runs the BOF to
// completion, then sends exactly one `output` (or `error`) message back. So
// when the operator runs a BOF on implant X (Create), the NEXT output/error
// event for implant X completes that task (Feed). The GUI polls Get() to pull
// the result — this bridges RustStrike's fire-and-forget event model onto the
// request/response task-polling model the frontend expects.
//
// Concurrency: one active task per implant at a time (Create supersedes the
// previous active task for that implant). Adequate for v1 operator-driven use.

type TaskStatus string

const (
	TaskRunning   TaskStatus = "running"
	TaskCompleted TaskStatus = "completed"
	TaskFailed    TaskStatus = "failed"
)

type Task struct {
	ID        string     `json:"id"`
	ImplantID uint64     `json:"implant_id"`
	BofName   string     `json:"bof_name,omitempty"` // which BOF this task runs (for artifact capture)
	Status    TaskStatus `json:"status"`
	Output    string     `json:"output"`
	Created   time.Time  `json:"created"`
	Updated   time.Time  `json:"updated"`
}

type TaskStore struct {
	mu     sync.Mutex
	tasks  map[string]*Task
	nextID uint64
	active map[uint64]string // implantID -> latest running task id
}

func NewTaskStore() *TaskStore {
	return &TaskStore{tasks: map[string]*Task{}, active: map[uint64]string{}}
}

// Create registers a new running task for an implant and returns its id. It
// supersedes any prior active task for that implant.
func (ts *TaskStore) Create(implantID uint64) string {
	return ts.CreateNamed(implantID, "")
}

// CreateNamed is Create with the BOF name recorded for artifact capture.
func (ts *TaskStore) CreateNamed(implantID uint64, bofName string) string {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	ts.nextID++
	id := fmt.Sprintf("t-%d", ts.nextID)
	now := time.Now()
	ts.tasks[id] = &Task{ID: id, ImplantID: implantID, BofName: bofName, Status: TaskRunning, Created: now, Updated: now}
	ts.active[implantID] = id
	return id
}

// Feed appends an output/error chunk from an implant to its active task and
// completes it. Called from the session readLoop when an output/error event
// arrives. kind is the wire type: "output" or "error". Returns the completed
// task's BOF name (if any) so the caller can capture an artifact.
func (ts *TaskStore) Feed(implantID uint64, kind, data string) (bofName string) {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	id, ok := ts.active[implantID]
	if !ok {
		return ""
	}
	t, ok := ts.tasks[id]
	if !ok {
		delete(ts.active, implantID)
		return ""
	}
	if t.Output == "" {
		t.Output = data
	} else {
		t.Output += "\n" + data
	}
	t.Updated = time.Now()
	switch kind {
	case "error":
		t.Status = TaskFailed
	case "output":
		t.Status = TaskCompleted
	}
	// one-shot: clear active so a subsequent stray event doesn't append.
	delete(ts.active, implantID)
	return t.BofName
}

// Get returns a copy of a task by id.
func (ts *TaskStore) Get(id string) (Task, bool) {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	t, ok := ts.tasks[id]
	if !ok {
		return Task{}, false
	}
	return *t, true
}

// reap removes tasks older than maxAge to bound memory.
func (ts *TaskStore) reap(maxAge time.Duration) {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	cutoff := time.Now().Add(-maxAge)
	for id, t := range ts.tasks {
		if t.Updated.Before(cutoff) {
			delete(ts.tasks, id)
		}
	}
}

// StartReaper periodically trims old tasks.
func (ts *TaskStore) StartReaper() {
	go func() {
		t := time.NewTicker(2 * time.Minute)
		defer t.Stop()
		for range t.C {
			ts.reap(10 * time.Minute)
		}
	}()
}
