#!/usr/bin/env python3
r"""Writes synthetic call lines (docs/lifting.md, "Synthetic calls") for one lifted entry.

usage: synth_args.py MODULE ENTRY TEMPLATE [--var NAME=SPEC ...] [--zip] [--limit N] [--after 'MOD ENTRY [n]']

TEMPLATE is the rest of a call line with {NAME} placeholders, e.g. 'words(1, {n})' or
'buf(64, {s}) {t} DS=DGROUP'. Each --var adds values to NAME (repeat it to take the union):

  edges16             0 1 -1 2 -2 100 -100 0x7FFE 0x7FFF -0x8000 -0x7FFF 0xFFFE
  edges32             long() values around 0, 0xFFFF/0x10000, 0x7FFFFFFF/0x80000000
  range:LO:HI[:STEP]  integers LO..HI inclusive
  rand:SEED:N:LO:HI   N random integers
  rand32:SEED:N       N random long() values
  list:A;B;C          the items as written
  str:A;B;C           string literals ("" for an empty item; \xHH, \t, \n and \\ are escapes)
  strfile:PATH        one string literal per line of PATH (no escapes are interpreted)
  randstr:SEED:N:MAXLEN[:ALPHABET]  N random strings of 0..MAXLEN characters

Values combine as a product over the variables, or in step with --zip (shorter lists repeat).
"""
import argparse
import itertools
import random
import sys

EDGES16 = ["0", "1", "-1", "2", "-2", "100", "-100", "0x7FFE", "0x7FFF", "-0x8000", "-0x7FFF", "0xFFFE"]
EDGES32 = [f"long({v})" for v in ("0", "1", "-1", "0xFFFF", "0x10000", "-0x10000", "0x7FFFFFFF",
                                  "-0x80000000", "0x12345678", "-0x12345678")]


def literal(text: str) -> str:
    out = []
    for ch in text:
        c = ord(ch)
        if ch in '"\\':
            out.append("\\" + ch)
        elif ch == "#":
            out.append("\\x23")  # keeps comment cutting simple for readers of the list
        elif 0x20 <= c < 0x7F:
            out.append(ch)
        else:
            out.append(f"\\x{c & 0xFF:02X}")
    return '"' + "".join(out) + '"'


def values(spec: str) -> list[str]:
    kind, _, rest = spec.partition(":")
    parts = rest.split(":") if rest else []
    if kind == "edges16":
        return list(EDGES16)
    if kind == "edges32":
        return list(EDGES32)
    if kind == "range":
        lo, hi = int(parts[0], 0), int(parts[1], 0)
        step = int(parts[2], 0) if len(parts) > 2 else 1
        return [str(v) for v in range(lo, hi + 1, step)]
    if kind == "rand":
        rng = random.Random(int(parts[0]))
        return [str(rng.randint(int(parts[2], 0), int(parts[3], 0))) for _ in range(int(parts[1]))]
    if kind == "rand32":
        rng = random.Random(int(parts[0]))
        return [f"long({rng.randint(-0x80000000, 0x7FFFFFFF)})" for _ in range(int(parts[1]))]
    if kind == "list":
        return rest.split(";")
    if kind == "str":
        return [literal(s.encode("latin-1").decode("unicode_escape")) for s in rest.split(";")]
    if kind == "strfile":
        with open(rest, encoding="latin-1") as f:
            return [literal(line.rstrip("\n")) for line in f]
    if kind == "randstr":
        rng = random.Random(int(parts[0]))
        alphabet = ":".join(parts[3:]) if len(parts) > 3 else "abcAB z.-"
        return [literal("".join(rng.choice(alphabet) for _ in range(rng.randint(0, int(parts[2])))))
                for _ in range(int(parts[1]))]
    sys.exit(f"synth_args: unknown value spec '{spec}'")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("module")
    ap.add_argument("entry")
    ap.add_argument("template")
    ap.add_argument("--var", action="append", default=[], metavar="NAME=SPEC")
    ap.add_argument("--zip", action="store_true", help="take the variables in step instead of the product")
    ap.add_argument("--limit", type=int, default=0, help="at most this many calls")
    ap.add_argument("--after", help="print an 'after' line first, e.g. 'LSCITV.EXE lsci_kGetEvent 20'")
    args = ap.parse_args()

    names: dict[str, list[str]] = {}
    for item in args.var:
        name, _, spec = item.partition("=")
        names.setdefault(name, []).extend(values(spec))
    keys = list(names)
    if args.zip and keys:
        longest = max(len(names[k]) for k in keys)
        rows = [tuple(names[k][i % len(names[k])] for k in keys) for i in range(longest)]
    else:
        rows = list(itertools.product(*(names[k] for k in keys)))
    if args.limit:
        rows = rows[: args.limit]
    if args.after:
        print(f"after {args.after}")
    for row in rows:
        print(f"call {args.module} {args.entry} {args.template.format(**dict(zip(keys, row)))}".rstrip())


if __name__ == "__main__":
    main()
