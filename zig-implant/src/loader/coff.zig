//! Pure COFF (x64 .obj) parsing. No platform deps. Mirrors
//! `crates/loader/src/obj.rs`.
//!
//! On-disk record sizes: IMAGE_FILE_HEADER 20B, SECTION_HEADER 40B,
//! IMAGE_SYMBOL 18B (aux records 18B each, counted in indices),
//! IMAGE_RELOCATION 10B.

const std = @import("std");

pub const IMAGE_FILE_MACHINE_AMD64: u16 = 0x8664;
pub const IMAGE_SYM_UNDEFINED: i16 = 0;
pub const IMAGE_SYM_ABSOLUTE: i16 = -1;
pub const IMAGE_SYM_DEBUG: i16 = -2;

pub const Section = struct {
    name: []const u8,
    size_of_raw_data: u32,
    pointer_to_raw_data: u32,
    pointer_to_relocations: u32,
    number_of_relocations: u16,
    characteristics: u32,
    /// Assigned offset of this section inside the loaded image buffer.
    image_offset: usize,
};

pub const Relocation = struct {
    virtual_address: u32,
    symbol_table_index: u32,
    typ: u16,
    /// Which section this relocation belongs to (index into Coff.sections).
    section_index: usize,
};

pub const Symbol = struct {
    name: []const u8,
    value: i32,
    section_number: i16,
    typ: u16,
    storage_class: u8,
    number_of_aux_symbols: u8,
    /// True for aux-record placeholder slots (must occupy an index so reloc
    /// symbol indices line up with the on-disk table, aux included).
    is_aux: bool,
};

pub const Coff = struct {
    machine: u16,
    sections: []Section,
    symbols: []Symbol,
    relocations: []Relocation,
    arena: *std.heap.ArenaAllocator,

    pub fn deinit(self: *Coff) void {
        self.arena.deinit();
        const gpa = self.arena.child_allocator;
        gpa.destroy(self.arena);
    }

    /// Find a defined symbol by name (e.g. `go`). Returns section index + value.
    pub fn findDefined(self: *const Coff, name: []const u8) ?struct { sec_idx: usize, value: i32 } {
        for (self.symbols) |s| {
            if (!s.is_aux and s.section_number >= 1 and std.mem.eql(u8, s.name, name)) {
                return .{ .sec_idx = @intCast(s.section_number - 1), .value = s.value };
            }
        }
        return null;
    }
};

fn readU16(b: []const u8, off: usize) !u16 {
    if (off + 2 > b.len) return error.ShortRead;
    return std.mem.readInt(u16, b[off..][0..2], .little);
}
fn readI16(b: []const u8, off: usize) !i16 {
    if (off + 2 > b.len) return error.ShortRead;
    return std.mem.readInt(i16, b[off..][0..2], .little);
}
fn readU32(b: []const u8, off: usize) !u32 {
    if (off + 4 > b.len) return error.ShortRead;
    return std.mem.readInt(u32, b[off..][0..4], .little);
}
fn readI32(b: []const u8, off: usize) !i32 {
    if (off + 4 > b.len) return error.ShortRead;
    return std.mem.readInt(i32, b[off..][0..4], .little);
}

/// Decode a COFF 8-byte name field. Two encodings for names that don't fit:
///   * MS-style: `/<decimal>/` -> offset into string table DATA (after the
///     4-byte size word).
///   * GNU/GAS-style (mingw): byte 0 is NUL, bytes [4..8] hold a LE offset
///     measured from the START of the string table (incl. the size word).
fn decodeName(field: []const u8, string_table: []const u8) ![]const u8 {
    if (field.len >= 4 and field[0] == '/' and std.ascii.isDigit(field[1])) {
        var off: usize = 0;
        var i: usize = 1;
        while (i < field.len and std.ascii.isDigit(field[i])) : (i += 1) {
            off = off * 10 + (field[i] - '0');
        }
        if (off >= string_table.len) return error.NameOffsetPastTable;
        const bytes = string_table[off..];
        const end = std.mem.indexOfScalar(u8, bytes, 0) orelse bytes.len;
        return bytes[0..end];
    }
    if (field.len >= 8 and field[0] == 0) {
        const off = std.mem.readInt(u32, field[4..8][0..4], .little);
        if (off >= 4 and off < string_table.len) {
            const bytes = string_table[off..];
            const end = std.mem.indexOfScalar(u8, bytes, 0) orelse bytes.len;
            if (end > 0) return bytes[0..end];
        }
    }
    const end = std.mem.indexOfScalar(u8, field, 0) orelse field.len;
    return field[0..end];
}

pub fn parse(alloc: std.mem.Allocator, bytes: []const u8) !Coff {
    if (bytes.len < 20) return error.TooSmall;
    const machine = try readU16(bytes, 0);
    if (machine != IMAGE_FILE_MACHINE_AMD64) return error.NotAmd64;

    const num_sections = try readU16(bytes, 2);
    const ptr_symbols = try readU32(bytes, 8);
    const num_symbols = try readU32(bytes, 12);
    const size_opt_header = try readU16(bytes, 16);
    const sec_table_off: usize = 20 + @as(usize, size_opt_header);

    const string_table_off = std.math.add(usize, ptr_symbols, 18 * @as(usize, num_symbols)) catch return error.SymbolOverflow;
    const string_table = if (string_table_off < bytes.len) bytes[string_table_off..] else &[_]u8{};

    const arena_ptr = try alloc.create(std.heap.ArenaAllocator);
    arena_ptr.* = std.heap.ArenaAllocator.init(alloc);
    errdefer {
        arena_ptr.deinit();
        alloc.destroy(arena_ptr);
    }
    const a = arena_ptr.allocator();

    // Sections.
    var sections = try a.alloc(Section, num_sections);
    var image_offset: usize = 0;
    var i: usize = 0;
    while (i < num_sections) : (i += 1) {
        const off = sec_table_off + i * 40;
        if (off + 40 > bytes.len) return error.SectionHeaderOOB;
        const name = try decodeName(bytes[off..][0..8], string_table);
        const size_of_raw_data = try readU32(bytes, off + 16);
        const pointer_to_raw_data = try readU32(bytes, off + 20);
        const pointer_to_relocations = try readU32(bytes, off + 24);
        const number_of_relocations = try readU16(bytes, off + 32);
        const characteristics = try readU32(bytes, off + 36);
        const aligned = (size_of_raw_data + 15) & ~@as(u32, 15);
        sections[i] = .{
            .name = name,
            .size_of_raw_data = size_of_raw_data,
            .pointer_to_raw_data = pointer_to_raw_data,
            .pointer_to_relocations = pointer_to_relocations,
            .number_of_relocations = number_of_relocations,
            .characteristics = characteristics,
            .image_offset = image_offset,
        };
        image_offset += @max(@as(usize, aligned), 16);
    }

    // Symbols (aux records occupy placeholder slots to keep indices aligned).
    var symbols = try a.alloc(Symbol, num_symbols);
    var si: usize = 0;
    var k: usize = 0;
    while (k < num_symbols) {
        const off = ptr_symbols + k * 18;
        if (off + 18 > bytes.len) return error.SymbolOOB;
        const name = try decodeName(bytes[off..][0..8], string_table);
        const value = try readI32(bytes, off + 8);
        const section_number = try readI16(bytes, off + 12);
        const typ = try readU16(bytes, off + 14);
        const storage_class = bytes[off + 16];
        const number_of_aux_symbols = bytes[off + 17];
        symbols[si] = .{
            .name = name,
            .value = value,
            .section_number = section_number,
            .typ = typ,
            .storage_class = storage_class,
            .number_of_aux_symbols = number_of_aux_symbols,
            .is_aux = false,
        };
        si += 1;
        k += 1;
        var aux: u8 = 0;
        while (aux < number_of_aux_symbols) : (aux += 1) {
            symbols[si] = .{
                .name = "",
                .value = 0,
                .section_number = 0,
                .typ = 0,
                .storage_class = 0,
                .number_of_aux_symbols = 0,
                .is_aux = true,
            };
            si += 1;
            k += 1;
        }
    }
    // `symbols` was allocated with num_symbols capacity, but aux expansion can't
    // exceed num_symbols (aux records ARE counted in num_symbols). si == num_symbols.

    // Relocations (10-byte records per section).
    var reloc_list: std.ArrayList(Relocation) = .empty;
    defer reloc_list.deinit(a);
    for (sections, 0..) |sec, sidx| {
        var r: usize = 0;
        while (r < sec.number_of_relocations) : (r += 1) {
            const off = @as(usize, sec.pointer_to_relocations) + r * 10;
            if (off + 10 > bytes.len) return error.RelocOOB;
            try reloc_list.append(a, .{
                .virtual_address = try readU32(bytes, off),
                .symbol_table_index = try readU32(bytes, off + 4),
                .typ = try readU16(bytes, off + 8),
                .section_index = sidx,
            });
        }
    }

    return Coff{
        .machine = machine,
        .sections = sections,
        .symbols = symbols,
        .relocations = try reloc_list.toOwnedSlice(a),
        .arena = arena_ptr,
    };
}
