//! ruststrike-protocol: server <-> implant message definitions.
//!
//! Wire format: newline-delimited JSON. Binary BOF payloads are base64-encoded
//! inside the JSON. All messages share a `type` discriminator via the
//! `ServerMessage` / `ImplantMessage` enums.

use serde::{Deserialize, Serialize};

/// Messages flowing server -> implant.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum ServerMessage {
    /// Operator typed `hello` — link check. Implant echoes back.
    Hello,
    /// Load & execute a BOF. `file` is the COFF bytes and `args` is the raw BOF
    /// argument buffer — both base64-encoded (args is binary, not text: the
    /// CS/AdaptixC2 packed format walked by BeaconDataParse/Extract/Int).
    Bof { file: String, args: String },
}

/// Messages flowing implant -> server.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum ImplantMessage {
    /// Reply to a `Hello` from the operator.
    Hello { data: String },
    /// Successful BOF (or command) output.
    Output { data: String },
    /// Error surfaced while handling a request.
    Error { data: String },
}

impl ServerMessage {
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).expect("server message always serializes")
    }
}

impl ImplantMessage {
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).expect("implant message always serializes")
    }
}

/// Decode a BOF file payload (base64) -> raw COFF bytes.
pub fn decode_bof(b64: &str) -> anyhow::Result<Vec<u8>> {
    use base64::Engine;
    Ok(base64::engine::general_purpose::STANDARD.decode(b64)?)
}

/// Encode raw COFF bytes -> base64 string for transport.
pub fn encode_bof(bytes: &[u8]) -> String {
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.encode(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_server_messages() {
        let hello = ServerMessage::Hello;
        let s = hello.to_json();
        assert!(s.contains("\"type\":\"hello\""));

        let bof = ServerMessage::Bof { file: "AAAA".into(), args: "x".into() };
        let s = bof.to_json();
        let back: ServerMessage = serde_json::from_str(&s).unwrap();
        match back {
            ServerMessage::Bof { args, .. } => assert_eq!(args, "x"),
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn roundtrip_implant_messages() {
        let out = ImplantMessage::Output { data: "hello from bof".into() };
        let s = out.to_json();
        let back: ImplantMessage = serde_json::from_str(&s).unwrap();
        match back {
            ImplantMessage::Output { data } => assert_eq!(data, "hello from bof"),
            _ => panic!("wrong variant"),
        }
    }

    #[test]
    fn bof_encoding_roundtrips() {
        let raw = vec![0u8, 1, 2, 255, 128];
        let enc = encode_bof(&raw);
        let dec = decode_bof(&enc).unwrap();
        assert_eq!(dec, raw);
    }
}
