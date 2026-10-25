#!/usr/bin/env python3
"""Turns an overlaid Borland MZ program into one plain MZ program for static analysis.

Each overlay's code is placed at its own segment after the image and its BSS, its fixups become MZ
relocations, and every INT 3Fh thunk in the stubs becomes a relocated JMP FAR to the overlay's
code, so Ghidra sees overlay functions as ordinary far functions with cross-references. The map
lists, per overlay, the flat segment, the stub segment that names it and the code size; lifted
markers use the stub segment (docs/lifting.md, "Overlays").
Usage: borland_ovl_flatten.py <in.exe> <out.exe> <map.tsv> [names.tsv]
names.tsv names each overlay entry ovl_<stub>_<offset> at its flat address (export_module.sh).
"""
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import borland_overlay  # noqa: E402

JMP_FAR = 0xEA
RELOC_TABLE = 0x40


def flatten(data):
    ovls = borland_overlay.overlays(data)
    if not ovls:
        raise SystemExit('no Borland overlays in this file')
    header = struct.unpack_from('<H', data, 8)[0] * 16
    count, table = struct.unpack_from('<HH', data, 6)[0], struct.unpack_from('<H', data, 0x18)[0]
    # MZ rows are (offset, segment); keep (segment, offset) like the overlay rows added below.
    relocs = [struct.unpack_from('<HH', data, table + 4 * i)[::-1] for i in range(count)]
    image = bytearray(data[header:borland_overlay.module_end(data)])
    min_paras = struct.unpack_from('<H', data, 0x0A)[0]
    # Leave the BSS and stack the loader would add free, so data references never hit overlay code.
    seg = (len(image) + 15) // 16 + min_paras
    rows, names = [], []
    for stub, o in sorted(ovls.items()):
        image.extend(b'\0' * (seg * 16 - len(image)))
        image.extend(o.code)
        relocs.extend((seg, off) for off in o.fixups)
        for i in range(o.entries):
            thunk = stub * 16 + borland_overlay.STUB_THUNKS + borland_overlay.THUNK_BYTES * i
            if image[thunk:thunk + 2] != b'\xcd\x3f':
                raise SystemExit(f'stub {stub:04X} thunk {i} is not INT 3Fh')
            target = struct.unpack_from('<H', image, thunk + 2)[0]
            image[thunk] = JMP_FAR
            struct.pack_into('<HH', image, thunk + 1, target, seg)
            relocs.append((stub, borland_overlay.STUB_THUNKS + borland_overlay.THUNK_BYTES * i + 3))
        rows.append((seg, stub, len(o.code), o.entries))
        entries = {struct.unpack_from('<H', image, stub * 16 + borland_overlay.STUB_THUNKS
                                      + borland_overlay.THUNK_BYTES * i + 1)[0] for i in range(o.entries)}
        names.extend(f'{seg:04X}:{off:04X}\tovl_{stub:04X}_{off:04X}' for off in sorted(entries))
        seg = (seg * 16 + len(o.code) + 15) // 16
    new_header = (RELOC_TABLE + 4 * len(relocs) + 15) // 16 * 16
    total = new_header + len(image)
    out = bytearray(data[:0x1C]) + bytes(new_header - 0x1C)
    struct.pack_into('<HHHH', out, 2, total % 512, (total + 511) // 512, len(relocs), new_header // 16)
    struct.pack_into('<H', out, 0x18, RELOC_TABLE)
    for i, (s, o) in enumerate(relocs):
        struct.pack_into('<HH', out, RELOC_TABLE + 4 * i, o, s)
    return bytes(out + image), rows, names


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__.splitlines()[-2])
    flat, rows, names = flatten(pathlib.Path(sys.argv[1]).read_bytes())
    pathlib.Path(sys.argv[2]).write_bytes(flat)
    lines = ['flat_seg\tstub_seg\tcode_bytes\tentries']
    lines += [f'{seg:04X}\t{stub:04X}\t{size}\t{entries}' for seg, stub, size, entries in rows]
    pathlib.Path(sys.argv[3]).write_text('\n'.join(lines) + '\n')
    if len(sys.argv) == 5:
        pathlib.Path(sys.argv[4]).write_text('\n'.join(names) + '\n')


if __name__ == '__main__':
    main()
