#!/usr/bin/env python3
"""Rewrites Borland/Microsoft floating-point emulator calls in an MZ image into the x87 code they stand for.

Programs linked with Borland's EMU.LIB carry every x87 instruction as an interrupt the linker made out
of it (fixups FIDRQQ, FIERQQ, FIWRQQ...). At run time the emulator either interprets them or, with a
coprocessor present, patches each one in place into the instruction below on its first execution:

    CD 34+i modrm ...      ->  9B D8+i modrm ...          (FWAIT, escape D8..DF)
    CD 3C b modrm ...      ->  9B seg (b | C0) modrm ...  (segment override; b >> 6: 0 DS, 1 CS, 2 SS, 3 ES)
    CD 3D                  ->  90 9B                      (FWAIT alone)

INT 3Eh (emulator shortcuts) is left alone. The output is the image a coprocessor run converges to, which
is what disassemblers and decompilers should see (docs/runtime.md, "FPU"). Only code ranges are patched:
data inside them that happens to hold CD 34..3D is the known risk, so give the ranges tightly.

Usage: borland_emu_patch.py <in.exe> <out.exe> [--code SEG-SEG ...] [--sites sites.tsv]
--code takes image-relative paragraph ranges (hex, end exclusive); without it the whole load image is
scanned. --sites lists every patched place: image offset, kind, original and new bytes.
"""
import argparse
import struct
import sys

SEG_PREFIX = {0: 0x3E, 1: 0x2E, 2: 0x36, 3: 0x26}
SEG_NAME = {0x3E: 'DS', 0x2E: 'CS', 0x36: 'SS', 0x26: 'ES'}


def patch(image, lo, hi, sites):
    i = lo
    while i < hi - 1:
        if image[i] != 0xCD or not 0x34 <= image[i + 1] <= 0x3D:
            i += 1
            continue
        vec = image[i + 1]
        old = bytes(image[i:i + 3])
        if vec <= 0x3B:
            image[i:i + 2] = bytes((0x9B, 0xD8 + vec - 0x34))
            kind, n = 'esc', 2
        elif vec == 0x3C:
            b = image[i + 2]
            image[i:i + 3] = bytes((0x9B, SEG_PREFIX[b >> 6], b | 0xC0))
            kind, n = 'seg ' + SEG_NAME[SEG_PREFIX[b >> 6]], 3
        else:
            image[i:i + 2] = bytes((0x90, 0x9B))
            kind, n = 'fwait', 2
        sites.append((i, kind, old[:n], bytes(image[i:i + n])))
        i += n
    return sites


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--code', action='append', default=[], help='paragraph range SEG-SEG (hex, end exclusive)')
    ap.add_argument('--sites')
    a = ap.parse_args()
    data = bytearray(open(a.src, 'rb').read())
    if data[:2] not in (b'MZ', b'ZM'):
        sys.exit('not an MZ file')
    header = struct.unpack_from('<H', data, 8)[0] * 16
    last, pages = struct.unpack_from('<HH', data, 2)
    end = pages * 512 - (512 - last if last else 0)
    image = data[header:end]
    ranges = [(0, len(image))]
    if a.code:
        ranges = []
        for r in a.code:
            lo, hi = (int(x, 16) * 16 for x in r.split('-'))
            ranges.append((lo, min(hi, len(image))))
    sites = []
    for lo, hi in ranges:
        patch(image, lo, hi, sites)
    data[header:end] = image
    open(a.dst, 'wb').write(data)
    if a.sites:
        with open(a.sites, 'w') as f:
            f.write('image_off\tkind\toriginal\tpatched\n')
            for off, kind, old, new in sites:
                f.write('%05X\t%s\t%s\t%s\n' % (off, kind, old.hex(' '), new.hex(' ')))
    counts = {}
    for _, kind, _, _ in sites:
        counts[kind.split()[0]] = counts.get(kind.split()[0], 0) + 1
    print('%d emulator sequences patched: %s' % (len(sites), ', '.join('%s %d' % kv for kv in sorted(counts.items()))))


if __name__ == '__main__':
    main()
