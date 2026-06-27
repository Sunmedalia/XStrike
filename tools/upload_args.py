#!/usr/bin/env python3
"""Build the CS-packed arg buffer for the `upload` BOF.

Two BeaconDataExtract blobs:
    [4-byte big-endian length][remote path bytes]
    [4-byte big-endian length][file contents]

Usage:
    python tools/upload_args.py <remote_path> <local_file> [out.bin]
    python tools/upload_args.py C:\\Temp\\tool.exe ./tool.exe

The resulting .bin is loaded via the GUI's args "file" source (or POST
/api/bofs/upload/run?implant=<id> base64-encoded).
"""
import struct
import sys


def pack_extract(data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + data


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        sys.exit(1)
    remote_path = argv[1].encode("utf-8", errors="replace")
    local_file = argv[2]
    out = argv[3] if len(argv) > 3 else "upload_args.bin"
    with open(local_file, "rb") as f:
        contents = f.read()
    buf = pack_extract(remote_path) + pack_extract(contents)
    with open(out, "wb") as f:
        f.write(buf)
    print(f"wrote {out} ({len(buf)} bytes): path={argv[1]!r} file={local_file} ({len(contents)} bytes)")


if __name__ == "__main__":
    main(sys.argv)
