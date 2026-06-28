package main

import (
	"encoding/base64"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// BofLib indexes .x64.o files in a directory so the GUI can list BOFs and run
// them by name. Resolve() also accepts a raw base64 COFF string (passthrough).
type BofLib struct {
	dir string
	mu  sync.RWMutex
	// name (without .x64.o) -> absolute path
	files map[string]string
}

func NewBofLib(dir string) *BofLib {
	bl := &BofLib{dir: dir, files: make(map[string]string)}
	bl.Scan()
	return bl
}

// Scan (re)reads the directory. Safe to call anytime to refresh.
func (bl *BofLib) Scan() {
	bl.mu.Lock()
	defer bl.mu.Unlock()
	bl.scanLocked()
}

// scanLocked is the directory re-read, assuming the write lock is held.
func (bl *BofLib) scanLocked() {
	bl.files = make(map[string]string)
	if entries, err := os.ReadDir(bl.dir); err == nil {
		for _, e := range entries {
			if e.IsDir() {
				continue
			}
			name := e.Name()
			if strings.HasSuffix(name, ".x64.o") {
				base := strings.TrimSuffix(name, ".x64.o")
				bl.files[base] = filepath.Join(bl.dir, name)
			}
		}
	}
}

// Refresh re-reads the directory so newly staged .x64.o files appear without a
// core restart. Called from the list/run API handlers — a one-shot readdir is
// cheap. If the dir is unchanged this is a no-op rebuild of the same map.
func (bl *BofLib) Refresh() {
	bl.mu.Lock()
	defer bl.mu.Unlock()
	bl.scanLocked()
}

type BofEntry struct {
	Name string `json:"name"`
	Size int64  `json:"size"`
}

// List returns indexed BOFs with file sizes.
func (bl *BofLib) List() []BofEntry {
	bl.mu.RLock()
	defer bl.mu.RUnlock()
	out := make([]BofEntry, 0, len(bl.files))
	for name, path := range bl.files {
		sz := int64(0)
		if fi, err := os.Stat(path); err == nil {
			sz = fi.Size()
		}
		out = append(out, BofEntry{Name: name, Size: sz})
	}
	return out
}

// Resolve turns a "bof" request value into a base64 COFF string. If it looks
// like a library name (no base64 payload marker / decodable), load from disk;
// otherwise treat the value itself as base64 COFF bytes.
func (bl *BofLib) Resolve(v string) (string, error) {
	if v == "" {
		return "", errors.New("empty bof reference")
	}
	// Library name lookup first.
	bl.mu.RLock()
	path, ok := bl.files[v]
	bl.mu.RUnlock()
	if !ok {
		// Not in the in-memory map — re-scan the dir in case a new .x64.o
		// was staged after startup (no core restart needed).
		bl.Refresh()
		bl.mu.RLock()
		path, ok = bl.files[v]
		bl.mu.RUnlock()
	}
	if ok {
		raw, err := os.ReadFile(path)
		if err != nil {
			return "", err
		}
		return base64.StdEncoding.EncodeToString(raw), nil
	}
	// Otherwise: is `v` valid base64 that decodes to plausible COFF bytes?
	if dec, err := base64.StdEncoding.DecodeString(v); err == nil && len(dec) > 0 {
		return v, nil
	}
	return "", errors.New("unknown BOF name (not in library) and not base64: " + v)
}

// Add stores an uploaded BOF (raw bytes) into the library under `name`.
func (bl *BofLib) Add(name string, raw []byte) (string, error) {
	if name == "" {
		return "", errors.New("empty name")
	}
	if err := os.MkdirAll(bl.dir, 0o755); err != nil {
		return "", err
	}
	path := filepath.Join(bl.dir, name+".x64.o")
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		return "", err
	}
	bl.mu.Lock()
	bl.files[name] = path
	bl.mu.Unlock()
	return name, nil
}
