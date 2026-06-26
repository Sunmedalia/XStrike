//! Pure COFF (x64 .obj) parsing. No platform dependencies — testable on any OS.
//!
//! Layout references: PE/COFF spec. Record sizes on disk:
//!  - IMAGE_FILE_HEADER: 20 bytes
//!  - IMAGE_SECTION_HEADER: 40 bytes
//!  - IMAGE_SYMBOL: 18 bytes (aux records are 18 bytes each, counted in indices)
//!  - IMAGE_RELOCATION: 10 bytes

use anyhow::{bail, ensure, Result};

pub const IMAGE_FILE_MACHINE_AMD64: u16 = 0x8664;

/// AMD64 relocation types (PE spec).
pub const REL_AMD64_ABSOLUTE: u16 = 0;
pub const REL_AMD64_ADDR64: u16 = 1;
pub const REL_AMD64_ADDR32NB: u16 = 2;
pub const REL_AMD64_REL32: u16 = 3;
/// REL32_1 .. REL32_5 add 1..5 extra bytes to the fixup displacement.
pub const REL_AMD64_REL32_1: u16 = 4;
pub const REL_AMD64_REL32_5: u16 = 8;
pub const REL_AMD64_SECTION: u16 = 9;
pub const REL_AMD64_SECREL: u16 = 10;
pub const REL_AMD64_SECREL7: u16 = 11;

pub const IMAGE_SYM_UNDEFINED: i16 = 0;
pub const IMAGE_SYM_ABSOLUTE: i16 = -1;
pub const IMAGE_SYM_DEBUG: i16 = -2;

#[derive(Debug, Clone)]
pub struct Section {
    pub name: String,
    pub size_of_raw_data: u32,
    pub pointer_to_raw_data: u32,
    pub pointer_to_relocations: u32,
    pub number_of_relocations: u16,
    pub characteristics: u32,
    /// Assigned offset of this section inside the loaded image buffer.
    pub image_offset: usize,
}

#[derive(Debug, Clone)]
pub struct Relocation {
    pub virtual_address: u32,
    pub symbol_table_index: u32,
    pub typ: u16,
    /// Which section this relocation belongs to (index into `Coff::sections`).
    pub section_index: usize,
}

#[derive(Debug, Clone)]
pub struct Symbol {
    pub name: String,
    pub value: i32,
    pub section_number: i16,
    pub typ: u16,
    pub storage_class: u8,
    pub number_of_aux_symbols: u8,
}

#[derive(Debug, Clone)]
pub struct Coff {
    pub machine: u16,
    pub sections: Vec<Section>,
    pub symbols: Vec<Symbol>,
    pub relocations: Vec<Relocation>,
}

fn read_u16(b: &[u8], off: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(b.get(off..off + 2).ok_or_else(|| anyhow::anyhow!("short u16"))?.try_into()?))
}
fn read_i16(b: &[u8], off: usize) -> Result<i16> {
    Ok(i16::from_le_bytes(b.get(off..off + 2).ok_or_else(|| anyhow::anyhow!("short i16"))?.try_into()?))
}
fn read_u32(b: &[u8], off: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(b.get(off..off + 4).ok_or_else(|| anyhow::anyhow!("short u32"))?.try_into()?))
}
fn read_i32(b: &[u8], off: usize) -> Result<i32> {
    Ok(i32::from_le_bytes(b.get(off..off + 4).ok_or_else(|| anyhow::anyhow!("short i32"))?.try_into()?))
}

/// Decode a COFF 8-byte name field. Two encodings exist for names that don't
/// fit inline:
///   * MS-style: begins with `/<decimal>/`; the decimal is an offset into the
///     string table *data* (i.e. after the 4-byte size word).
///   * GNU/GAS-style (mingw): byte 0 is `0x00` and bytes [4..8] hold a little
///     endian offset measured from the *start* of the string table (including
///     the 4-byte size word). We detect this when byte 0 is NUL and the offset
///     lands on a valid string.
fn decode_name(field: &[u8], string_table: &[u8]) -> Result<String> {
    if field.len() >= 4 && field[0] == b'/' && (field[1] as char).is_ascii_digit() {
        let digits: String = field[1..]
            .iter()
            .take_while(|c| c.is_ascii_digit())
            .map(|c| *c as char)
            .collect();
        let off: usize = digits.parse().map_err(|_| anyhow::anyhow!("bad name offset"))?;
        let bytes = string_table
            .get(off..)
            .ok_or_else(|| anyhow::anyhow!("name offset past string table"))?;
        let end = bytes.iter().position(|c| *c == 0).unwrap_or(bytes.len());
        return Ok(String::from_utf8_lossy(&bytes[..end]).into_owned());
    }
    // GNU/GAS-style string table reference.
    if field.len() >= 8 && field[0] == 0 {
        let off = u32::from_le_bytes(field[4..8].try_into().unwrap()) as usize;
        if off >= 4 {
            // `off` is measured from the table start (incl. size word); the
            // 4-byte size word occupies [0..4), so data begins at offset 4.
            let bytes = string_table
                .get(off..)
                .ok_or_else(|| anyhow::anyhow!("gnu name offset past string table"))?;
            let end = bytes.iter().position(|c| *c == 0).unwrap_or(bytes.len());
            if end > 0 {
                return Ok(String::from_utf8_lossy(&bytes[..end]).into_owned());
            }
        }
    }
    let end = field.iter().position(|c| *c == 0).unwrap_or(field.len());
    Ok(String::from_utf8_lossy(&field[..end]).into_owned())
}

pub fn parse(bytes: &[u8]) -> Result<Coff> {
    if bytes.len() < 20 {
        bail!("file too small to be a COFF object");
    }
    let machine = read_u16(bytes, 0)?;
    ensure!(machine == IMAGE_FILE_MACHINE_AMD64, "not an AMD64 COFF (machine=0x{machine:04x})");

    let num_sections = read_u16(bytes, 2)?;
    let ptr_symbols = read_u32(bytes, 8)? as usize;
    let num_symbols = read_u32(bytes, 12)? as usize;
    let size_opt_header = read_u16(bytes, 16)? as usize;

    let sec_table_off = 20 + size_opt_header;

    // String table starts right after the symbol table.
    let string_table_off = ptr_symbols.checked_add(18 * num_symbols)
        .ok_or_else(|| anyhow::anyhow!("symbol table overflow"))?;
    let string_table = bytes.get(string_table_off..).unwrap_or(&[]);

    // Sections.
    let mut sections = Vec::with_capacity(num_sections as usize);
    let mut image_offset = 0usize;
    for i in 0..num_sections as usize {
        let off = sec_table_off + i * 40;
        let name = decode_name(&bytes[off..off + 8], string_table)?;
        let size_of_raw_data = read_u32(bytes, off + 16)?;
        let pointer_to_raw_data = read_u32(bytes, off + 20)?;
        let pointer_to_relocations = read_u32(bytes, off + 24)?;
        let number_of_relocations = read_u16(bytes, off + 32)?;
        let characteristics = read_u32(bytes, off + 36)?;
        // Align each section to 16 bytes (matches typical allocation granularity).
        let aligned = (size_of_raw_data as usize + 15) & !15;
        sections.push(Section {
            name,
            size_of_raw_data,
            pointer_to_raw_data,
            pointer_to_relocations,
            number_of_relocations,
            characteristics,
            image_offset,
        });
        image_offset += aligned.max(16);
    }

    // Symbols (skip aux records but keep their slots so indices line up).
    let mut symbols = Vec::with_capacity(num_symbols);
    let mut i = 0;
    while i < num_symbols {
        let off = ptr_symbols + i * 18;
        let name = decode_name(&bytes[off..off + 8], string_table)?;
        let value = read_i32(bytes, off + 8)?;
        let section_number = read_i16(bytes, off + 12)?;
        let typ = read_u16(bytes, off + 14)?;
        let storage_class = bytes[off + 16];
        let number_of_aux_symbols = bytes[off + 17];
        symbols.push(Symbol {
            name,
            value,
            section_number,
            typ,
            storage_class,
            number_of_aux_symbols,
        });
        i += 1 + number_of_aux_symbols as usize;
    }

    // Relocations (10-byte records per section).
    let mut relocations = Vec::new();
    for (si, sec) in sections.iter().enumerate() {
        for r in 0..sec.number_of_relocations as usize {
            let off = sec.pointer_to_relocations as usize + r * 10;
            let virtual_address = read_u32(bytes, off)?;
            let symbol_table_index = read_u32(bytes, off + 4)?;
            let typ = read_u16(bytes, off + 8)?;
            relocations.push(Relocation {
                virtual_address,
                symbol_table_index,
                typ,
                section_index: si,
            });
        }
    }

    Ok(Coff { machine, sections, symbols, relocations })
}

impl Coff {
    /// Find a defined symbol by name (e.g. `go`). Returns its section index and
    /// in-section value, or None.
    pub fn find_defined(&self, name: &str) -> Option<(usize, i32)> {
        for s in &self.symbols {
            if s.name == name && s.section_number >= 1 {
                return Some((s.section_number as usize - 1, s.value));
            }
        }
        None
    }

    /// Names of all external (undefined) symbols — the ones a loader must supply.
    pub fn external_names(&self) -> Vec<&str> {
        self.symbols
            .iter()
            .filter(|s| s.section_number == IMAGE_SYM_UNDEFINED && !s.name.is_empty())
            .map(|s| s.name.as_str())
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    /// Requires examples/hello.x64.o (built by the verify step). Skips if absent.
    fn load_example() -> Option<Vec<u8>> {
        let p = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../examples/hello.x64.o");
        std::fs::read(&p).ok()
    }

    #[test]
    fn parses_example_bof() {
        let bytes = match load_example() {
            Some(b) => b,
            None => {
                eprintln!("skipping: examples/hello.x64.o not built yet");
                return;
            }
        };
        let coff = parse(&bytes).expect("parse");
        assert_eq!(coff.machine, IMAGE_FILE_MACHINE_AMD64);
        assert!(coff.find_defined("go").is_some(), "go entry must be present");
        // hello.c calls BeaconPrintf -> an external undefined symbol.
        let exts = coff.external_names();
        assert!(
            exts.iter().any(|n| n.contains("BeaconPrintf")),
            "expected BeaconPrintf external, got {exts:?}"
        );
        assert!(!coff.sections.is_empty(), "must have at least one section");
    }
}
