package main

import "testing"

func TestParseTemplateTOML(t *testing.T) {
	raw := `# a template
id = "ruststrike-beacon-cycle"
name = "RustStrike Beacon (Short-Cycle)"
description = "short-cycle beacon"
base = "ruststrike-beacon-cycle.exe"
variant = "beacon-cycle"
supports = ["silent", "sleep", "dwell"]
default_sleep = 5
default_dwell = 2
`
	tt, err := parseTemplateTOML(raw)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if tt.ID != "ruststrike-beacon-cycle" {
		t.Errorf("ID = %q", tt.ID)
	}
	if tt.Variant != "beacon-cycle" {
		t.Errorf("Variant = %q", tt.Variant)
	}
	if tt.Base != "ruststrike-beacon-cycle.exe" {
		t.Errorf("Base = %q", tt.Base)
	}
	if tt.DefaultSleep != 5 || tt.DefaultDwell != 2 {
		t.Errorf("sleep/dwell = %d/%d", tt.DefaultSleep, tt.DefaultDwell)
	}
	// supports materializes as a sorted slice.
	want := []string{"dwell", "silent", "sleep"}
	if len(tt.Supports) != len(want) {
		t.Fatalf("supports = %v", tt.Supports)
	}
	for i, s := range want {
		if tt.Supports[i] != s {
			t.Errorf("supports[%d] = %q, want %q", i, tt.Supports[i], s)
		}
	}
}

func TestParseTemplateTOMLMinimal(t *testing.T) {
	// Only the required fields; supports/default_* omitted.
	raw := `id = "x"
base = "x.exe"
variant = "implant"
`
	tt, err := parseTemplateTOML(raw)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if tt.ID != "x" || tt.Base != "x.exe" || tt.Variant != "implant" {
		t.Errorf("got %+v", tt)
	}
	if len(tt.Supports) != 0 {
		t.Errorf("supports should be empty, got %v", tt.Supports)
	}
}

func TestResolveVariant(t *testing.T) {
	// No template loaded for "nope" → falls back to booleans.
	if got := resolveVariant(false, false, ""); got != variantImplant {
		t.Errorf("default = %q, want implant", got)
	}
	if got := resolveVariant(false, true, ""); got != variantBeacon {
		t.Errorf("beacon = %q, want beacon", got)
	}
	if got := resolveVariant(true, true, ""); got != variantCycle {
		t.Errorf("cycle wins = %q, want beacon-cycle", got)
	}
	if got := resolveVariant(true, false, "nope"); got != variantCycle {
		t.Errorf("unknown template + cycle = %q, want beacon-cycle", got)
	}
}

func TestResolveVariantTemplateSemantics(t *testing.T) {
	// Seed the template registry with the three real variants, save/restore.
	prev := agentTemplates
	agentTemplates = map[string]AgentTemplate{
		"ruststrike-implant":       {ID: "ruststrike-implant", Variant: "implant"},
		"ruststrike-beacon":        {ID: "ruststrike-beacon", Variant: "beacon"},
		"ruststrike-beacon-cycle":  {ID: "ruststrike-beacon-cycle", Variant: "beacon-cycle"},
	}
	defer func() { agentTemplates = prev }()

	// Non-implant template is authoritative: the booleans can't override it
	// (the frontend doesn't send beacon/cycle for non-implant templates anyway).
	if got := resolveVariant(false, false, "ruststrike-beacon"); got != variantBeacon {
		t.Errorf("beacon template = %q, want beacon", got)
	}
	if got := resolveVariant(false, false, "ruststrike-beacon-cycle"); got != variantCycle {
		t.Errorf("cycle template = %q, want beacon-cycle", got)
	}
	// Implant template + beacon checkbox → beacon (booleans win for the implant
	// template, preserving the pre-template "beacon checkbox selects beacon exe"
	// behavior). This is the key regression guard.
	if got := resolveVariant(false, true, "ruststrike-implant"); got != variantBeacon {
		t.Errorf("implant template + beacon = %q, want beacon (beacon checkbox must still work)", got)
	}
	if got := resolveVariant(true, false, "ruststrike-implant"); got != variantCycle {
		t.Errorf("implant template + cycle = %q, want beacon-cycle", got)
	}
	// Implant template + no booleans → implant.
	if got := resolveVariant(false, false, "ruststrike-implant"); got != variantImplant {
		t.Errorf("implant template alone = %q, want implant", got)
	}
}

func TestTrailerCadence(t *testing.T) {
	// implant gets no cadence fields.
	if iv, dw := trailerCadence(variantImplant, "5", "3"); iv != "" || dw != "" {
		t.Errorf("implant cadence = %q/%q, want empty", iv, dw)
	}
	// beacon gets only interval.
	if iv, dw := trailerCadence(variantBeacon, "5", "3"); iv != "5" || dw != "" {
		t.Errorf("beacon cadence = %q/%q, want 5/empty", iv, dw)
	}
	// cycle gets interval + dwell, with dwell defaulting to "2" when empty.
	if iv, dw := trailerCadence(variantCycle, "5", ""); iv != "5" || dw != "2" {
		t.Errorf("cycle cadence empty dwell = %q/%q, want 5/2", iv, dw)
	}
	if iv, dw := trailerCadence(variantCycle, "5", "3"); iv != "5" || dw != "3" {
		t.Errorf("cycle cadence = %q/%q, want 5/3", iv, dw)
	}
}
