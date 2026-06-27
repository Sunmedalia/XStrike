//! Windows-only: allocate executable memory, lay out sections, resolve symbols,
//! apply relocations, and invoke the BOF `go` entry point.

#![cfg(windows)]

use crate::beacon;
use crate::coff::{self, Coff, Symbol, IMAGE_SYM_ABSOLUTE, IMAGE_SYM_DEBUG, IMAGE_SYM_UNDEFINED};
use anyhow::{bail, ensure, Context, Result};
use std::collections::HashMap;
use windows_sys::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryA};
use windows_sys::Win32::System::Memory::{
    VirtualAlloc, VirtualFree, MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_EXECUTE_READWRITE,
};

// ---- C runtime helpers implemented in Rust (so we don't depend on msvcrt) ----

pub extern "C" fn rt_memcpy(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    unsafe {
        if !dst.is_null() && !src.is_null() {
            std::ptr::copy_nonoverlapping(src, dst, n);
        }
        dst
    }
}
pub extern "C" fn rt_memmove(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    unsafe {
        if !dst.is_null() && !src.is_null() {
            std::ptr::copy(src, dst, n);
        }
        dst
    }
}
pub extern "C" fn rt_memset(dst: *mut u8, val: i32, n: usize) -> *mut u8 {
    unsafe {
        if !dst.is_null() {
            std::ptr::write_bytes(dst, val as u8, n);
        }
        dst
    }
}
pub extern "C" fn rt_memcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    unsafe {
        if a.is_null() || b.is_null() {
            return 0;
        }
        let sa = std::slice::from_raw_parts(a, n);
        let sb = std::slice::from_raw_parts(b, n);
        for i in 0..n {
            if sa[i] != sb[i] {
                return sa[i] as i32 - sb[i] as i32;
            }
        }
        0
    }
}
pub extern "C" fn rt_strlen(s: *const u8) -> usize {
    unsafe {
        if s.is_null() {
            return 0;
        }
        let mut i = 0;
        while *s.add(i) != 0 {
            i += 1;
        }
        i
    }
}
/// `__chkstk` / `___chkstk_ms` no-op: stack probing is unnecessary for our
/// small in-process frames; treat as a no-op return.
pub extern "C" fn rt_chkstk() {}

/// Generic unimplemented Beacon API stub — records a note and returns 0.
pub extern "C" fn beacon_unimplemented() {
    beacon::append_external_note("[ruststrike] unimplemented Beacon API called");
}

fn align16(n: usize) -> usize {
    (n + 15) & !15
}

/// Thunk slot size (bytes). A thunk is `FF 25 00 00 00 00` (`jmp [rip+0]`)
/// followed by the 8-byte absolute target — 14 bytes, padded to 16.
const THUNK_SIZE: usize = 16;

/// Load + execute a BOF. Returns the text captured from Beacon* output.
pub fn run_bof(coff_bytes: &[u8], args: &str) -> Result<String> {
    let parsed = coff::parse(coff_bytes).context("parsing COFF")?;
    beacon::reset_output();

    let sections_size = total_image_size(&parsed);
    ensure!(sections_size > 0, "BOF has no loadable sections");

    // Resolve every external (undefined) symbol to its real address up front,
    // and lay out one in-image thunk per external. The thunk lets a BOF's
    // 32-bit-relative `call`/`jmp`/`lea` reach an external even when it lands
    // far away (VirtualAlloc routinely returns memory > 2 GB from our code,
    // which a REL32 displacement cannot span). The thunk sits inside the
    // allocated image, so the REL32 reaches it; the thunk then jumps to the
    // real (64-bit absolute) target.
    let externals = build_external_map();
    let ext_slots = resolve_external_slots(&parsed, &externals);
    let thunk_count = ext_slots.len();
    let thunk_region = align16(thunk_count * THUNK_SIZE);
    let alloc_size = sections_size.checked_add(thunk_region)
        .ok_or_else(|| anyhow::anyhow!("image + thunks size overflow"))?;
    let thunk_base_off = sections_size;

    unsafe {
        // Executable + writable buffer for the whole image + thunks.
        let base = VirtualAlloc(std::ptr::null(), alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if base.is_null() {
            bail!("VirtualAlloc failed for image (size={alloc_size})");
        }
        let base = base as *mut u8;
        let base_usize = base as usize;

        // (best-effort cleanup on error; on success we keep the image for the
        // process lifetime — v1 does not free executable BOF memory.)
        let cleanup = || { let _ = VirtualFree(base as *mut _, 0, MEM_RELEASE); };

        // Write thunks: sym_idx -> thunk VA. Built before relocation so the
        // target addresses are available to the REL32 fixups.
        let mut thunk_va: HashMap<u32, usize> = HashMap::with_capacity(thunk_count);
        for (i, &(sym_idx, real)) in ext_slots.iter().enumerate() {
            let off = thunk_base_off + i * THUNK_SIZE;
            thunk_va.insert(sym_idx, base_usize + off);
            write_thunk(base.add(off), real);
        }

        if let Err(e) = load_into(&parsed, coff_bytes, base, base_usize, &thunk_va) {
            cleanup();
            return Err(e);
        }

        let go = match parsed.find_defined("go") {
            Some((sec_idx, value)) => base_usize + parsed.sections[sec_idx].image_offset + value as usize,
            None => {
                cleanup();
                bail!("BOF has no `go` entry point");
            }
        };

        // Prepare args buffer in writable memory.
        let args_bytes = args.as_bytes();
        let mut args_buf = vec![0u8; args_bytes.len().max(1)];
        if !args_bytes.is_empty() {
            args_buf[..args_bytes.len()].copy_from_slice(args_bytes);
        }
        let args_ptr = args_buf.as_ptr();
        let args_len = args_bytes.len() as i32;

        let go_fn: extern "C" fn(*const u8, i32) = std::mem::transmute(go);

        // Capture panics? BOFs are raw code; a fault here is a hard crash by
        // design. We call directly.
        go_fn(args_ptr, args_len);

        let output = beacon::take_output();
        // Keep `args_buf` (and image) alive until after the call — it is, since
        // we return now. The executable image is intentionally not freed.
        drop(args_buf);
        Ok(output)
    }
}

/// Write a thunk at `dst`: `jmp [rip+0]` followed by the 8-byte absolute
/// `target`. `dst` must point to at least 14 writable bytes.
unsafe fn write_thunk(dst: *mut u8, target: usize) {
    // FF 25 00 00 00 00 = jmp qword ptr [rip+0]; the 8-byte target follows.
    std::ptr::copy_nonoverlapping([0xFFu8, 0x25, 0, 0, 0, 0].as_ptr(), dst, 6);
    std::ptr::copy_nonoverlapping(
        (target as u64).to_le_bytes().as_ptr(),
        dst.add(6),
        8,
    );
}

/// Collect `(symbol_table_index, real_address)` for every resolvable external
/// (undefined, named) symbol, in symbol order. Each gets a thunk slot.
fn resolve_external_slots(
    parsed: &Coff,
    externals: &HashMap<String, usize>,
) -> Vec<(u32, usize)> {
    let mut slots = Vec::new();
    for (idx, sym) in parsed.symbols.iter().enumerate() {
        if sym.is_aux || sym.section_number != IMAGE_SYM_UNDEFINED || sym.name.is_empty() {
            continue;
        }
        // base_usize is irrelevant for externals (they don't reference the image).
        if let Ok(addr) = resolve_symbol(sym, parsed, 0, externals) {
            if addr != 0 {
                slots.push((idx as u32, addr));
            }
        }
    }
    slots
}

fn total_image_size(parsed: &Coff) -> usize {
    let mut total = 0usize;
    for s in &parsed.sections {
        let sz = align16(s.size_of_raw_data as usize).max(16);
        total = s.image_offset + sz;
    }
    total
}

unsafe fn load_into(
    parsed: &Coff,
    raw: &[u8],
    base: *mut u8,
    base_usize: usize,
    thunk_va: &HashMap<u32, usize>,
) -> Result<()> {
    // 1. Copy each section's raw data into the image.
    for sec in &parsed.sections {
        let dst = base.add(sec.image_offset);
        if sec.size_of_raw_data == 0 || sec.pointer_to_raw_data == 0 {
            continue; // BSS / uninitialized
        }
        let src_off = sec.pointer_to_raw_data as usize;
        let n = sec.size_of_raw_data as usize;
        let src = raw.get(src_off..src_off + n)
            .ok_or_else(|| anyhow::anyhow!("section {} raw data out of range", sec.name))?;
        std::ptr::copy_nonoverlapping(src.as_ptr(), dst, n);
    }

    // 2. Build the external symbol -> address map.
    let externals = build_external_map();

    // 3. Apply relocations.
    let img = std::slice::from_raw_parts_mut(base, total_image_size(parsed));
    for rel in &parsed.relocations {
        let sec = &parsed.sections[rel.section_index];
        let fixup_off = sec.image_offset + rel.virtual_address as usize;
        let fixup_va = base_usize + fixup_off;

        let sym: &Symbol = parsed.symbols.get(rel.symbol_table_index as usize)
            .ok_or_else(|| anyhow::anyhow!("reloc references bad symbol index {}", rel.symbol_table_index))?;
        let target = resolve_symbol(sym, parsed, base_usize, &externals)
            .with_context(|| format!("resolving symbol `{}`", sym.name))?;

        apply_relocation(img, fixup_off, fixup_va, target, base_usize, rel.typ, rel.symbol_table_index, sym, thunk_va)?;
    }
    Ok(())
}

fn resolve_symbol(
    sym: &Symbol,
    parsed: &Coff,
    base_usize: usize,
    externals: &HashMap<String, usize>,
) -> Result<usize> {
    match sym.section_number {
        s if s >= 1 => {
            let sec = &parsed.sections[(s - 1) as usize];
            Ok(base_usize + sec.image_offset + sym.value as usize)
        }
        IMAGE_SYM_UNDEFINED => {
            if sym.name.is_empty() {
                return Ok(0);
            }
            if let Some(addr) = externals.get(sym.name.as_str()) {
                return Ok(*addr);
            }
            // Try LIBRARY$function form and plain-name DLL lookup.
            if let Some(addr) = resolve_external_by_name(&sym.name) {
                return Ok(addr);
            }
            bail!("unresolved external symbol: {}", sym.name);
        }
        IMAGE_SYM_ABSOLUTE => Ok(sym.value as usize),
        IMAGE_SYM_DEBUG => Ok(0),
        other => bail!("unsupported section number {other} for symbol {}", sym.name),
    }
}

/// Build the map of loader-provided symbol names -> function addresses.
fn build_external_map() -> HashMap<String, usize> {
    let mut m = HashMap::new();
    // Beacon API stubs. Cast each fn pointer to its address (usize) — NOT the
    // address of a local holding the pointer (that would dangle after return).
    add_fn(&mut m, "BeaconDataParse", beacon::beacon_data_parse as unsafe extern "C" fn(*mut u8, *const u8, i32) as usize);
    add_fn(&mut m, "BeaconDataInt", beacon::beacon_data_int as unsafe extern "C" fn(*mut u8) -> i32 as usize);
    add_fn(&mut m, "BeaconDataShort", beacon::beacon_data_short as unsafe extern "C" fn(*mut u8) -> i16 as usize);
    add_fn(&mut m, "BeaconDataExtract", beacon::beacon_data_extract as unsafe extern "C" fn(*mut u8, *mut i32) -> *const u8 as usize);
    add_fn(&mut m, "BeaconOutput", beacon::beacon_output as unsafe extern "C" fn(i32, *const u8, i32) as usize);
    add_fn(&mut m, "BeaconPrintf", beacon::beacon_printf as unsafe extern "C" fn(i32, *const u8, u64, u64) as usize);
    add_fn(&mut m, "BeaconIsAdmin", beacon::beacon_is_admin as unsafe extern "C" fn() -> i32 as usize);
    // C runtime helpers.
    add_fn(&mut m, "memcpy", rt_memcpy as unsafe extern "C" fn(*mut u8, *const u8, usize) -> *mut u8 as usize);
    add_fn(&mut m, "memmove", rt_memmove as unsafe extern "C" fn(*mut u8, *const u8, usize) -> *mut u8 as usize);
    add_fn(&mut m, "memset", rt_memset as unsafe extern "C" fn(*mut u8, i32, usize) -> *mut u8 as usize);
    add_fn(&mut m, "memcmp", rt_memcmp as unsafe extern "C" fn(*const u8, *const u8, usize) -> i32 as usize);
    add_fn(&mut m, "strlen", rt_strlen as unsafe extern "C" fn(*const u8) -> usize as usize);
    add_fn(&mut m, "__chkstk", rt_chkstk as unsafe extern "C" fn() as usize);
    add_fn(&mut m, "___chkstk_ms", rt_chkstk as unsafe extern "C" fn() as usize);
    add_fn(&mut m, "_chkstk", rt_chkstk as unsafe extern "C" fn() as usize);
    m
}

fn add_fn(m: &mut HashMap<String, usize>, name: &str, addr: usize) {
    m.insert(name.to_string(), addr);
}

/// Resolve a `LIBRARY$function` symbol, or a plain function name by scanning
/// common DLLs. Returns the function address on success.
fn resolve_external_by_name(name: &str) -> Option<usize> {
    if let Some((lib, func)) = name.split_once('$') {
        return unsafe { proc_addr(lib, func) };
    }
    // Common DLLs to scan for plain-name imports.
    for lib in ["kernel32.dll", "ntdll.dll", "user32.dll", "advapi32.dll", "ws2_32.dll", "msvcrt.dll"] {
        if let Some(a) = unsafe { proc_addr(lib, name) } {
            return Some(a);
        }
    }
    // Any other `Beacon*` symbol we haven't stubbed -> no-op stub (records a note).
    if name.starts_with("Beacon") {
        return Some(beacon_unimplemented as *const () as usize);
    }
    None
}

unsafe fn proc_addr(lib: &str, func: &str) -> Option<usize> {
    let lib_c = std::ffi::CString::new(lib).ok()?;
    let h = LoadLibraryA(lib_c.as_ptr() as *const u8);
    if h.is_null() {
        return None;
    }
    let func_c = std::ffi::CString::new(func).ok()?;
    let p = GetProcAddress(h, func_c.as_ptr() as *const u8);
    if p.is_none() {
        // Try with a leading underscore (some imports are decorated).
        if !func.starts_with('_') {
            let decorated = format!("_{func}");
            if let Ok(c) = std::ffi::CString::new(decorated) {
                let p2 = GetProcAddress(h, c.as_ptr() as *const u8);
                if let Some(addr) = p2 {
                    return Some(addr as usize);
                }
            }
        }
        return None;
    }
    Some(p.unwrap() as usize)
}

fn read_i32_at(img: &[u8], off: usize) -> i32 {
    i32::from_le_bytes(img[off..off + 4].try_into().unwrap())
}
fn read_i64_at(img: &[u8], off: usize) -> i64 {
    i64::from_le_bytes(img[off..off + 8].try_into().unwrap())
}
fn write_u32(img: &mut [u8], off: usize, v: u32) {
    img[off..off + 4].copy_from_slice(&v.to_le_bytes());
}
fn write_u64(img: &mut [u8], off: usize, v: u64) {
    img[off..off + 8].copy_from_slice(&v.to_le_bytes());
}
fn write_i32(img: &mut [u8], off: usize, v: i32) {
    img[off..off + 4].copy_from_slice(&v.to_le_bytes());
}
fn write_u16(img: &mut [u8], off: usize, v: u16) {
    img[off..off + 2].copy_from_slice(&v.to_le_bytes());
}

#[allow(clippy::too_many_arguments)]
fn apply_relocation(
    img: &mut [u8],
    fixup_off: usize,
    fixup_va: usize,
    target: usize,
    base_usize: usize,
    typ: u16,
    sym_idx: u32,
    sym: &Symbol,
    thunk_va: &HashMap<u32, usize>,
) -> Result<()> {
    // Relocation type values. NOTE: the mingw/binutils 2.46 toolchain used to
    // build BOFs (x86_64-w64-mingw32-gcc) numbers the AMD64 ADDR32NB/REL32
    // family one HIGHER than Microsoft's winnt.h: it emits 3 for ADDR32NB and
    // 4 for REL32 (winnt.h: 2 and 3). This was confirmed by linking a BOF with
    // the system linker and reading back the resolved displacements — a type-4
    // fixup resolves as REL32 (reference = fixup_va + 4), type-3 as ADDR32NB
    // (RVA = target - image_base). We therefore match the toolchain's numbering
    // rather than winnt.h. We also accept the standard values (2=ADDR32NB,
    // 9/10/11=SECTION/SECREL/SECREL7) so MSVC-built objects still behave.
    match typ {
        0 => Ok(()), // ABSOLUTE (no-op)
        1 => {
            // ADDR64: 64-bit absolute.
            let addend = read_i64_at(img, fixup_off);
            let v = (target as i64).wrapping_add(addend) as u64;
            write_u64(img, fixup_off, v);
            Ok(())
        }
        2 | 3 => {
            // ADDR32NB: 32-bit RVA (target - image base).
            let addend = read_i32_at(img, fixup_off);
            let rva = (target as i64 - base_usize as i64 + addend as i64) as u32;
            write_u32(img, fixup_off, rva);
            Ok(())
        }
        4..=9 => {
            // REL32 family. Toolchain: 4=REL32, 5..9=REL32_1..REL32_5, i.e. the
            // number of trailing bytes after the 4-byte field is (typ - 4).
            // For an external (thunked) symbol, target the in-image thunk so the
            // 32-bit displacement can reach it.
            let tgt = thunk_va.get(&sym_idx).copied().unwrap_or(target);
            let n = (typ - 4) as i64; // 0..5 extra bytes
            let addend = read_i32_at(img, fixup_off) as i64;
            let v = (tgt as i64)
                .wrapping_sub(fixup_va as i64 + 4 + n)
                .wrapping_add(addend) as i32;
            write_i32(img, fixup_off, v);
            Ok(())
        }
        10 => {
            // SECTION: 16-bit section index (toolchain 10).
            let s = sym.section_number;
            if s < 1 {
                bail!("SECTION relocation on non-section symbol {}", sym.name);
            }
            write_u16(img, fixup_off, s as u16);
            Ok(())
        }
        11 => {
            // SECREL: 32-bit offset from the section start (toolchain 11).
            write_u32(img, fixup_off, sym.value as u32);
            Ok(())
        }
        12 => {
            // SECREL7: 7-bit section-relative offset (toolchain 12).
            let cur = img[fixup_off];
            let val = (sym.value as u8) & 0x7f;
            img[fixup_off] = (cur & 0x80) | val;
            Ok(())
        }
        other => {
            // Unknown type (e.g. in a .debug$ section we don't execute). Don't
            // fail the whole load over a reloc in non-executable data.
            beacon::append_external_note(&format!(
                "[ruststrike] skipped unsupported AMD64 relocation type {other} (symbol {})",
                sym.name
            ));
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    fn load_example() -> Option<Vec<u8>> {
        let p = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../examples/hello.x64.o");
        std::fs::read(&p).ok()
    }

    /// Actually loads + executes the example BOF in-process. Requires the
    /// .o to be built (mingw gcc). A hard fault here crashes the test process
    /// (by design — BOFs are raw code); a clean return proves the exec path.
    #[test]
    fn runs_example_bof() {
        let bytes = match load_example() {
            Some(b) => b,
            None => {
                eprintln!("skipping: examples/hello.x64.o not built");
                return;
            }
        };
        let out = run_bof(&bytes, "").expect("run_bof should succeed");
        assert!(out.contains("hello from bof"), "got: {out:?}");
    }
}
