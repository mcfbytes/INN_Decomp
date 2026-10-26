#!/usr/bin/env python3
"""Writes census.tsv for an export directory: functions and body bytes per code segment.
usage: census.py <export dir> <first application segment, hex> > census.tsv
Kinds, in this order: app (segment views from the first application segment up), overlay (per stub
segment, by the flat range in overlays.tsv), rtl (segment views below that: start-up, library, assembly),
stub (functions inside an overlay stub segment)."""
import collections
import pathlib
import sys

KINDS = ('app', 'overlay', 'rtl', 'stub')


def census(export, first_app):
    ovl = []  # (flat linear, end linear, stub)
    path = export / 'overlays.tsv'
    if path.exists():
        for row in path.read_text().splitlines()[1:]:
            flat, stub, code = row.split('\t')[:3]
            ovl.append((int(flat, 16) * 16, int(flat, 16) * 16 + int(code), stub))
    stubs = {s for _a, _b, s in ovl}
    rows = collections.defaultdict(lambda: [0, 0])
    for row in (export / 'index.tsv').read_text().splitlines()[1:]:
        addr, _name, size = row.split('\t')[:3]
        seg, off = addr.split(':')
        linear = int(seg, 16) * 16 + int(off, 16)
        for lo, hi, stub in ovl:
            if lo <= linear < hi:
                key = ('overlay', stub)
                break
        else:
            if seg in stubs:
                key = ('stub', seg)
            else:
                key = ('app' if int(seg, 16) >= first_app else 'rtl', seg)
        rows[key][0] += 1
        rows[key][1] += int(size)
    out = ['kind\tsegment_or_stub\tfunctions\tbytes']
    for key in sorted(rows, key=lambda k: (KINDS.index(k[0]), k[1])):
        out.append(f'{key[0]}\t{key[1]}\t{rows[key][0]}\t{rows[key][1]}')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.splitlines()[1])
    sys.stdout.write(census(pathlib.Path(sys.argv[1]), int(sys.argv[2], 16)))
