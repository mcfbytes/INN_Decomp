#!/usr/bin/env python3
"""Rewrite constant call sites so they name the lifted entry.

usage: lift_callsites.py <lifted_dir> [--apply] [--report FILE] [--dirs D1,D2,...]

Without --apply the tree is not changed. With --apply, each rewritten file is
edited in place and gains #include "lift_calls.h" after its last #include that
still precedes the first rewrite.

A call is rewritten only when its segment and offset are integer-literal enum
constants (or a literal) and that address is one lifted entry of the same
near/far kind. Region lifts and overlay entries are not targets: an overlay
has no fixed address. The calls left over are reported.

  lsci_call(m, S, O)                         -> LCALLF0
  lsci_callf(m, S, O, N, a...)               -> LCALLF / LCALLF0 (N must match)
  LSCI_CDECL(m, S, O, a...)                  -> LCALLF / LCALLF0
  lsci_calln(m, O, N, a...)                  -> LCALLN / LCALLN0
  guest_call_near(m, O)                      -> LCALLN0
  TSN_CALL_NEAR(m, O, a...)                  -> LCALLN / LCALLN0 at segment 0

Near calls other than TSN_CALL_NEAR use the segment shared by every LIFTED
marker in the file. A file with none uses the directory's segment when the
directory has only one. TSN_CALL_NEAR is segment 0 of TSNEXEC.EXE.

Constants are enumerators written `NAME = <C integer literal>`. Enumerators
in the file win over the headers it includes. The same name with two different
values in those headers, or an enumerator with no literal, is unknown, and so
is any other expression. A near call in a file whose markers disagree on the
segment is not rewritten.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_lift_table  # noqa: E402

INN_DIRS = (
    'TSNEXEC', 'LSCITV', 'LSCITV-ev', 'LSCITV-gfx', 'LSCITV-mem', 'LSCITV-vm',
    'GRAPH256', 'NLNULL', 'VGA320', 'MODEM', 'NOBRK', 'IBMKBD', 'ADL', 'SNDBLAST',
)

PATTERNS = ('lsci_callf', 'lsci_calln', 'lsci_call', 'LSCI_CDECL', 'guest_call_near', 'TSN_CALL_NEAR')
FAR = {'lsci_call', 'lsci_callf', 'LSCI_CDECL'}
NEAR = {'lsci_calln', 'guest_call_near', 'TSN_CALL_NEAR'}

# C integer literal: hex, octal (leading 0), decimal, with an optional suffix.
LITERAL = re.compile(
    r'(0[xX][0-9A-Fa-f]+|0[0-7]*|[1-9][0-9]*)'
    r'(?:[uU](?:ll|LL|[lL])?|(?:ll|LL|[lL])[uU]?)?\Z')
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
IDENT_RE = re.compile(r'[A-Za-z_]\w*\Z')


class ParseError(Exception):
    def __init__(self, line, message):
        self.line = line
        self.message = message


def line_of(text, index):
    return text.count('\n', 0, index) + 1


def is_ident_start(c):
    return c.isalpha() or c == '_'


def is_ident(c):
    return c.isalnum() or c == '_'


def skip_string(text, i):
    """i points at the opening quote. Returns the index after the literal."""
    quote = text[i]
    i += 1
    n = len(text)
    while i < n:
        if text[i] == '\\':
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        i += 1
    return n


def skip_ws(text, i):
    """Skip whitespace, comments and backslash-newline splices."""
    n = len(text)
    while i < n:
        if text[i] in ' \t\r\n':
            i += 1
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if text[i] == '\\' and i + 1 < n and text[i + 1] == '\n':
            i += 2
            continue
        break
    return i


def read_ident(text, i):
    if i >= len(text) or not is_ident_start(text[i]):
        return None, i
    j = i + 1
    while j < len(text) and is_ident(text[j]):
        j += 1
    return text[i:j], j


def parse_args(text, i):
    """i points at '('. Returns (args, index after the closing paren)."""
    if i >= len(text) or text[i] != '(':
        raise ParseError(line_of(text, i), 'expected (')
    i += 1
    n = len(text)
    args = []
    start = i
    paren = 1
    brace = 0
    bracket = 0
    while i < n:
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            if j < 0:
                raise ParseError(line_of(text, i), 'unclosed comment')
            i = j + 2
            continue
        c = text[i]
        if c in '"\'':
            i = skip_string(text, i)
            continue
        if c == '\\' and i + 1 < n and text[i + 1] == '\n':
            i += 2
            continue
        if c == '(':
            paren += 1
        elif c == ')':
            paren -= 1
            if paren == 0:
                args.append(text[start:i])
                return args, i + 1
        elif c == '{':
            brace += 1
        elif c == '}':
            brace = max(0, brace - 1)
        elif c == '[':
            bracket += 1
        elif c == ']':
            bracket = max(0, bracket - 1)
        elif c == ',' and paren == 1 and brace == 0 and bracket == 0:
            args.append(text[start:i])
            i += 1
            start = i
            continue
        i += 1
    raise ParseError(line_of(text, start), 'unclosed call')


def iter_calls(text):
    """Yield (name, args, start, end) for each pattern call. end is after ')'."""
    i = 0
    n = len(text)
    while i < n:
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        c = text[i]
        if c in '"\'':
            i = skip_string(text, i)
            continue
        if c == '\\' and i + 1 < n and text[i + 1] == '\n':
            i += 2
            continue
        if is_ident_start(c):
            j = i + 1
            while j < n and is_ident(text[j]):
                j += 1
            name = text[i:j]
            if name in PATTERNS:
                k = skip_ws(text, j)
                if k < n and text[k] == '(':
                    args, end = parse_args(text, k)
                    yield name, args, i, end
                    i = end
                    continue
            i = j
            continue
        i += 1


def compact(expr):
    """Drop comments and whitespace outside strings, for constant matching."""
    out = []
    i = 0
    n = len(expr)
    while i < n:
        if expr.startswith('//', i):
            j = expr.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if expr.startswith('/*', i):
            j = expr.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        c = expr[i]
        if c in '"\'':
            j = skip_string(expr, i)
            out.append(expr[i:j])
            i = j
            continue
        if c == '\\' and i + 1 < n and expr[i + 1] == '\n':
            i += 2
            continue
        if c in ' \t\r\n':
            i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def squash(expr):
    """One-line form of an argument, comments removed, strings kept."""
    out = []
    i = 0
    n = len(expr)
    spaced = False
    while i < n:
        if expr.startswith('//', i):
            j = expr.find('\n', i)
            i = n if j < 0 else j + 1
            spaced = True
            continue
        if expr.startswith('/*', i):
            j = expr.find('*/', i + 2)
            i = n if j < 0 else j + 2
            spaced = True
            continue
        c = expr[i]
        if c in '"\'':
            if spaced and out:
                out.append(' ')
            spaced = False
            j = skip_string(expr, i)
            out.append(expr[i:j])
            i = j
            continue
        if c == '\\' and i + 1 < n and expr[i + 1] == '\n':
            i += 2
            spaced = True
            continue
        if c in ' \t\r\n':
            spaced = True
            i += 1
            continue
        if spaced and out:
            out.append(' ')
        spaced = False
        out.append(c)
        i += 1
    return ''.join(out).strip()


def wrapped(expr):
    """True when expr is one parenthesized group. Parens inside strings do not count."""
    if len(expr) < 2 or expr[0] != '(' or expr[-1] != ')':
        return False
    depth = 0
    i = 0
    n = len(expr)
    while i < n:
        c = expr[i]
        if c in '"\'':
            i = skip_string(expr, i)
            continue
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i == n - 1
        i += 1
    return False


def int_literal(expr):
    match = LITERAL.fullmatch(expr)
    if not match:
        return None
    num = match.group(1)
    if num[0] == '0' and len(num) > 1 and num[1] in 'xX':
        return int(num, 16)
    if num[0] == '0':
        return int(num, 8)
    return int(num, 10)


def word_value(expr, consts, blocked):
    """A 16-bit constant, or None when the expression is not one."""
    text = compact(expr)
    while wrapped(text):
        text = text[1:-1]
    if not text or text in blocked:
        return None
    number = int_literal(text)
    if number is None and IDENT_RE.fullmatch(text) and text in consts and text not in blocked:
        number = consts[text]
    if number is None or number < 0 or number > 0xFFFF:
        return None
    return number


def unprotected_comma(expr):
    """A comma the preprocessor would treat as a macro-argument separator."""
    depth = 0
    i = 0
    n = len(expr)
    while i < n:
        if expr.startswith('//', i) or expr.startswith('/*', i):
            # squash already removed comments from replacement text
            pass
        c = expr[i]
        if c in '"\'':
            i = skip_string(expr, i)
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth = max(0, depth - 1)
        elif c == ',' and depth == 0:
            return True
        i += 1
    return False


def shield(expr):
    if expr == '__VA_ARGS__':
        return expr
    if unprotected_comma(expr):
        return f'({expr})'
    return expr


def scan_enums(text):
    """Literal enumerators, and names that are enumerators without a usable literal.

    A name in the second set hides a constant from an included header: the file
    defines it, and its value is not a literal this tool can use.
    """
    values = {}
    hidden = set()
    i = 0
    n = len(text)

    def note_literal(name, value):
        if name in hidden:
            return
        if name in values and values[name] != value:
            hidden.add(name)
            values.pop(name, None)
        else:
            values[name] = value

    def note_hidden(name):
        hidden.add(name)
        values.pop(name, None)

    while i < n:
        i = skip_ws(text, i)
        if i >= n:
            break
        if text[i] in '"\'':
            i = skip_string(text, i)
            continue
        name, j = read_ident(text, i)
        if name != 'enum':
            i = j if name else i + 1
            continue
        k = skip_ws(text, j)
        tag, k2 = read_ident(text, k)
        if tag:
            k = skip_ws(text, k2)
        if k >= n or text[k] != '{':
            i = j
            continue
        i = k + 1
        while True:
            i = skip_ws(text, i)
            if i >= n:
                raise ParseError(line_of(text, k), 'unclosed enum')
            if text[i] == '}':
                i += 1
                break
            enum_name, i = read_ident(text, i)
            if not enum_name:
                raise ParseError(line_of(text, i), 'enumerator expected')
            i = skip_ws(text, i)
            if i < n and text[i] == '=':
                i += 1
                start = i
                depth_p = depth_b = depth_k = 0
                while i < n:
                    if text.startswith('//', i):
                        nl = text.find('\n', i)
                        i = n if nl < 0 else nl + 1
                        continue
                    if text.startswith('/*', i):
                        end = text.find('*/', i + 2)
                        if end < 0:
                            raise ParseError(line_of(text, i), 'unclosed comment')
                        i = end + 2
                        continue
                    c = text[i]
                    if c in '"\'':
                        i = skip_string(text, i)
                        continue
                    if c == '(':
                        depth_p += 1
                    elif c == ')':
                        depth_p = max(0, depth_p - 1)
                    elif c == '{':
                        depth_b += 1
                    elif c == '}':
                        if depth_p == 0 and depth_b == 0 and depth_k == 0:
                            break
                        depth_b = max(0, depth_b - 1)
                    elif c == '[':
                        depth_k += 1
                    elif c == ']':
                        depth_k = max(0, depth_k - 1)
                    elif c == ',' and depth_p == 0 and depth_b == 0 and depth_k == 0:
                        break
                    i += 1
                number = int_literal(compact(text[start:i]))
                if number is None:
                    note_hidden(enum_name)
                else:
                    note_literal(enum_name, number)
                if i < n and text[i] == ',':
                    i += 1
                continue
            note_hidden(enum_name)
            if i < n and text[i] == ',':
                i += 1
    return values, hidden


def resolve_include(src, quoted, module_dir, lifted_dir):
    for candidate in (src.parent / quoted, module_dir / quoted, lifted_dir / 'include' / quoted):
        if candidate.is_file():
            return candidate.resolve()
    return None


def include_closure(start, module_dir, lifted_dir, cache):
    """Headers included (transitively) from start, not including start itself."""
    seen = set()
    stack = [start.resolve()]
    found = []
    while stack:
        path = stack.pop()
        if path in seen:
            continue
        seen.add(path)
        text = cache.setdefault(path, path.read_text(errors='replace'))
        for match in INCLUDE_RE.finditer(text):
            header = resolve_include(path, match.group(1), module_dir, lifted_dir)
            if header is not None and header not in seen:
                stack.append(header)
                found.append(header)
    return found


def constants_for(path, module_dir, lifted_dir, cache, enum_cache):
    """Constants visible in path: included headers, then the file's own enumerators."""
    merged = {}

    def absorb(values, hidden, overrule):
        for name in hidden:
            merged[name] = None
        for name, value in values.items():
            if not overrule and merged.get(name, value) != value:
                merged[name] = None
                continue
            if not overrule and name in merged and merged[name] is None:
                continue
            merged[name] = value

    for header in include_closure(path, module_dir, lifted_dir, cache):
        if header not in enum_cache:
            enum_cache[header] = scan_enums(cache.setdefault(header, header.read_text(errors='replace')))
        absorb(*enum_cache[header], overrule=False)
    if path.resolve() not in enum_cache:
        text = cache.setdefault(path.resolve(), path.read_text(errors='replace'))
        enum_cache[path.resolve()] = scan_enums(text)
    absorb(*enum_cache[path.resolve()], overrule=True)
    consts = {name: value for name, value in merged.items() if value is not None}
    blocked = {name for name, value in merged.items() if value is None}
    return consts, blocked


def build_index(lifted_dir):
    """(module, seg, off, near) -> {function names}. Regions and overlays are omitted."""
    index = {}
    for module_dir in sorted(p for p in lifted_dir.iterdir() if (p / 'module.txt').is_file()):
        name, image, relocs, overlays = gen_lift_table.read_module(module_dir)
        for entry in gen_lift_table.entries_of(module_dir, name, image, relocs, overlays):
            module, seg, off, fn, near = entry[0], entry[1], entry[2], entry[3], entry[4]
            if entry[8] or '#' in module:
                continue
            index.setdefault((module, seg, off, bool(near)), set()).add(fn)
    return index


def marker_segments(text):
    return [int(match.group(1), 16) for match in gen_lift_table.MARKER.finditer(text)]


def directory_segment(module_dir, cache):
    segs = set()
    for path in list(module_dir.glob('*.c')) + list(module_dir.glob('*.h')):
        text = cache.setdefault(path.resolve(), path.read_text(errors='replace'))
        segs.update(marker_segments(text))
    if len(segs) == 1:
        return next(iter(segs))
    return None


def file_segment(text, module_dir, cache):
    segs = set(marker_segments(text))
    if len(segs) == 1:
        return next(iter(segs)), None
    if len(segs) > 1:
        return None, 'markers in this file use more than one segment'
    seg = directory_segment(module_dir, cache)
    if seg is None:
        return None, 'no segment for a near call'
    return seg, None


def short(expr, limit=60):
    text = squash(expr)
    if len(text) > limit:
        return text[:limit - 3] + '...'
    return text


def classify(name, args, consts, blocked, module, file_seg, seg_reason):
    """Return ('rewrite', macro_text, seg, off) or ('remain', seg, off) or ('skip', reason)."""
    if name in FAR:
        if len(args) < 3:
            return ('skip', 'too few arguments')
        seg = word_value(args[1], consts, blocked)
        off = word_value(args[2], consts, blocked)
        if seg is None:
            return ('skip', f'segment is not a constant ({short(args[1])})')
        if off is None:
            return ('skip', f'offset is not a constant ({short(args[2])})')
        if name == 'lsci_call' and len(args) != 3:
            return ('skip', f'{len(args)} arguments')
        extra = []
        if name == 'lsci_callf':
            if len(args) < 4:
                return ('skip', 'no argument count')
            count = word_value(args[3], consts, blocked)
            if count is None:
                return ('skip', f'argument count is not a constant ({short(args[3])})')
            extra = args[4:]
            if len(extra) != count:
                return ('skip', f'argument count {count} does not match {len(extra)} arguments')
        elif name == 'LSCI_CDECL':
            extra = args[3:]
        near = False
        machine = args[0]
    elif name == 'TSN_CALL_NEAR':
        if module != 'TSNEXEC.EXE':
            return ('skip', 'TSN_CALL_NEAR is only rewritten in TSNEXEC')
        if len(args) < 2:
            return ('skip', 'too few arguments')
        off = word_value(args[1], consts, blocked)
        if off is None:
            return ('skip', f'offset is not a constant ({short(args[1])})')
        seg = 0
        near = True
        extra = args[2:]
        machine = args[0]
    else:
        if seg_reason:
            return ('skip', seg_reason)
        if name == 'guest_call_near':
            if len(args) != 2:
                return ('skip', f'{len(args)} arguments')
            off = word_value(args[1], consts, blocked)
            extra = []
        else:
            if len(args) < 3:
                return ('skip', 'too few arguments')
            off = word_value(args[1], consts, blocked)
            count = word_value(args[2], consts, blocked)
            if off is None:
                return ('skip', f'offset is not a constant ({short(args[1])})')
            if count is None:
                return ('skip', f'argument count is not a constant ({short(args[2])})')
            extra = args[3:]
            if len(extra) != count:
                return ('skip', f'argument count {count} does not match {len(extra)} arguments')
        if off is None:
            return ('skip', f'offset is not a constant ({short(args[1])})')
        seg = file_seg
        near = True
        machine = args[0]
    if any(squash(arg) == '' for arg in [machine, *extra]):
        return ('skip', 'empty argument')
    hits = index_lookup(module, seg, off, near)
    if hits is None:
        return ('skip', 'internal')
    names, others = hits
    if len(names) == 1:
        fn = next(iter(names))
        macro = render(name, machine, fn, extra)
        return ('rewrite', macro, seg, off, fn)
    if len(names) > 1:
        return ('skip', 'several lifted entries at ' + f'{seg:04X}:{off:04X}')
    if others:
        kind = 'near' if near else 'far'
        return ('skip', f"lifted entry {', '.join(sorted(others))} is not a {kind} entry")
    return ('remain', seg, off)


# Set by process(); classify reads it. Kept as a parameter would thread it through every call.
_INDEX = {}


def index_lookup(module, seg, off, near):
    names = _INDEX.get((module, seg, off, near), set())
    others = _INDEX.get((module, seg, off, not near), set())
    return names, others


def render(pattern, machine, fn, extra):
    args = [shield(squash(arg)) for arg in extra]
    m = shield(squash(machine))
    if pattern in ('lsci_call', 'guest_call_near') or not args:
        macro = 'LCALLN0' if pattern in NEAR else 'LCALLF0'
        return f'{macro}({m}, {fn})'
    macro = 'LCALLN' if pattern in NEAR else 'LCALLF'
    return f"{macro}({m}, {fn}, {', '.join(args)})"


def last_include_end(text):
    """Index just after the newline of the last #include that is not in a comment."""
    in_block = False
    last = None
    offset = 0
    for raw in text.splitlines(keepends=True):
        i = 0
        visible = []
        while i < len(raw):
            if in_block:
                end = raw.find('*/', i)
                if end < 0:
                    i = len(raw)
                    break
                in_block = False
                i = end + 2
                continue
            if raw.startswith('//', i):
                break
            if raw.startswith('/*', i):
                in_block = True
                i += 2
                continue
            if raw[i] in '"\'':
                j = skip_string(raw, i)
                i = j
                continue
            visible.append(raw[i])
            i += 1
        if re.match(r'\s*#\s*include\b', ''.join(visible)):
            last = offset + len(raw)
        offset += len(raw)
    return last


def insert_include(text, first_edit):
    if re.search(r'(?m)^\s*#\s*include\s+"lift_calls.h"', text):
        return text
    nl = '\r\n' if '\r\n' in text else '\n'
    point = last_include_end(text)
    if point is None:
        point = 0
    if first_edit < point:
        point = text.rfind('\n', 0, first_edit) + 1
    line = f'#include "lift_calls.h"{nl}'
    if point > 0 and text[point - 1] != '\n':
        line = nl + line
    return text[:point] + line + text[point:]


def source_files(module_dir):
    return sorted(list(module_dir.glob('*.c')) + list(module_dir.glob('*.h')))


def process_file(path, module_dir, lifted_dir, module, cache, enum_cache):
    text = cache.setdefault(path.resolve(), path.read_text(errors='replace'))
    consts, blocked = constants_for(path, module_dir, lifted_dir, cache, enum_cache)
    file_seg, seg_reason = file_segment(text, module_dir, cache)
    rel = path.relative_to(lifted_dir).as_posix()
    rewrites = []
    remaining = []
    skipped = []
    for name, args, start, end in iter_calls(text):
        line = line_of(text, start)
        try:
            result = classify(name, args, consts, blocked, module, file_seg, seg_reason)
        except ParseError as exc:
            raise ParseError(exc.line, f'{rel}: {exc.message}') from exc
        kind = result[0]
        if kind == 'rewrite':
            _, macro, seg, off, fn = result
            rewrites.append((start, end, macro, line, name, seg, off, fn))
        elif kind == 'remain':
            _, seg, off = result
            remaining.append((line, seg, off, name))
        else:
            skipped.append((line, name, result[1]))
    return text, rewrites, remaining, skipped, rel


def format_report(per_file, rewrites, remaining, skipped, dirs):
    lines = [
        f'rewritten: {len(rewrites)}',
        f'remaining: {len(remaining)}',
        f'unevaluated: {len(skipped)}',
        f'directories: {", ".join(dirs)}',
        '',
        '# rewritten per file',
    ]
    for rel, count in per_file:
        lines.append(f'{rel}: {count} rewritten')
    lines.append('')
    lines.append('# rewrites')
    for rel, line, name, seg, off, fn, macro in rewrites:
        lines.append(f'{rel}:{line} {name} {seg:04X}:{off:04X} {fn} -> {macro}')
    lines.append('')
    lines.append('# remaining (calls into original code)')
    for rel, line, module, seg, off, name in remaining:
        lines.append(f'{rel}:{line} {module} {seg:04X}:{off:04X} {name}')
    lines.append('')
    lines.append('# unevaluated')
    for rel, line, name, reason in skipped:
        lines.append(f'{rel}:{line} {name} {reason}')
    lines.append('')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    parser.add_argument('lifted_dir', type=Path)
    parser.add_argument('--apply', action='store_true', help='edit files in place')
    parser.add_argument('--report', type=Path, help='write the full report here')
    parser.add_argument('--dirs', help='comma-separated directory names (default: the INN tree)')
    args = parser.parse_args()
    lifted_dir = args.lifted_dir.resolve()
    if not lifted_dir.is_dir():
        sys.exit(f'not a directory: {lifted_dir}')
    names = INN_DIRS if not args.dirs else tuple(part for part in args.dirs.split(',') if part)
    if not names:
        sys.exit('no directories')
    dirs = []
    for name in names:
        path = lifted_dir / name
        if not (path / 'module.txt').is_file():
            sys.exit(f'no module.txt in {path}')
        dirs.append(path)

    global _INDEX
    _INDEX = build_index(lifted_dir)
    cache = {}
    enum_cache = {}
    per_file = []
    rewrites = []
    remaining = []
    skipped = []
    edited = 0
    for module_dir in dirs:
        module = (module_dir / 'module.txt').read_text().splitlines()[0].strip()
        for path in source_files(module_dir):
            text, file_rewrites, file_remaining, file_skipped, rel = process_file(
                path, module_dir, lifted_dir, module, cache, enum_cache)
            if file_rewrites:
                per_file.append((rel, len(file_rewrites)))
            for start, end, macro, line, name, seg, off, fn in file_rewrites:
                rewrites.append((rel, line, name, seg, off, fn, macro))
            for line, seg, off, name in file_remaining:
                remaining.append((rel, line, module, seg, off, name))
            for line, name, reason in file_skipped:
                skipped.append((rel, line, name, reason))
            if args.apply and file_rewrites:
                new = text
                for start, end, macro, *_ in sorted(file_rewrites, key=lambda item: item[0], reverse=True):
                    new = new[:start] + macro + new[end:]
                new = insert_include(new, min(item[0] for item in file_rewrites))
                if new != text:
                    path.write_text(new)
                    cache[path.resolve()] = new
                    edited += 1
    report = format_report(per_file, rewrites, remaining, skipped, [path.name for path in dirs])
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(report)
    print(f'rewritten: {len(rewrites)}')
    print(f'remaining: {len(remaining)}')
    print(f'unevaluated: {len(skipped)}')
    if args.apply:
        print(f'edited files: {edited}')
    if args.report:
        print(f'report: {args.report}')
    elif not args.apply:
        sys.stdout.write(report)


if __name__ == '__main__':
    try:
        main()
    except ParseError as exc:
        sys.exit(f'{exc.line}: {exc.message}' if exc.line else exc.message)
