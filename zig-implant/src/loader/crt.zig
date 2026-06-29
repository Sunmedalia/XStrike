//! C runtime helpers implemented in Zig so BOFs don't pull in msvcrt.
//! Mirrors `crates/loader/src/exec.rs` rt_* functions. Addresses are registered
//! in the loader's external map (resolved by name, not linked).

const std = @import("std");

pub fn rtMemcpy(dst: ?[*]u8, src: ?[*]const u8, n: usize) callconv(.c) ?[*]u8 {
    if (dst == null or src == null) return dst;
    const d = dst.?;
    const s = src.?;
    var i: usize = 0;
    while (i < n) : (i += 1) d[i] = s[i];
    return d;
}

pub fn rtMemmove(dst: ?[*]u8, src: ?[*]const u8, n: usize) callconv(.c) ?[*]u8 {
    if (dst == null or src == null) return dst;
    const d = dst.?;
    const s = src.?;
    if (@intFromPtr(d) < @intFromPtr(s)) {
        var i: usize = 0;
        while (i < n) : (i += 1) d[i] = s[i];
    } else {
        var i: usize = n;
        while (i > 0) {
            i -= 1;
            d[i] = s[i];
        }
    }
    return d;
}

pub fn rtMemset(dst: ?[*]u8, val: i32, n: usize) callconv(.c) ?[*]u8 {
    const d = dst orelse return null;
    const v: u8 = @truncate(@as(u32, @bitCast(val)));
    var i: usize = 0;
    while (i < n) : (i += 1) d[i] = v;
    return d;
}

pub fn rtMemcmp(a: ?[*]const u8, b: ?[*]const u8, n: usize) callconv(.c) i32 {
    if (a == null or b == null) return 0;
    const aa = a.?;
    const bb = b.?;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        if (aa[i] != bb[i]) return @as(i32, aa[i]) - @as(i32, bb[i]);
    }
    return 0;
}

pub fn rtStrlen(s: ?[*]const u8) callconv(.c) usize {
    const p = s orelse return 0;
    var i: usize = 0;
    while (p[i] != 0) i += 1;
    return i;
}

/// `__chkstk` / `___chkstk_ms`: touch stack pages so the OS commits them.
///
/// x64 ABI: the requested allocation size arrives in rax; probe each 4 KiB page
/// between rsp and rsp - size so Windows grows the stack, then return with rax
/// PRESERVED (the caller does `sub rsp, rax` after the call). Returning the
/// wrong rax (e.g. 0) makes the caller skip its frame allocation and fault.
pub fn rtChkstk() callconv(.c) void {
    asm volatile (
        \\movq %%rax, %%r11
        \\movq %%rsp, %%r10
        \\2:
        \\subq $4096, %%r10
        \\testq %%r10, (%%r10)
        \\subq $4096, %%rax
        \\jg 2b
        \\movq %%r11, %%rax
        :
        :
        : .{ .rax = true, .r10 = true, .r11 = true, .memory = true }
    );
}
