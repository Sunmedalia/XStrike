//! Beacon API stubs. These are `extern "C"` functions whose addresses are
//! patched into BOF relocations for `Beacon*` symbols. Output is captured into
//! a thread-local buffer that `run_bof` returns.
//!
//! Windows-only (relies on the MSVC x64 ABI for the calling convention and for
//! capturing the first two variadic arguments of `BeaconPrintf` via r8/r9).

#![cfg(windows)]

use std::cell::RefCell;

thread_local! {
    pub static OUTPUT: RefCell<String> = RefCell::new(String::new());
}

pub fn reset_output() {
    OUTPUT.with(|c| c.borrow_mut().clear());
}

pub fn take_output() -> String {
    OUTPUT.with(|c| c.borrow_mut().clone())
}

/// Append a captured chunk (BeaconOutput / BeaconPrintf).
fn append(s: &str) {
    OUTPUT.with(|c| c.borrow_mut().push_str(s));
}

/// Append a loader-side note (e.g. unimplemented Beacon API usage).
pub fn append_external_note(s: &str) {
    if s.is_empty() {
        return;
    }
    OUTPUT.with(|c| c.borrow_mut().push_str(s));
}

// ---- datap struct (Cobalt Strike 4.x beacon.h layout, x64) ----
//   char *original;  // +0   the original buffer [so we can free it]
//   char *buffer;    // +8   current pointer into our buffer
//   int   length;    // +16  remaining length of data
//   int   size;      // +20  total size of this buffer
// total 24 bytes. This MUST match the `datap` a BOF compiles against (CS 4.x
// / AdaptixC2 Extension-Kit). The earlier RustStrike-only layout
// (original/size/length/buffer) was non-standard and broke real BOFs.

const DATAP_ORIG: usize = 0;
const DATAP_BUF: usize = 8;
const DATAP_LEN: usize = 16;
const DATAP_SIZE: usize = 20;

unsafe fn write_ptr(cell: *mut u8, off: usize, val: *const u8) {
    (cell.add(off) as *mut *const u8).write_unaligned(val);
}
unsafe fn write_i32(cell: *mut u8, off: usize, val: i32) {
    (cell.add(off) as *mut i32).write_unaligned(val);
}
unsafe fn read_ptr(cell: *const u8, off: usize) -> *const u8 {
    (cell.add(off) as *const *const u8).read_unaligned()
}
unsafe fn read_i32(cell: *const u8, off: usize) -> i32 {
    (cell.add(off) as *const i32).read_unaligned()
}

/// `void BeaconDataParse(datap *parser, char *buffer, int size)`
pub extern "C" fn beacon_data_parse(parser: *mut u8, buffer: *const u8, size: i32) {
    if parser.is_null() {
        return;
    }
    unsafe {
        write_ptr(parser, DATAP_ORIG, buffer);
        write_ptr(parser, DATAP_BUF, buffer);
        write_i32(parser, DATAP_SIZE, size);
        write_i32(parser, DATAP_LEN, size);
    }
}

/// `int BeaconDataInt(datap *parser)` — 4-byte big-endian (CS convention).
pub extern "C" fn beacon_data_int(parser: *mut u8) -> i32 {
    if parser.is_null() {
        return 0;
    }
    unsafe {
        let buf = read_ptr(parser, DATAP_BUF);
        if buf.is_null() {
            return 0;
        }
        let mut v: i32 = 0;
        for i in 0..4 {
            v = (v << 8) | *buf.add(i) as i32;
        }
        write_ptr(parser, DATAP_BUF, buf.add(4));
        let len = read_i32(parser, DATAP_LEN).saturating_sub(4);
        write_i32(parser, DATAP_LEN, len);
        v
    }
}

/// `short BeaconDataShort(datap *parser)` — 2-byte big-endian.
pub extern "C" fn beacon_data_short(parser: *mut u8) -> i16 {
    if parser.is_null() {
        return 0;
    }
    unsafe {
        let buf = read_ptr(parser, DATAP_BUF);
        if buf.is_null() {
            return 0;
        }
        let v: i16 = (((*buf as i32) << 8) | *buf.add(1) as i32) as i16;
        write_ptr(parser, DATAP_BUF, buf.add(2));
        let len = read_i32(parser, DATAP_LEN).saturating_sub(2);
        write_i32(parser, DATAP_LEN, len);
        v
    }
}

/// `char *BeaconDataExtract(datap *parser, int *size)` — 4-byte BE length-prefixed blob.
pub extern "C" fn beacon_data_extract(parser: *mut u8, size: *mut i32) -> *const u8 {
    if parser.is_null() {
        return std::ptr::null();
    }
    unsafe {
        let len = beacon_data_int(parser);
        let buf = read_ptr(parser, DATAP_BUF);
        if !size.is_null() {
            *size = len;
        }
        let blob = buf;
        write_ptr(parser, DATAP_BUF, buf.add(len.max(0) as usize));
        let remaining = read_i32(parser, DATAP_LEN).saturating_sub(len.max(0));
        write_i32(parser, DATAP_LEN, remaining);
        blob
    }
}

/// `void BeaconOutput(int type, char *data, int len)`
pub extern "C" fn beacon_output(_type: i32, data: *const u8, len: i32) {
    if data.is_null() || len <= 0 {
        return;
    }
    unsafe {
        let slice = std::slice::from_raw_parts(data, len as usize);
        append(&String::from_utf8_lossy(slice));
    }
}

/// `BOOL BeaconIsAdmin()` — v1 stub: report not admin.
pub extern "C" fn beacon_is_admin() -> i32 {
    0
}

/// `void BeaconPrintf(int type, char *fmt, ...)`
///
/// v1 captures the first two variadic args via r8/a1 and r9/a2 (the third and
/// fourth integer argument registers on the x64 ABI). Format specifiers beyond
/// the first two are emitted literally. Sufficient for typical BOFs using
/// `CALLBACK_OUTPUT` with a few `%s`/`%d` substitutions.
pub extern "C" fn beacon_printf(typ: i32, fmt: *const u8, a1: u64, a2: u64) {
    if fmt.is_null() {
        append(&format!("[BeaconPrintf type={typ} null fmt]"));
        return;
    }
    unsafe {
        let cstr = std::ffi::CStr::from_ptr(fmt as *const i8);
        let fmt = cstr.to_string_lossy().into_owned();
        let formatted = format_bof(&fmt, &[a1, a2]);
        append(&formatted);
    }
}

/// Minimal printf-style interpreter over the first `args` 64-bit slots.
fn format_bof(fmt: &str, args: &[u64]) -> String {
    let mut out = String::new();
    let bytes = fmt.as_bytes();
    let mut i = 0;
    let mut ai = 0;
    while i < bytes.len() {
        let c = bytes[i];
        if c != b'%' {
            out.push(c as char);
            i += 1;
            continue;
        }
        // parse %[flags][width][length]conv
        i += 1;
        if i >= bytes.len() {
            out.push('%');
            break;
        }
        // flags
        while i < bytes.len() && matches!(bytes[i], b'-' | b'+' | b' ' | b'#' | b'0') {
            i += 1;
        }
        // width (digits, or *)
        if i < bytes.len() && bytes[i] == b'*' {
            i += 1;
        } else {
            while i < bytes.len() && bytes[i].is_ascii_digit() {
                i += 1;
            }
        }
        // precision
        if i < bytes.len() && bytes[i] == b'.' {
            i += 1;
            if i < bytes.len() && bytes[i] == b'*' {
                i += 1;
            } else {
                while i < bytes.len() && bytes[i].is_ascii_digit() {
                    i += 1;
                }
            }
        }
        // length modifiers (ignored for sizing except they hint size)
        let mut is_long = false;
        let mut is_size = false;
        while i < bytes.len() && matches!(bytes[i], b'l' | b'L' | b'h' | b'z' | b'j' | b't') {
            if bytes[i] == b'l' || bytes[i] == b'L' {
                is_long = true;
            }
            if bytes[i] == b'z' || bytes[i] == b'j' {
                is_size = true;
            }
            i += 1;
        }
        if i >= bytes.len() {
            out.push_str("%<truncated>");
            break;
        }
        let conv = bytes[i] as char;
        i += 1;
        match conv {
            '%' => out.push('%'),
            'c' => {
                if ai < args.len() {
                    out.push((args[ai] as u8) as char);
                    ai += 1;
                }
            }
            's' => {
                if ai < args.len() {
                    let p = args[ai] as *const u8;
                    ai += 1;
                    if !p.is_null() {
                        unsafe {
                            let cstr = std::ffi::CStr::from_ptr(p as *const i8);
                            out.push_str(&cstr.to_string_lossy());
                        }
                    } else {
                        out.push_str("(null)");
                    }
                }
            }
            'd' | 'i' => {
                if ai < args.len() {
                    let v = if is_long || is_size {
                        args[ai] as i64
                    } else {
                        args[ai] as i32 as i64
                    };
                    out.push_str(&v.to_string());
                    ai += 1;
                }
            }
            'u' => {
                if ai < args.len() {
                    let v = if is_long || is_size {
                        args[ai]
                    } else {
                        args[ai] as u32 as u64
                    };
                    out.push_str(&v.to_string());
                    ai += 1;
                }
            }
            'x' => {
                if ai < args.len() {
                    out.push_str(&format!("{:x}", args[ai]));
                    ai += 1;
                }
            }
            'X' => {
                if ai < args.len() {
                    out.push_str(&format!("{:X}", args[ai]));
                    ai += 1;
                }
            }
            'p' => {
                if ai < args.len() {
                    out.push_str(&format!("0x{:x}", args[ai]));
                    ai += 1;
                }
            }
            other => {
                out.push('%');
                out.push(other);
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn format_no_args() {
        assert_eq!(format_bof("hello from bof", &[]), "hello from bof");
    }
    #[test]
    fn format_with_args() {
        // only first two slots used
        let s = format_bof("v=%d s=%s", &[42i32 as u64, 0]);
        assert!(s.starts_with("v=42 s="));
    }
}
