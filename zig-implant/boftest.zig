//! Standalone BOF loader test harness (not built by build.zig). Reads a .o and
//! runs it via the same exec path as the implant, printing captured output.
//! Compile manually:
//!   zig build-exe boftest.zig -target x86_64-windows-msvc -ODebug
//! Usage: boftest.exe <bof.x64.o> [args-file]

const std = @import("std");
const exec = @import("src/loader/exec.zig");
const beacon = @import("src/loader/beacon.zig");
const winapi = @import("src/winapi.zig");

pub fn main() !void {
    const alloc = std.heap.smp_allocator;
    beacon.setAllocator(alloc);

    const cmdline = winapi.commandLine();
    var it = std.mem.tokenizeAny(u8, cmdline, " ");
    _ = it.next(); // exe
    const bof_path = it.next() orelse {
        std.debug.print("usage: boftest <bof.x64.o> [args-file]\n", .{});
        return;
    };

    var bof_z: [1024]u8 = undefined;
    const bof_z_path = std.fmt.bufPrintZ(&bof_z, "{s}", .{bof_path}) catch {
        std.debug.print("path too long\n", .{});
        return;
    };
    const coff_bytes = winapi.readFile(alloc, bof_z_path) orelse {
        std.debug.print("failed to read {s}\n", .{bof_path});
        return;
    };
    defer alloc.free(coff_bytes);

    var args: []const u8 = &[_]u8{};
    var owned_args: ?[]u8 = null;
    defer if (owned_args) |a| alloc.free(a);
    if (it.next()) |ap| {
        var ap_z: [1024]u8 = undefined;
        const ap_z_path = std.fmt.bufPrintZ(&ap_z, "{s}", .{ap}) catch return;
        owned_args = winapi.readFile(alloc, ap_z_path);
        if (owned_args) |a| args = a;
    }

    std.debug.print("loading {s} ({d} bytes)...\n", .{ bof_path, coff_bytes.len });
    const out = exec.runBof(alloc, coff_bytes, args) catch |e| {
        std.debug.print("runBof error: {s}\n", .{@errorName(e)});
        return;
    };
    defer alloc.free(out);
    std.debug.print("--- OUTPUT ({d} bytes) ---\n{s}\n--- END ---\n", .{ out.len, out });
}
