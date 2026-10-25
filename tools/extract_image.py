#!/usr/bin/env python3
"""Cuts a code image out of an INN_DUMP_AT memory dump (docs/lifting.md, "Images loaded by the program").

usage: extract_image.py <dump.bin> <linear> <bytes> <out>

<linear> is the image's linear address (segment * 16 + offset; hex with 0x, or seg:off in hex), <bytes>
its size. Take the dump at the first far call into the image (a module address point such as
INN_DUMP_AT=PROG.EXE:1234:0000:1), before the image's own code has changed its data.
"""
import pathlib
import sys


def parse_linear(text):
    if ':' in text:
        seg, off = text.split(':')
        return int(seg, 16) * 16 + int(off, 16)
    return int(text, 0)


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__.strip().split('\n\n')[1])
    dump = pathlib.Path(sys.argv[1]).read_bytes()
    linear, size = parse_linear(sys.argv[2]), int(sys.argv[3], 0)
    if size <= 0 or linear + size > len(dump):
        sys.exit(f'{sys.argv[2]} + {size} bytes lies outside the dump ({len(dump)} bytes)')
    pathlib.Path(sys.argv[4]).write_bytes(dump[linear:linear + size])


if __name__ == '__main__':
    main()
