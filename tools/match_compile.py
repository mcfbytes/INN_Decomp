#!/usr/bin/env python3
"""Function-level matching-decompilation check against the shipped Borland C++ 3.0 binaries.

Each case is a C file under `private/lifted/matching/<MODULE>/<seg>_<off>.c` holding one function named `orig` and
a header comment

    /* match: MODULE SEG:OFF */

`SEG:OFF` is the address printed by the Ghidra/lift reports: a resident linear address in the module's
segmentation, or, for a Borland VROOMM overlay, the stub segment plus the offset inside the overlay
code (`overlays.tsv` maps the stub to the flat segment Ghidra used). The original bytes are cut from
the media EXE (MZ load image at `SEG*16+OFF`, or the overlay code at the stub/offset) with the length
from `private/ghidra/<MODULE>/index.tsv`; overlay fixup words and MZ relocation words are masked on
the original side.

The case is compiled by BC++ 3.0 in DOSBox 0.74 (DPMILOAD, then `bcc -c -ml -O2 -IC:\\INCLUDE`) from
a DOS 8.3 copy `IN.C` -> `IN.OBJ`, the `orig` public is cut out of the OMF `_TEXT` segment with OMF
fixup bytes masked, and the two byte strings are compared.

Results: `exact` (same length, every compared byte equal), `diff` (same length, a compared byte
differs), `length` (lengths differ) or `tool-error` (the case could not be built or read). The TSV
ledger has the columns module, addr, size, compiled, result, diff: `size`/`compiled` are the original
and compiled function lengths, `diff` is the first differing unmasked offset (or the common length
when a length mismatch has an equal prefix).

usage: match_compile.py --cases DIR --out TSV [--only MODULE]
       [--private DIR] [--media DIR ...] [--bcpp DIR] [--dosbox PATH]
       [--timeout SECONDS] [--keep-work DIR]

Defaults: --cases matching, --private <repo>/private (else <repo>/../private, else the integration
private tree), the three known media trees, --bcpp /mnt/source/tools/borlandcpp/bcpp30 and --dosbox
dosbox. The exit status is 0 when every case produced a result, 1 when a case ended in `tool-error`,
2 on usage or setup errors.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import borland_overlay  # noqa: E402
import omf_sigs  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BCPP = Path('/mnt/source/tools/borlandcpp/bcpp30')
DEFAULT_MEDIA = (
    Path('/mnt/source/TSN-INN-Decomp/work/games/YSERBIUS'),
    Path('/mnt/source/TSN-INN-Decomp/work/games/TWINION'),
    Path('/mnt/source/scratch/inn-defaults/INN/CAWDOR'),
)
INTEGRATION_PRIVATE = Path('/mnt/source/scratch/lift/integ-private')
DEFAULT_BCC_ARGS = '-c -ml -O2 -IC:\\INCLUDE'
DOSBOX_TIMEOUT = 120

CASE_NAME = re.compile(r'^([0-9A-Fa-f]{1,4})_([0-9A-Fa-f]{1,4})$')
MATCH_HEADER = re.compile(
    r'match:\s*([A-Za-z0-9_.-]+)\s+([0-9A-Fa-f]{1,4}):([0-9A-Fa-f]{1,4})')
ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')


class CaseError(Exception):
    """A case cannot be measured; it becomes a `tool-error` ledger row."""


@dataclass
class Case:
    path: Path
    module: str
    seg: int
    off: int
    error: str | None = None

    @property
    def addr(self):
        return f'{self.seg:04X}:{self.off:04X}'


def default_private():
    for candidate in (REPO_ROOT / 'private', REPO_ROOT.parent / 'private', INTEGRATION_PRIVATE):
        if (candidate / 'ghidra').is_dir():
            return candidate
    return REPO_ROOT / 'private'


def load_case(path):
    """Case from its file name, with the `/* match: MODULE SEG:OFF */` header taking precedence."""
    match = CASE_NAME.match(path.stem)
    if match is None:
        raise CaseError(f'{path.name}: expected <seg>_<off>.c')
    module = path.parent.name.upper()
    seg, off = int(match.group(1), 16), int(match.group(2), 16)
    case = Case(path, module, seg, off)
    try:
        text = path.read_text(encoding='latin-1')
    except OSError as exc:
        case.error = f'cannot read {path.name}: {exc}'
        return case
    header = MATCH_HEADER.search(text[:4096])
    if header is None:
        case.error = 'missing /* match: MODULE SEG:OFF */ header'
        return case
    case.module = re.sub(r'\.EXE$', '', header.group(1), flags=re.IGNORECASE).upper()
    case.seg, case.off = int(header.group(2), 16), int(header.group(3), 16)
    return case


def discover(cases_dir, only):
    cases = []
    for path in sorted(cases_dir.glob('*/*.c')):
        if only and path.parent.name.upper() != only.upper():
            continue
        name = CASE_NAME.match(path.stem)
        if name is None:  # not a matching case (helpers and the like)
            continue
        try:
            cases.append(load_case(path))
        except CaseError as exc:
            cases.append(Case(path, path.parent.name.upper(), int(name.group(1), 16),
                              int(name.group(2), 16), str(exc)))
    cases.sort(key=lambda c: (c.module, c.seg, c.off, str(c.path)))
    return cases


def key_for(seg, off, overlay_rows):
    """Index key for an address: ('ovl', stub, offset in the overlay code) or ('res', linear).

    A case address may name the stub segment directly or the flat segment Ghidra used; resident
    addresses are linear, so a Ghidra segmentation different from the MZ one still resolves. The flat
    range is tried first because a Ghidra segment can also be a stub's number.
    """
    linear = seg * 16 + off
    for flat, size, stub in overlay_rows:
        if flat <= linear < flat + size:
            return ('ovl', stub, linear - flat)
    for _flat, _size, stub in overlay_rows:
        if stub == seg:
            return ('ovl', stub, off)
    return ('res', linear)


def compare(orig, orig_known, comp, comp_known):
    """('exact'|'diff'|'length', first differing unmasked offset or None)."""
    common = min(len(orig), len(comp))
    first = None
    for i in range(common):
        if orig_known[i] and comp_known[i] and orig[i] != comp[i]:
            first = i
            break
    if len(orig) != len(comp):
        return 'length', first if first is not None else common
    if first is not None:
        return 'diff', first
    return 'exact', None


class ModuleData:
    """The media EXE, its overlay table and the Ghidra export's function index."""

    def __init__(self, exe, export):
        self.exe = exe
        self.data = exe.read_bytes()
        if self.data[:2] not in (b'MZ', b'ZM'):
            raise CaseError(f'{exe}: not an MZ file')
        self.header = struct.unpack_from('<H', self.data, 8)[0] * 16
        self.image = self.data[self.header:]
        self.relocs = self._relocations()
        try:
            self.mz_overlays = borland_overlay.overlays(self.data)
        except (ValueError, struct.error) as exc:
            raise CaseError(f'{exe}: bad overlay table: {exc}') from None
        self.overlay_rows = self._overlay_rows(export / 'overlays.tsv')
        self.stub_segs = {stub for _flat, _size, stub in self.overlay_rows}
        self.by_addr = {}  # (seg, off) -> size
        self.by_key = {}   # index key -> {sizes}
        self._load_index(export / 'index.tsv')

    def _relocations(self):
        count, table = struct.unpack_from('<HH', self.data, 6)[0], struct.unpack_from(
            '<H', self.data, 0x18)[0]
        sites = []
        for i in range(count):
            off, seg = struct.unpack_from('<HH', self.data, table + 4 * i)
            sites.append(seg * 16 + off)
        return sites

    @staticmethod
    def _overlay_rows(path):
        rows = []
        if path.is_file():
            for line in path.read_text().splitlines()[1:]:
                fields = line.split('\t')
                if len(fields) < 3:
                    continue
                rows.append((int(fields[0], 16) * 16, int(fields[2]), int(fields[1], 16)))
        return rows

    def _load_index(self, path):
        if not path.is_file():
            raise CaseError(f'{path}: no Ghidra index')
        for line in path.read_text().splitlines()[1:]:
            fields = line.split('\t')
            if len(fields) < 3:
                continue
            try:
                seg, off = (int(x, 16) for x in fields[0].split(':'))
                size = int(fields[2])
            except ValueError:
                continue
            self.by_addr.setdefault((seg, off), size)
            # An index row at a stub segment is a thunk into an overlay, not overlay code itself.
            key = ('thunk', seg, off) if seg in self.stub_segs else key_for(
                seg, off, self.overlay_rows)
            self.by_key.setdefault(key, set()).add(size)

    def resolve(self, case):
        """((kind, ...), size) for a case address; see `key_for`."""
        key = key_for(case.seg, case.off, self.overlay_rows)
        # A stub-named case means an offset inside that stub's overlay code, so only a flat row can
        # give its size. If there is no such row, the address is an index row for a thunk (resident
        # code at the stub), and measuring the thunk itself is the honest answer.
        if case.seg in self.stub_segs:
            sizes = self.by_key.get(key, set())
            if not sizes:
                if (case.seg, case.off) in self.by_addr:
                    return ('res', case.seg * 16 + case.off), self.by_addr[(case.seg, case.off)]
                raise CaseError(f'no index.tsv entry for {case.addr}')
        elif (case.seg, case.off) in self.by_addr:
            return key, self.by_addr[(case.seg, case.off)]
        else:
            sizes = self.by_key.get(key, set())
        if not sizes:
            raise CaseError(f'no index.tsv entry for {case.addr}')
        if len(sizes) > 1:
            raise CaseError(f'ambiguous index sizes {sorted(sizes)} for {case.addr}')
        return key, next(iter(sizes))

    def original(self, case):
        """Original bytes and a 1-per-comparable-byte mask."""
        key, size = self.resolve(case)
        if size <= 0:
            raise CaseError(f'{case.addr}: index size is {size}')
        if key[0] == 'ovl':
            stub, offset = key[1], key[2]
            overlay = self.mz_overlays.get(stub)
            if overlay is None:
                raise CaseError(f'{self.exe.name}: overlay stub {stub:04X} not in the FBOV table')
            if offset + size > len(overlay.code):
                raise CaseError(f'{case.addr}: {size} bytes run past the overlay code')
            data = bytes(overlay.code[offset:offset + size])
            known = bytearray(b'\x01' * size)
            for fixup in overlay.fixups:
                if offset <= fixup < offset + size:
                    for i in range(fixup - offset, min(fixup - offset + 2, size)):
                        known[i] = 0
            return data, bytes(known), 'overlay'
        linear = key[1]
        if linear + size > len(self.image):
            raise CaseError(f'{case.addr}: {size} bytes run past the load image')
        data = bytes(self.image[linear:linear + size])
        known = bytearray(b'\x01' * size)
        for site in self.relocs:
            if linear <= site < linear + size:
                for i in range(site - linear, min(site - linear + 2, size)):
                    known[i] = 0
        return data, bytes(known), 'resident'


def find_module_file(directory, module):
    """Case-insensitive `<module>.EXE` (or `<module>`) inside one directory."""
    if not directory.is_dir():
        return None
    wanted = f'{module}.EXE'.upper()
    if module == 'TWGENN-CAW':
        # The Cawdor gallery ships under the plain name; do not pick Twinion's TWGENN.EXE.
        if 'CAWDOR' not in str(directory).upper():
            return None
        wanted = 'TWGENN.EXE'
    for path in directory.iterdir():
        if path.is_file() and path.name.upper() == wanted:
            return path
    return None


class Modules:
    def __init__(self, media, private):
        self.media = list(media)
        self.private = private
        self.cache = {}

    def get(self, module):
        if module in self.cache:
            return self.cache[module]
        exe = None
        for directory in self.media:
            exe = find_module_file(directory, module)
            if exe is not None:
                break
        if exe is None:
            trees = ' or '.join(str(d) for d in self.media)
            raise CaseError(f'{module}: no {module}.EXE in {trees}')
        export = None
        for name in (module, f'{module}.EXE'):
            candidate = self.private / 'ghidra' / name
            if (candidate / 'index.tsv').is_file():
                export = candidate
                break
        if export is None:
            raise CaseError(f'{module}: no private/ghidra/<MODULE>/index.tsv')
        data = ModuleData(exe, export)
        self.cache[module] = data
        return data


def dbx_conf(workdir, bcpp, bcc_args):
    def mount(path):
        return f'"{Path(path).as_posix()}"'

    return '\n'.join([
        '[sdl]',
        'autolock=false',
        '[dosbox]',
        'machine=svga_s3',
        'memsize=16',
        '[cpu]',
        'cycles=max',
        '[autoexec]',
        f'mount c {mount(bcpp)}',
        f'mount d {mount(workdir)}',
        'c:',
        'set PATH=C:\\BIN',
        'd:',
        'dpmiload',
        f'bcc {bcc_args} IN.C',
        'exit',
        '',
    ])


def run_bcc(case_path, workdir, opts):
    """Compile the case as IN.C; returns the OMF object path."""
    shutil.copyfile(case_path, workdir / 'IN.C')
    obj = workdir / 'IN.OBJ'
    if obj.exists():
        obj.unlink()
    conf = workdir / 'dbx.conf'
    conf.write_text(dbx_conf(workdir, opts.bcpp, opts.bcc_args))
    env = dict(os.environ)
    env.setdefault('SDL_VIDEODRIVER', 'dummy')
    env.setdefault('SDL_AUDIODRIVER', 'dummy')
    try:
        proc = subprocess.run(
            [opts.dosbox, '-conf', str(conf), '-exit'],
            cwd=workdir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=opts.timeout)
    except subprocess.TimeoutExpired:
        raise CaseError(f'bcc timed out after {opts.timeout:.0f}s') from None
    except OSError as exc:
        raise CaseError(f'cannot run {opts.dosbox}: {exc}') from None
    if not obj.is_file() or obj.stat().st_size == 0:
        log = ANSI.sub('', proc.stdout.decode('latin-1', 'replace')).strip()
        tail = '\n'.join(log.splitlines()[-8:])
        raise CaseError(f'bcc produced no IN.OBJ (exit {proc.returncode})' + (f'\n{tail}' if tail else ''))
    return obj


def compiled_orig(obj_path):
    """The `orig` public of the OMF `_TEXT` segment and its wildcard mask."""
    try:
        module = next(omf_sigs.parse_omf_file(obj_path.read_bytes()))
    except (StopIteration, IndexError, struct.error, ValueError) as exc:
        raise CaseError(f'{obj_path.name}: unreadable OMF object: {exc}') from None
    matches = [(seg, off) for sym, seg, off in module.publics
               if sym.lower() in ('_orig', 'orig')]
    if not matches:
        raise CaseError('object has no orig public')
    if len(matches) > 1:
        raise CaseError('object has several orig publics')
    seg, off = matches[0]
    segment = module.segments[seg]
    if segment is None or 'TEXT' not in (segment.name or '').upper():
        raise CaseError(f'orig is not in a _TEXT segment (got {getattr(segment, "name", "?")})')
    ends = [o for _sym, s, o in module.publics if s == seg and o > off]
    end = min(ends) if ends else segment.length
    if end > segment.length:
        end = segment.length
    if off >= end:
        raise CaseError('orig public is empty')
    return bytes(segment.data[off:end]), bytes(segment.known[off:end])


def measure(case, modules, opts):
    """One ledger row: (module, addr, size, compiled, result, diff, note)."""
    if case.error:
        return case.module, case.addr, '', '', 'tool-error', '', case.error
    try:
        module = modules.get(case.module)
        orig, orig_known, _kind = module.original(case)
        if opts.keep_work:
            workdir = Path(opts.keep_work) / f'{case.module}_{case.seg:04X}_{case.off:04X}'
            workdir.mkdir(parents=True, exist_ok=True)
            obj = run_bcc(case.path, workdir, opts)
            comp, comp_known = compiled_orig(obj)
        else:
            with tempfile.TemporaryDirectory(prefix='match-compile-') as tmp:
                obj = run_bcc(case.path, Path(tmp), opts)
                comp, comp_known = compiled_orig(obj)
    except CaseError as exc:
        return case.module, case.addr, '', '', 'tool-error', '', str(exc)
    result, diff = compare(orig, orig_known, comp, comp_known)
    note = ''
    if result in ('diff', 'length') and diff is not None and diff < min(len(orig), len(comp)):
        note = f'orig={orig[diff]:02X} compiled={comp[diff]:02X}'
    return (case.module, case.addr, str(len(orig)), str(len(comp)), result,
            '' if diff is None else f'0x{diff:04X}', note)


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cases', default='private/lifted/matching', help='case tree (default: private/lifted/matching)')
    ap.add_argument('--out', required=True, help='TSV ledger to write')
    ap.add_argument('--only', help='only this module')
    ap.add_argument('--private', default=str(default_private()),
                    help='private tree holding ghidra/<MODULE>/ (default: %(default)s)')
    ap.add_argument('--media', action='append', default=None,
                    help='directory holding <MODULE>.EXE (repeatable; default: known trees)')
    ap.add_argument('--bcpp', default=str(DEFAULT_BCPP), help='Borland C++ 3.0 root')
    ap.add_argument('--dosbox', default='dosbox', help='DOSBox binary (default: dosbox)')
    ap.add_argument('--timeout', type=float, default=DOSBOX_TIMEOUT,
                    help='seconds per case (default: %(default)s)')
    ap.add_argument('--bcc-args', default=DEFAULT_BCC_ARGS,
                    help='bc compiler arguments (default: %(default)s)')
    ap.add_argument('--keep-work', metavar='DIR',
                    help='keep each case\'s IN.C/IN.OBJ/dbx.conf under DIR')
    opts = ap.parse_args(argv)
    if opts.media is None:
        opts.media = [str(p) for p in DEFAULT_MEDIA]
    if opts.keep_work:
        Path(opts.keep_work).mkdir(parents=True, exist_ok=True)
    return opts


def main(argv=None):
    opts = parse_args(argv)
    cases_dir = Path(opts.cases)
    if not cases_dir.is_dir():
        print(f'match_compile: {cases_dir}: not a directory', file=sys.stderr)
        return 2
    bcpp = Path(opts.bcpp)
    if not (bcpp / 'BIN' / 'BCC.EXE').is_file():
        print(f'match_compile: {bcpp}: no BIN/BCC.EXE', file=sys.stderr)
        return 2
    cases = discover(cases_dir, opts.only)
    if not cases:
        print(f'match_compile: no cases under {cases_dir}', file=sys.stderr)
        return 2

    modules = Modules([Path(p) for p in opts.media], Path(opts.private))
    rows, errors = [], 0
    for case in cases:
        row = measure(case, modules, opts)
        rows.append(row)
        module, addr, size, compiled, result, diff, note = row
        line = f'{module} {addr} {result} {size or "-"}/{compiled or "-"}'
        if diff:
            line += f' @{diff}'
        if note:
            line += f' {note}'
        print(line, file=sys.stderr if result == 'tool-error' else sys.stdout)
        if result == 'tool-error':
            errors += 1

    out = Path(opts.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w', encoding='utf-8') as f:
        f.write('module\taddr\tsize\tcompiled\tresult\tdiff\n')
        for module, addr, size, compiled, result, diff, _note in rows:
            f.write(f'{module}\t{addr}\t{size}\t{compiled}\t{result}\t{diff}\n')

    counts = {}
    for row in rows:
        counts[row[4]] = counts.get(row[4], 0) + 1
    summary = ', '.join(f'{counts[k]} {k}' for k in ('exact', 'diff', 'length', 'tool-error')
                        if k in counts)
    print(f'{len(rows)} case(s): {summary}; ledger {out}')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
