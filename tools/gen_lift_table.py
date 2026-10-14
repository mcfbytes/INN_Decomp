#!/usr/bin/env python3
"""Generates the lifted-function registration tables from LIFTED(...) markers.

Each module directory under the lifted tree holds module.txt (two lines: the module name as the
loader announces it, e.g. LSCITV.EXE, and the path of the unpacked original) and C files whose
lifted functions are marked with
    LIFTED(0x07D1, 0x00FC, lsci_kAbs, LIFT_FAR, LSCI_KERNEL_SCRATCH)
The entry signatures are read from the original at generation time, so no original bytes are
ever stored in source. Usage: gen_lift_table.py <lifted_dir> <out.c>

Borland overlays (docs/lifting.md, "Overlays"): when the segment of a marker is the stub segment of
an overlay of the module's EXE, the entry belongs to that overlay. Its offset is relative to the
overlay's code, it registers under "<MODULE>#<stub segment>", which the runtime announces when the
overlay manager reads the code, and its signature and relocations come from the overlay's code and
fixup table. Signature bytes past the end of the overlay's code are not compared.
"""
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import borland_overlay  # noqa: E402

MARKER = re.compile(
    r'^LIFTED\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(LIFT_FAR_NOSHADOW|LIFT_NEAR_NOSHADOW|LIFT_FAR|LIFT_NEAR|LIFT_REGION)\s*,'
    r'\s*([\w|\s]+?)\s*\)', re.M)
SIGNATURE_LEN = 12
SPLIT_HEAD = 1 << 15  # byte 0 is the high half of a relocated word that starts before the entry


def borland_overlays(data):
    """{stub segment: (code, fixup offsets)} from an MZ file's FBOV trailer."""
    return {seg: (o.code, set(o.fixups)) for seg, o in borland_overlay.overlays(data).items()}


def mz_image(data):
    """Load image and the image offsets of its relocated words."""
    count = struct.unpack_from('<H', data, 6)[0]
    table = struct.unpack_from('<H', data, 0x18)[0]
    header = struct.unpack_from('<H', data, 8)[0] * 16
    relocs = set()
    for i in range(count):
        off, seg = struct.unpack_from('<HH', data, table + 4 * i)
        relocs.add(seg * 16 + off)
    return data[header:], relocs


def lsci_dll_image(data):
    """LSCI DLL: u16 paragraphs, u16 relocation count, u16 relocation offsets, image."""
    count = struct.unpack_from('<H', data, 2)[0]
    relocs = set(struct.unpack_from(f'<{count}H', data, 4))
    return data[4 + 2 * count:], relocs


def image_of(exe_path):
    """Load image, its relocated offsets, and its overlays."""
    data = exe_path.read_bytes()
    if data[:2] in (b'MZ', b'ZM'):
        return (*mz_image(data), borland_overlays(data))
    if exe_path.suffix.upper() == '.DLL':
        return (*lsci_dll_image(data), {})
    return data, set(), {}


def read_module(module_dir):
    lines = (module_dir / 'module.txt').read_text().split('\n')
    name, exe = lines[0].strip(), lines[1].strip()
    exe_path = pathlib.Path(exe)
    if not exe_path.is_absolute():
        exe_path = module_dir / exe_path
    return (name, *image_of(exe_path))


def reloc_mask(start, relocs):
    mask = SPLIT_HEAD if start - 1 in relocs else 0
    for i in range(SIGNATURE_LEN):
        if start + i in relocs:
            mask |= 1 << i
    return mask


def entries_of(module_dir, name, image, relocs, overlays):
    for source in sorted(module_dir.glob('*.c')):
        for match in MARKER.finditer(source.read_text()):
            seg, off = int(match[1], 16), int(match[2], 16)
            module, unknown = name, 0
            if seg in overlays:
                code, fixups = overlays[seg]
                if off >= len(code):
                    sys.exit(f'{source}: {match[3]} lies outside overlay {seg:04X}')
                module, seg, start = f'{name}#{seg:04X}', 0, off
                signature = code[off:off + SIGNATURE_LEN]
                for i in range(len(signature), SIGNATURE_LEN):
                    unknown |= 1 << i
                signature = signature.ljust(SIGNATURE_LEN, b'\0')
                mask = reloc_mask(start, fixups)
            else:
                start = seg * 16 + off
                signature = image[start:start + SIGNATURE_LEN]
                mask = reloc_mask(start, relocs)
            if len(signature) != SIGNATURE_LEN:
                sys.exit(f'{source}: {match[3]} lies outside the image')
            kind = match[4]
            yield (module, seg, off, match[3], kind.startswith('LIFT_NEAR'), match[5], signature, mask,
                   kind == 'LIFT_REGION', kind.endswith('_NOSHADOW'), unknown)


def emit_module(out, index, entries):
    for _, _, _, fn, *_ in entries:
        out.append(f'void {fn}(Machine *m);')
    out.append(f'static const LiftedEntry module_{index}[] = {{')
    for module, seg, off, fn, near, scratch, signature, mask, region, no_shadow, unknown in entries:
        sig = ', '.join(f'0x{b:02X}' for b in signature)
        out.append(f'    {{ "{module}", 0x{seg:04X}, 0x{off:04X}, {fn}, "{fn}", {str(near).lower()}, '
                   f'{scratch}, {{ {sig} }}, 0x{mask:04X}, {str(region).lower()}, {str(no_shadow).lower()}, '
                   f'0x{unknown:04X} }},')
    out.append('};')


def main():
    lifted_dir, out_path = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out = ['/* Generated by tools/gen_lift_table.py; contains original bytes: never commit. */',
           '#include "lift.h"', '#include "lifted_conventions.h"', '']
    modules = []
    for module_dir in sorted(p for p in lifted_dir.iterdir() if (p / 'module.txt').exists()):
        name, image, relocs, overlays = read_module(module_dir)
        entries = list(entries_of(module_dir, name, image, relocs, overlays))
        if entries:
            emit_module(out, len(modules), entries)
            modules.append(len(entries))
    out.append('')
    out.append('void lifted_register_all(Machine *m)')
    out.append('{')
    for i, count in enumerate(modules):
        out.append(f'    lift_add_table(m, module_{i}, {count});')
    out.append('}')
    out_path.write_text('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
