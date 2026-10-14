#!/usr/bin/env python3
"""Match runtime-library functions from OMF libraries (.LIB/.OBJ) inside DOS MZ load images.

Each public symbol becomes a byte signature running from its offset to the next public symbol
(or segment end), with fixup bytes as wildcards. Usage:
    omf_sigs.py --lib CL.LIB [--lib MATHL.LIB ...] [--min-fixed 12] TARGET.EXE [...]
--names OUT writes the unambiguous matches of one MZ target as seg:off names for
tools/ghidra/export_module.sh; the segment is the nearest far-call target segment at or below
the match.
"""
import argparse
import struct
import sys
from dataclasses import dataclass, field

REC_THEADR, REC_COMENT, REC_MODEND, REC_MODEND32 = 0x80, 0x88, 0x8A, 0x8B
REC_EXTDEF, REC_PUBDEF, REC_PUBDEF32, REC_LNAMES = 0x8C, 0x90, 0x91, 0x96
REC_SEGDEF, REC_SEGDEF32, REC_FIXUPP, REC_FIXUPP32 = 0x98, 0x99, 0x9C, 0x9D
REC_LEDATA, REC_LEDATA32, REC_LIDATA, REC_LIDATA32 = 0xA0, 0xA1, 0xA2, 0xA3
REC_LIB_HEADER, REC_LIB_END = 0xF0, 0xF1

# Bytes patched by a fixup, by OMF location type.
FIXUP_WIDTH = {0: 1, 1: 2, 2: 2, 3: 4, 4: 1, 5: 2, 9: 4, 11: 6, 13: 4}


@dataclass
class Segment:
    name: str
    length: int
    data: bytearray = field(default_factory=bytearray)
    known: bytearray = field(default_factory=bytearray)  # 1 = fixed byte, 0 = wildcard/unknown


@dataclass
class Module:
    name: str
    segments: list
    publics: list  # (name, segment index, offset)


@dataclass
class Signature:
    module: str
    symbol: str
    data: bytes
    mask: bytes
    segment: str = ''

    @property
    def fixed(self):
        return sum(self.mask)


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def u8(self):
        v = self.data[self.pos]
        self.pos += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.data, self.pos)[0]
        self.pos += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.data, self.pos)[0]
        self.pos += 4
        return v

    def index(self):
        v = self.u8()
        return ((v & 0x7F) << 8 | self.u8()) if v & 0x80 else v

    def name(self):
        n = self.u8()
        s = self.data[self.pos:self.pos + n].decode('latin-1')
        self.pos += n
        return s

    def at_end(self):
        return self.pos >= len(self.data)


def parse_fixupp(body, is32, last_ledata):
    """Yields (segment index, data offset, width) for each fixup patching last_ledata."""
    r = Reader(body)
    while not r.at_end():
        b = r.u8()
        if not b & 0x80:  # THREAD subrecord
            method = b >> 2 & 7
            is_frame = b & 0x40
            if not is_frame or method < 4:
                r.index()
            continue
        locat = (b << 8) | r.u8()
        location = locat >> 10 & 0xF
        record_off = locat & 0x3FF
        fixdat = r.u8()
        if not fixdat & 0x80:
            frame = fixdat >> 4 & 7
            if frame < 3:
                r.index()
        if not fixdat & 0x08:
            r.index()
        if not fixdat & 0x04:
            r.u32() if is32 else r.u16()
        if last_ledata:
            seg_index, base = last_ledata
            yield seg_index, base + record_off, FIXUP_WIDTH.get(location, 2)


def parse_module(records):
    lnames, segments, publics = [None], [None], []
    name, last_ledata = '?', None
    for rtype, body in records:
        r = Reader(body)
        is32 = rtype & 1
        base_type = rtype & ~1
        if rtype == REC_THEADR:
            name = r.name()
        elif rtype == REC_LNAMES:
            while not r.at_end():
                lnames.append(r.name())
        elif base_type == REC_SEGDEF:
            attr = r.u8()
            if attr >> 5 == 0:
                r.u16(); r.u8()
            length = r.u32() if is32 else r.u16()
            if attr & 0x02 and not is32:
                length = 0x10000
            seg_name = lnames[r.index()]
            segments.append(Segment(seg_name, length, bytearray(length), bytearray(length)))
        elif base_type == REC_PUBDEF:
            group, seg = r.index(), r.index()
            if seg == 0:
                r.u16()
            while not r.at_end():
                sym = r.name()
                off = r.u32() if is32 else r.u16()
                r.index()
                if seg:
                    publics.append((sym, seg, off))
        elif base_type == REC_LEDATA:
            seg = r.index()
            off = r.u32() if is32 else r.u16()
            data = body[r.pos:]
            s = segments[seg]
            s.data[off:off + len(data)] = data
            s.known[off:off + len(data)] = b'\x01' * len(data)
            last_ledata = (seg, off)
        elif base_type == REC_LIDATA:
            last_ledata = None
        elif base_type == REC_FIXUPP:
            for seg, off, width in parse_fixupp(body, is32, last_ledata):
                s = segments[seg]
                for i in range(off, min(off + width, len(s.known))):
                    s.known[i] = 0
    return Module(name, segments, publics)


def parse_omf_file(data):
    """Yields Module for a single .OBJ or each module of a .LIB."""
    page = 1
    pos = 0
    if data[0] == REC_LIB_HEADER:
        page = struct.unpack_from('<H', data, 1)[0] + 3
        pos = page
    yield from parse_omf_file_from(data, pos, page)


def parse_omf_file_from(data, pos, page):
    records = []
    while pos + 3 <= len(data):
        rtype = data[pos]
        length = struct.unpack_from('<H', data, pos + 1)[0]
        if rtype == REC_LIB_END:
            return
        records.append((rtype, data[pos + 3:pos + 3 + length - 1]))
        pos += 3 + length
        if rtype in (REC_MODEND, REC_MODEND32):
            yield parse_module(records)
            records = []
            pos = (pos + page - 1) // page * page


def signatures(module, max_len=96):
    by_seg = {}
    for sym, seg, off in module.publics:
        by_seg.setdefault(seg, []).append((off, sym))
    for seg, entries in by_seg.items():
        s = module.segments[seg]
        entries.sort()
        for i, (off, sym) in enumerate(entries):
            end = entries[i + 1][0] if i + 1 < len(entries) else s.length
            end = min(end, off + max_len)
            yield Signature(module.name, sym, bytes(s.data[off:end]), bytes(s.known[off:end]), s.name)


def load_image(path):
    data = open(path, 'rb').read()
    if data[:2] in (b'MZ', b'ZM'):
        header_paras = struct.unpack_from('<H', data, 8)[0]
        return data[header_paras * 16:]
    return data


def mz_segments(path):
    """Code segment bases: relocated segment words of far calls and far jumps."""
    data = open(path, 'rb').read()
    count, header, table = (struct.unpack_from('<H', data, o)[0] for o in (6, 8, 0x18))
    image = data[header * 16:]
    segs = set()
    for i in range(count):
        off, seg = struct.unpack_from('<HH', data, table + 4 * i)
        site = seg * 16 + off
        if 3 <= site and site + 2 <= len(image) and image[site - 3] in (0x9A, 0xEA):
            segs.add(struct.unpack_from('<H', image, site)[0])
    return sorted(segs)


def seg_off(segs, linear):
    base = max((s for s in segs if s * 16 <= linear and linear - s * 16 < 0x10000), default=None)
    return None if base is None else (base, linear - base * 16)


def write_names(path, target, matches):
    """matches: [(offset, hit count, module, symbol)]; keeps offsets one module claims once."""
    by_offset = {}
    for offset, hits, module, symbol in matches:
        by_offset.setdefault(offset, []).append((hits, module, symbol))
    segs = mz_segments(target)
    lines, used = [], set()
    for offset, claims in sorted(by_offset.items()):
        if len({m for _, m, _ in claims}) != 1 or any(h != 1 for h, _, _ in claims):
            continue
        so = seg_off(segs, offset)
        name = claims[0][2].lstrip('_') or claims[0][2]
        if so is None or name in used:
            continue
        used.add(name)
        lines.append(f'{so[0]:04X}:{so[1]:04X}\t{name}')
    with open(path, 'w') as f:
        f.write('# Borland C++ 3.1 runtime library, matched by tools/omf_sigs.py\n' + '\n'.join(lines) + '\n')
    return len(lines)


def find_matches(image, sig):
    """Image offsets where sig matches, anchored on its longest fixed run."""
    best_start, best_len, i = 0, 0, 0
    while i < len(sig.mask):
        if sig.mask[i]:
            j = i
            while j < len(sig.mask) and sig.mask[j]:
                j += 1
            if j - i > best_len:
                best_start, best_len = i, j - i
            i = j
        else:
            i += 1
    if best_len < 4:
        return []
    anchor = sig.data[best_start:best_start + best_len]
    hits, pos = [], image.find(anchor)
    while pos >= 0:
        base = pos - best_start
        if base >= 0 and base + len(sig.data) <= len(image) and all(
                not k or image[base + n] == b for n, (b, k) in enumerate(zip(sig.data, sig.mask))):
            hits.append(base)
        pos = image.find(anchor, pos + 1)
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--lib', action='append', required=True)
    ap.add_argument('--min-fixed', type=int, default=12)
    ap.add_argument('--list', action='store_true', help='print every match')
    ap.add_argument('--names', help='write seg:off names of one MZ target here')
    ap.add_argument('targets', nargs='+')
    args = ap.parse_args()
    sigs = []
    for lib in args.lib:
        for module in parse_omf_file(open(lib, 'rb').read()):
            sigs.extend(s for s in signatures(module) if s.fixed >= args.min_fixed)
    print(f'{len(sigs)} signatures', file=sys.stderr)
    for target in args.targets:
        image = load_image(target)
        modules_hit, total_bytes, matches = set(), 0, []
        for sig in sigs:
            hits = find_matches(image, sig)
            if hits:
                if sig.segment.endswith('TEXT'):  # names are for code; data symbols match too
                    matches.append((hits[0], len(hits), sig.module, sig.symbol))
                modules_hit.add(sig.module)
                total_bytes += len(sig.data)
                if args.list:
                    print(f'{target}\t{hits[0]:06X}\t{len(hits)}\t{sig.module}\t{sig.symbol}')
        print(f'{target}: {len(modules_hit)} library modules matched, ~{total_bytes} bytes '
              f'of {len(image)}', file=sys.stderr)
        if args.names:
            print(f'{args.names}: {write_names(args.names, target, matches)} names', file=sys.stderr)


if __name__ == '__main__':
    main()
