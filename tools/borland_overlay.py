"""Borland overlay (VROOMM) layout of an MZ file; see docs/lifting.md, "Overlays".

Shared by gen_lift_table.py and borland_ovl_flatten.py; mirrors runtime/src/borland_overlay.c.
"""
import struct
from dataclasses import dataclass

STUB_FLAGS = 3
STUB_THUNKS = 0x20
THUNK_BYTES = 5


@dataclass
class Overlay:
    stub_seg: int     # image-relative segment of the INT 3Fh stub; names the overlay
    file_off: int     # absolute file offset of the code
    code: bytes
    fixups: list      # code offsets of words that get the program's load segment added
    entries: int      # thunks in the stub, 5 bytes each from +20h


def module_end(data):
    last, pages = struct.unpack_from('<HH', data, 2)
    return pages * 512 - (512 - last if last else 0)


def overlays(data):
    """{stub segment: Overlay} for an MZ file with an FBOV trailer, {} without one."""
    if data[:2] not in (b'MZ', b'ZM'):
        return {}
    end = module_end(data)
    if data[end:end + 4] != b'FBOV':
        return {}
    data_start = end + 16
    _, table, rows = struct.unpack_from('<III', data, end + 4)
    header = struct.unpack_from('<H', data, 8)[0] * 16
    found = {}
    for i in range(rows):
        seg, _, flags, _ = struct.unpack_from('<HHHH', data, table + 8 * i)
        if flags != STUB_FLAGS:
            continue
        stub = header + seg * 16
        if data[stub:stub + 2] != b'\xcd\x3f':
            raise ValueError(f'overlay stub {seg:04X} does not start with INT 3Fh')
        code_off, code_bytes, fixup_bytes, entries = struct.unpack_from('<IHHH', data, stub + 4)
        start = data_start + code_off
        fixups = list(struct.unpack_from(f'<{fixup_bytes // 2}H', data, start + code_bytes))
        found[seg] = Overlay(seg, start, data[start:start + code_bytes], fixups, entries)
    return found
