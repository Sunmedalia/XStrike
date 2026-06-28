package main

// Wire protocol types — MUST match crates/protocol/src/lib.rs exactly.
// serde: tag="type" rename_all="lowercase". The implant's Bof requires `args`
// (no #[serde(default)]), so Bof always carries Args (empty string ok).

type serverMsg struct {
	Type string `json:"type"`           // "hello" | "bof"
	File string `json:"file,omitempty"` // base64 COFF (bof only)
	Args string `json:"args"`           // base64 raw BOF arg buffer (bof only; always present)
}

type implantMsg struct {
	Type string `json:"type"` // "hello" | "output" | "error"
	Data string `json:"data"`
}
