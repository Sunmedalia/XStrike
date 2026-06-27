#!/usr/bin/env python3
"""Build a BOF argument buffer (Cobalt Strike packed format) for `cmd_exec`.

CS arg format for a single BeaconDataExtract string:
    [4-byte big-endian length][that many bytes of data]

Usage:
    python tools/cmd_exec_args.py whoami                 # writes cmd_args.bin
    python tools/cmd_args.py "ipconfig /all"             # quoted multi-word
    python tools/cmd_args.py "dir C:\\Windows" out.bin   # custom output path

The resulting .bin is what you load in the GUI's args "file" source, or pass to
the server's `loadb` / `POST /api/bofs/cmd_exec/run` (base64-encoded).
"""
import struct
import sys


def pack_extract_string(s: str) -> bytes:
    raw = s.encode("utf-8", errors="replace")
    return struct.pack(">I", len(raw)) + raw


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = argv[1]
    out = argv[2] if len(argv) > 2 else "cmd_args.bin"
    data = pack_extract_string(cmd)
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out} ({len(data)} bytes) for command: {cmd!r}")


if __name__ == "__main__":
    main(sys.argv)
