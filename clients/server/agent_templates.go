package main

// Agent templates: the Generate Agent dialog reads these so the agent-type
// dropdown is data-driven instead of hardcoded. Each template lives in
// agents/templates/<id>.toml and declares the base exe, the stub variant, the
// GUI options it supports (silent / sleep / dwell), and default cadence
// values. The stub builder's `template` request field resolves to a variant
// via these templates (see stub_patcher.go::resolveVariant).
//
// Template file format (a deliberately tiny subset of TOML — string fields +
// one string-array field; parsed by parseTemplateTOML below, no external dep):
//
//	id           = "ruststrike-beacon-cycle"
//	name         = "RustStrike Beacon (Short-Cycle)"
//	description  = "..."
//	base         = "ruststrike-beacon-cycle.exe"
//	variant      = "beacon-cycle"
//	supports     = ["silent", "sleep", "dwell"]
//	default_sleep = 5
//	default_dwell = 2

import (
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// AgentTemplate is one entry in the Generate Agent dropdown.
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

// templatesEnv is the env var that overrides the templates directory.
const templatesEnv = "RUSTSTRIKE_TEMPLATES"

// agentTemplates is the loaded set, keyed by template ID. Populated by
// loadAgentTemplates at startup; safe to read after init.
var agentTemplates = map[string]AgentTemplate{}

// loadAgentTemplates scans the templates directory (env RUSTSTRIKE_TEMPLATES,
// else <repo>/agents/templates) for *.toml and parses each into the
// agentTemplates map. Missing dir or parse errors are logged but never fatal —
// the stub builder still works with explicit beacon/cycle booleans when no
// templates are present.
func loadAgentTemplates() {
	dir := strings.TrimSpace(os.Getenv(templatesEnv))
	if dir == "" {
		dir = filepath.Join(repoRoot(), "agents", "templates")
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		// No templates dir is non-fatal: the stub builder falls back to the
		// beacon/cycle booleans. Log once so the operator knows.
		fmt.Fprintf(os.Stderr, "agent templates: %s: %v (falling back to boolean variant flags)\n", dir, err)
		return
	}
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(strings.ToLower(e.Name()), ".toml") {
			continue
		}
		raw, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			fmt.Fprintf(os.Stderr, "agent templates: read %s: %v\n", e.Name(), err)
			continue
		}
		t, err := parseTemplateTOML(string(raw))
		if err != nil {
			fmt.Fprintf(os.Stderr, "agent templates: parse %s: %v\n", e.Name(), err)
			continue
		}
		if t.ID == "" || t.Variant == "" || t.Base == "" {
			fmt.Fprintf(os.Stderr, "agent templates: %s: missing id/base/variant\n", e.Name())
			continue
		}
		// Reject an unknown variant rather than silently letting variantNames
		// fall through to the implant exe — a typo'd template would otherwise
		// build the wrong agent with no error.
		switch t.Variant {
		case variantImplant, variantBeacon, variantCycle:
		default:
			fmt.Fprintf(os.Stderr, "agent templates: %s: unknown variant %q (want implant|beacon|beacon-cycle)\n", e.Name(), t.Variant)
			continue
		}
		agentTemplates[t.ID] = t
	}
}

// agentTemplatesHandler — GET /api/agent/templates -> {success, data: [...]}.
// Returns the templates sorted by ID for a stable dropdown order.
func agentTemplatesHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	out := make([]AgentTemplate, 0, len(agentTemplates))
	for _, t := range agentTemplates {
		out = append(out, t)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	writeJSON(w, http.StatusOK, out)
}

// lookupTemplateVariant returns the stub variant for a template ID, or "" if
// the template is absent/empty (so the caller falls back to the beacon/cycle
// booleans). Used by stub_patcher.go::resolveVariant.
func lookupTemplateVariant(templateID string) string {
	id := strings.TrimSpace(templateID)
	if id == "" {
		return ""
	}
	t, ok := agentTemplates[id]
	if !ok {
		return ""
	}
	return t.Variant
}

// repoRoot derives the RustStrike repo root from the core exe location by
// walking up for a Cargo.toml marker, falling back to two dirs up (the core
// lives in clients/server). Shared with stub_patcher's repoRootFromBaseExe
// idea but anchored on the exe, not a base implant path.
func repoRoot() string {
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		for d := dir; d != "" && d != "." && d != filepath.Dir(d); d = filepath.Dir(d) {
			if _, err := os.Stat(filepath.Join(d, "Cargo.toml")); err == nil {
				return d
			}
		}
	}
	// fallback: <repo>/clients/server -> two dirs up
	return filepath.Join("..", "..")
}

// parseTemplateTOML parses the tiny TOML subset used by agent templates:
//   - `key = "quoted string"`
//   - `key = ["a", "b", "c"]`  (string array)
//   - `key = 123`              (integer)
//   - `# comment` lines and blank lines are skipped
//
// No external TOML dependency — the template schema is fixed and small, so a
// purpose-built reader keeps the core's dep set minimal (modernc.org/sqlite is
// the only external dep, by convention).
func parseTemplateTOML(raw string) (AgentTemplate, error) {
	var t AgentTemplate
	supports := map[string]bool{}
	for _, line := range strings.Split(raw, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			return t, fmt.Errorf("malformed line (no '='): %q", line)
		}
		key := strings.TrimSpace(line[:eq])
		val := strings.TrimSpace(line[eq+1:])
		switch key {
		case "id":
			t.ID = unquote(val)
		case "name":
			t.Name = unquote(val)
		case "description":
			t.Description = unquote(val)
		case "base":
			t.Base = unquote(val)
		case "variant":
			t.Variant = unquote(val)
		case "supports":
			for _, s := range parseStringArray(val) {
				supports[s] = true
			}
		case "default_sleep":
			t.DefaultSleep = parseInt(val)
		case "default_dwell":
			t.DefaultDwell = parseInt(val)
		default:
			// Unknown keys are ignored (forward-compatible), unlike the JSON
			// config which rejects them — templates are looser on purpose.
		}
	}
	// Materialize supports as a sorted slice for stable JSON output.
	for s := range supports {
		t.Supports = append(t.Supports, s)
	}
	sort.Strings(t.Supports)
	return t, nil
}

// unquote strips surrounding double quotes from a TOML string value.
func unquote(s string) string {
	if len(s) >= 2 && s[0] == '"' && s[len(s)-1] == '"' {
		return s[1 : len(s)-1]
	}
	return s
}

// parseStringArray parses a TOML inline array of quoted strings, e.g.
// `["silent", "sleep", "dwell"]`. Tolerates extra whitespace.
func parseStringArray(s string) []string {
	s = strings.TrimSpace(s)
	if !strings.HasPrefix(s, "[") || !strings.HasSuffix(s, "]") {
		return nil
	}
	inner := strings.TrimSpace(s[1 : len(s)-1])
	if inner == "" {
		return nil
	}
	var out []string
	for _, part := range strings.Split(inner, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		out = append(out, unquote(part))
	}
	return out
}

// parseInt parses a TOML integer, returning 0 on any error (defaults are
// optional fields).
func parseInt(s string) int {
	n, err := strconv.Atoi(strings.TrimSpace(s))
	if err != nil {
		return 0
	}
	return n
}
