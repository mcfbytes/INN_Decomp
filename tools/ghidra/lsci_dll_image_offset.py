#!/usr/bin/env python3
"""Print the LSCI DLL image offset: 4 + 2 * (u16 relocation count at offset 2)."""

import struct
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: lsci_dll_image_offset.py <file.dll>", file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as handle:
        header = handle.read(4)
    if len(header) < 4:
        print("file shorter than 4 bytes", file=sys.stderr)
        return 1
    relocations = struct.unpack_from("<H", header, 2)[0]
    print(4 + 2 * relocations)
    return 0


if __name__ == "__main__":
    sys.exit(main())
