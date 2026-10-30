#!/usr/bin/env python3
"""Tests tools/match_compile.py without DOSBox:  python3 -I tools/test_match_compile.py

Covers the address resolution, the masked comparison and the MZ relocation masking on a synthetic
MZ module; the end-to-end compile is exercised by running the tool on `matching/`.
"""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import match_compile as mc  # noqa: E402


class CaseParsing(unittest.TestCase):
    def test_header_wins_over_file_name(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / '0000_0001.c'
            path.write_text('/* match: CGENN 08A6:000C */\nint far orig(void) { return 0; }\n')
            case = mc.load_case(path)
        self.assertIsNone(case.error)
        self.assertEqual((case.module, case.seg, case.off), ('CGENN', 0x08A6, 0x0C))
        self.assertEqual(case.addr, '08A6:000C')

    def test_missing_header_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / '1E5E_0556.c'
            path.write_text('int far orig(void) { return 0; }\n')
            case = mc.load_case(path)
        self.assertIn('header', case.error)
        self.assertEqual(case.addr, '1E5E:0556')


class Resolution(unittest.TestCase):
    # DARKSTRT's first overlay: stubs at paragraph 1E5E, code flattened to paragraph 3EBE.
    ROWS = [(0x3EBE * 16, 1676, 0x1E5E)]

    def test_stub_address(self):
        self.assertEqual(mc.key_for(0x1E5E, 0x556, self.ROWS), ('ovl', 0x1E5E, 0x556))

    def test_flat_address(self):
        self.assertEqual(mc.key_for(0x3EBE, 0x556, self.ROWS), ('ovl', 0x1E5E, 0x556))

    def test_resident_address(self):
        # Ghidra calls the CGENN function 0894:012C; the report calls the same linear 08A6:000C.
        self.assertEqual(mc.key_for(0x08A6, 0x000C, []), ('res', 0x8A6C))
        self.assertEqual(mc.key_for(0x0894, 0x012C, []), ('res', 0x8A6C))

    def test_flat_range_wins_over_a_stub_number(self):
        # A Ghidra segment can share its number with another overlay's stub; the flat range decides.
        rows = [(0x1000 * 16, 0x200, 0x3000), (0x0000, 0x10, 0x1000)]
        self.assertEqual(mc.key_for(0x1000, 0x10, rows), ('ovl', 0x3000, 0x10))

    def test_resident_outside_overlay(self):
        self.assertEqual(mc.key_for(0x0000, 0x0000, self.ROWS), ('res', 0))


class OverlaySizes(unittest.TestCase):
    def test_stub_name_ignores_a_thunk_row(self):
        # The index holds a 5-byte thunk at the stub address and the 44-byte function under the flat
        # segment; a stub-named case must take the function size.
        md = object.__new__(mc.ModuleData)
        md.overlay_rows = [(0x4814 * 16, 5321, 0x1EA6)]
        md.stub_segs = {0x1EA6}
        md.by_addr = {(0x1EA6, 0xBB): 5, (0x4814, 0x132): 44}
        md.by_key = {('thunk', 0x1EA6, 0xBB): {5}, ('ovl', 0x1EA6, 0x132): {44}}
        key, size = md.resolve(mc.Case(Path('x'), 'DARKSTRT', 0x1EA6, 0x132))
        self.assertEqual((key, size), (('ovl', 0x1EA6, 0x132), 44))

    def test_thunk_address_reads_resident_bytes(self):
        # A stub address with no overlay function behind it is an index thunk row; size and bytes come
        # from the resident image, not from overlay code at that offset.
        md = object.__new__(mc.ModuleData)
        md.overlay_rows = [(0x4814 * 16, 5321, 0x1EA6)]
        md.stub_segs = {0x1EA6}
        md.by_addr = {(0x1EA6, 0xBB): 5}
        md.by_key = {('thunk', 0x1EA6, 0xBB): {5}}
        key, size = md.resolve(mc.Case(Path('x'), 'DARKSTRT', 0x1EA6, 0xBB))
        self.assertEqual((key, size), (('res', 0x1EB1B), 5))


class Comparison(unittest.TestCase):
    def test_exact(self):
        self.assertEqual(mc.compare(b'\x55\x8b', b'\x01\x01', b'\x55\x8b', b'\x01\x01'),
                         ('exact', None))

    def test_fixup_bytes_are_ignored(self):
        # A wildcard on either side hides the byte.
        self.assertEqual(mc.compare(b'\x55\x8b', b'\x01\x00', b'\x55\x00', b'\x01\x01'),
                         ('exact', None))
        self.assertEqual(mc.compare(b'\x55\x8b', b'\x00\x01', b'\xff\x8b', b'\x01\x01'),
                         ('exact', None))

    def test_diff(self):
        self.assertEqual(mc.compare(b'\x55\x8b\xec', b'\x01\x01\x01',
                                    b'\x55\x8a\xec', b'\x01\x01\x01'), ('diff', 1))

    def test_length(self):
        self.assertEqual(mc.compare(b'\x55\x8b', b'\x01\x01',
                                    b'\x55\x8b\xec', b'\x01\x01\x01'), ('length', 2))
        self.assertEqual(mc.compare(b'\x55\x8b', b'\x01\x01',
                                    b'\x55\x8a\xec', b'\x01\x01\x01'), ('length', 1))


class MzRelocations(unittest.TestCase):
    """A resident function's relocated word is masked, everything else stays comparable."""

    def test_relocated_word_is_masked(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            export = root / 'ghidra' / 'MOD'
            export.mkdir(parents=True)
            (export / 'index.tsv').write_text(
                'addr\tname\tsize\tcallers\tcallees\tconvention\n'
                '0001:0000\tFUN\t4\t0\t0\tunknown\n')
            header = bytearray(0x20)
            header[:2] = b'MZ'
            struct.pack_into('<H', header, 2, 0)      # last page bytes
            struct.pack_into('<H', header, 4, 1)      # pages
            struct.pack_into('<H', header, 6, 1)      # one relocation
            struct.pack_into('<H', header, 8, 2)      # header size in paragraphs
            struct.pack_into('<H', header, 0x18, 0x1C)
            struct.pack_into('<HH', header, 0x1C, 0x12, 0)  # word at linear 0x12
            image = bytes(range(32))
            exe = root / 'MOD.EXE'
            exe.write_bytes(bytes(header) + image)
            md = mc.ModuleData(exe, export)
            case = mc.Case(Path('case.c'), 'MOD', 1, 0)
        data, known, kind = md.original(case)
        self.assertEqual(kind, 'resident')
        self.assertEqual(data, image[0x10:0x14])
        self.assertEqual(known, b'\x01\x01\x00\x00')

    def test_ghidra_segmentation_still_resolves(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            export = root / 'ghidra' / 'MOD'
            export.mkdir(parents=True)
            (export / 'index.tsv').write_text(
                'addr\tname\tsize\tcallers\tcallees\tconvention\n'
                '0002:0014\tFUN\t3\t0\t0\tunknown\n')  # linear 0x34
            header = bytearray(0x20)
            header[:2] = b'MZ'
            struct.pack_into('<H', header, 8, 2)
            image = bytes(range(0x40))
            exe = root / 'MOD.EXE'
            exe.write_bytes(bytes(header) + image)
            md = mc.ModuleData(exe, export)
            # The report prints the real paragraph: linear 0x34 is 0002:0014 here, 0003:0004 elsewhere.
            case = mc.Case(Path('case.c'), 'MOD', 3, 4)
            data, _known, _kind = md.original(case)
        self.assertEqual(data, image[0x34:0x37])


class DosboxConfig(unittest.TestCase):
    def test_config_runs_dpmiload_then_bcc(self):
        conf = mc.dbx_conf('/tmp/work', '/opt/bcpp30', '-c -ml -O2 -IC:\\INCLUDE')
        lines = conf.splitlines()
        self.assertIn('dpmiload', lines)
        self.assertIn('bcc -c -ml -O2 -IC:\\INCLUDE IN.C', lines)
        self.assertLess(lines.index('dpmiload'), lines.index('bcc -c -ml -O2 -IC:\\INCLUDE IN.C'))
        self.assertIn('mount d "/tmp/work"', lines)


if __name__ == '__main__':
    unittest.main()
