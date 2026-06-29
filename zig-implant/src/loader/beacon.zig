//! Beacon API stubs. Mirrors `crates/loader/src/stubs.rs`.
//!
//! These are `extern "C"` functions whose addresses are patched into BOF
//! relocations for `Beacon*` symbols. Output is captured into a thread-local
//! buffer that `run_bof` returns. Windows-only (relies on the x64 MSVC ABI for
//! capturing the first two variadic args of BeaconPrintf via r8/r9).

const std = @import("std");

var gpa_holder: std.mem.Allocator = std.heap.page_allocator;

pub fn setAllocator(a: std.mem.Allocator) void {
    gpa_holder = a;
}

// Thread-local captured output. Lazily initialized; cleared per BOF run.
threadlocal var output: ?std.ArrayList(u8) = null;

fn out() *std.ArrayList(u8) {
    if (output == null) output = .empty;
    return &output.?;
}

pub fn resetOutput() void {
    if (output) |*o| o.clearRetainingCapacity();
}

pub fn takeOutput(alloc: std.mem.Allocator) ![]u8 {
    if (output) |*o| {
        const dup = try alloc.dupe(u8, o.items);
        o.clearRetainingCapacity();
        return dup;
    }
    return try alloc.dupe(u8, "");
}

fn append(s: []const u8) void {
    out().appendSlice(gpa_holder, s) catch {};
}

pub fn appendNote(s: []const u8) void {
    if (s.len == 0) return;
    append(s);
}

// ---- datap struct (Cobalt Strike 4.x beacon.h layout, x64) ----
//   char *original; // +0
//   char *buffer;   // +8
//   int   length;   // +16
//   int   size;     // +20
// total 24 bytes. MUST match the `datap` a BOF compiles against (CS 4.x).
const DATAP_ORIG: usize = 0;
const DATAP_BUF: usize = 8;
const DATAP_LEN: usize = 16;
const DATAP_SIZE: usize = 20;

fn writePtr(cell: [*]u8, off: usize, val: ?[*]const u8) void {
    const p: usize = if (val) |v| @intFromPtr(v) else 0;
    std.mem.writeInt(usize, cell[off..][0..@sizeOf(usize)], p, .little);
}
fn writeI32(cell: [*]u8, off: usize, val: i32) void {
    std.mem.writeInt(i32, cell[off..][0..4], val, .little);
}
fn readPtr(cell: [*]const u8, off: usize) ?[*]const u8 {
    const p = std.mem.readInt(usize, cell[off..][0..@sizeOf(usize)], .little);
    if (p == 0) return null;
    return @ptrFromInt(p);
}

/// void BeaconDataParse(datap *parser, char *buffer, int size)
pub export fn BeaconDataParse(parser: ?[*]u8, buffer: ?[*]const u8, size: i32) callconv(.c) void {
    const p = parser orelse return;
    writePtr(p, DATAP_ORIG, buffer);
    writePtr(p, DATAP_BUF, buffer);
    writeI32(p, DATAP_SIZE, size);
    writeI32(p, DATAP_LEN, size);
}

/// int BeaconDataInt(datap *parser) — 4-byte big-endian (CS convention).
pub export fn BeaconDataInt(parser: ?[*]u8) callconv(.c) i32 {
    const p = parser orelse return 0;
    const buf = readPtr(p, DATAP_BUF) orelse return 0;
    var v: i32 = 0;
    var i: usize = 0;
    while (i < 4) : (i += 1) v = (v << 8) | @as(i32, buf[i]);
    writePtr(p, DATAP_BUF, buf + 4);
    const len = std.mem.readInt(i32, p[DATAP_LEN..][0..4], .little);
    writeI32(p, DATAP_LEN, len -| 4);
    return v;
}

/// short BeaconDataShort(datap *parser) — 2-byte big-endian.
pub export fn BeaconDataShort(parser: ?[*]u8) callconv(.c) i16 {
    const p = parser orelse return 0;
    const buf = readPtr(p, DATAP_BUF) orelse return 0;
    const v: i16 = @truncate((@as(i32, buf[0]) << 8) | @as(i32, buf[1]));
    writePtr(p, DATAP_BUF, buf + 2);
    const len = std.mem.readInt(i32, p[DATAP_LEN..][0..4], .little);
    writeI32(p, DATAP_LEN, len -| 2);
    return v;
}

/// char *BeaconDataExtract(datap *parser, int *size) — 4-byte BE length-prefixed blob.
pub export fn BeaconDataExtract(parser: ?[*]u8, size: ?[*]i32) callconv(.c) ?[*]const u8 {
    const p = parser orelse return null;
    const len = BeaconDataInt(parser);
    const buf = readPtr(p, DATAP_BUF) orelse return null;
    if (size) |sz| sz[0] = len;
    const blob = buf;
    const adv: usize = @intCast(@max(len, 0));
    writePtr(p, DATAP_BUF, buf + adv);
    const rem = std.mem.readInt(i32, p[DATAP_LEN..][0..4], .little);
    writeI32(p, DATAP_LEN, rem -| @max(len, 0));
    return blob;
}

/// void BeaconOutput(int type, char *data, int len)
pub export fn BeaconOutput(_: i32, data: ?[*]const u8, len: i32) callconv(.c) void {
    const d = data orelse return;
    if (len <= 0) return;
    append(d[0..@intCast(len)]);
}

/// Generic unimplemented Beacon API stub — records a note and returns.
pub fn beaconUnimplemented() callconv(.c) void {
    appendNote("[rt] unimplemented API called");
}

/// BOOL BeaconIsAdmin() — v1 stub: report not admin.
pub export fn BeaconIsAdmin() callconv(.c) i32 {
    return 0;
}

/// void BeaconPrintf(int type, char *fmt, ...)
///
/// Captures the first two variadic args via a1/a2 (r8/r9 on x64 MSVC). Format
/// specifiers beyond the first two are emitted literally.
pub export fn BeaconPrintf(typ: i32, fmt: ?[*:0]const u8, a1: u64, a2: u64) callconv(.c) void {
    const f = fmt orelse {
        var tmp: [48]u8 = undefined;
        const s = std.fmt.bufPrint(&tmp, "[printf type={d} null fmt]", .{typ}) catch "[printf null fmt]";
        append(s);
        return;
    };
    const fmt_len = std.mem.indexOfSentinel(u8, 0, f);
    const formatted = formatBof(gpa_holder, f[0..fmt_len], &.{ a1, a2 }) catch return;
    defer gpa_holder.free(formatted);
    append(formatted);
}

fn appendFmt(acc: *std.ArrayList(u8), alloc: std.mem.Allocator, comptime fmt: []const u8, args: anytype) void {
    var buf: [32]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch return;
    acc.appendSlice(alloc, s) catch {};
}

/// Minimal printf-style interpreter over the first `args` 64-bit slots.
fn formatBof(alloc: std.mem.Allocator, fmt: []const u8, args: []const u64) ![]u8 {
    var acc: std.ArrayList(u8) = .empty;
    defer acc.deinit(alloc);
    const bytes = fmt;
    var i: usize = 0;
    var ai: usize = 0;
    while (i < bytes.len) {
        const c = bytes[i];
        if (c != '%') {
            try acc.append(alloc, c);
            i += 1;
            continue;
        }
        i += 1;
        if (i >= bytes.len) {
            try acc.append(alloc, '%');
            break;
        }
        // flags
        while (i < bytes.len and switch (bytes[i]) {
            '-', '+', ' ', '#', '0' => true,
            else => false,
        }) i += 1;
        // width
        if (i < bytes.len and bytes[i] == '*') {
            i += 1;
        } else {
            while (i < bytes.len and std.ascii.isDigit(bytes[i])) i += 1;
        }
        // precision
        if (i < bytes.len and bytes[i] == '.') {
            i += 1;
            if (i < bytes.len and bytes[i] == '*') i += 1 else while (i < bytes.len and std.ascii.isDigit(bytes[i])) i += 1;
        }
        // length modifiers
        var is_long = false;
        var is_size = false;
        while (i < bytes.len and switch (bytes[i]) {
            'l', 'L', 'h', 'z', 'j', 't' => true,
            else => false,
        }) : (i += 1) {
            if (bytes[i] == 'l' or bytes[i] == 'L') is_long = true;
            if (bytes[i] == 'z' or bytes[i] == 'j') is_size = true;
        }
        if (i >= bytes.len) {
            try acc.appendSlice(alloc, "%<truncated>");
            break;
        }
        const conv = bytes[i];
        i += 1;
        switch (conv) {
            '%' => try acc.append(alloc, '%'),
            'c' => {
                if (ai < args.len) {
                    try acc.append(alloc, @as(u8, @truncate(args[ai])));
                    ai += 1;
                }
            },
            's' => {
                if (ai < args.len) {
                    const p: ?[*:0]const u8 = @ptrFromInt(args[ai]);
                    ai += 1;
                    if (p) |pp| {
                        const sl = std.mem.indexOfSentinel(u8, 0, pp);
                        try acc.appendSlice(alloc, pp[0..sl]);
                    } else try acc.appendSlice(alloc, "(null)");
                }
            },
            'd', 'i' => {
                if (ai < args.len) {
                    const v: i64 = if (is_long or is_size) @bitCast(args[ai]) else @as(i32, @truncate(@as(i64, @bitCast(args[ai]))));
                    appendFmt(&acc, alloc, "{d}", .{v});
                    ai += 1;
                }
            },
            'u' => {
                if (ai < args.len) {
                    const v: u64 = if (is_long or is_size) args[ai] else @as(u32, @truncate(args[ai]));
                    appendFmt(&acc, alloc, "{d}", .{v});
                    ai += 1;
                }
            },
            'x' => {
                if (ai < args.len) {
                    appendFmt(&acc, alloc, "{x}", .{args[ai]});
                    ai += 1;
                }
            },
            'X' => {
                if (ai < args.len) {
                    appendFmt(&acc, alloc, "{X}", .{args[ai]});
                    ai += 1;
                }
            },
            'p' => {
                if (ai < args.len) {
                    appendFmt(&acc, alloc, "0x{x}", .{args[ai]});
                    ai += 1;
                }
            },
            else => {
                try acc.append(alloc, '%');
                try acc.append(alloc, conv);
            },
        }
    }
    return acc.toOwnedSlice(alloc);
}
