package main

// Wire protocol types — MUST match crates/protocol/src/lib.rs exactly.
// serde: tag="type" rename_all="snake_case" (identical to lowercase for the
// single-word variants; relay variants use relay_listen/relay_started/etc.).
// The implant's Bof requires `args` (no #[serde(default)]), so Bof always
// carries Args (empty string ok).

type serverMsg struct {
	Type string `json:"type"`           // "hello" | "bof" | "relay_listen" | "relay_stop"
	File string `json:"file,omitempty"` // base64 COFF (bof only)
	Args string `json:"args"`           // base64 raw BOF arg buffer (bof only; always present)
	// Relay (relay_listen / relay_stop). RelayID is core-assigned so the async
	// relay_started/stopped/error reply can be correlated. Port has no omitempty
	// so relay_listen with port 0 (OS-assigned) is actually sent on the wire —
	// the implant's RelayListen treats a missing/zero port as "auto".
	RelayID string `json:"relay_id,omitempty"`
	BindIP  string `json:"bind_ip,omitempty"`
	Port    uint16 `json:"port"`
}

type implantMsg struct {
	Type string `json:"type"` // "hello" | "output" | "error" | "relay_started" | "relay_stopped" | "relay_error"
	Data string `json:"data"` // payload (output text / error msg / relay_error msg)
	// Relay (relay_started / relay_stopped / relay_error).
	RelayID string `json:"relay_id,omitempty"`
	BindIP  string `json:"bind_ip,omitempty"`
	Port    uint16 `json:"port,omitempty"`
}
