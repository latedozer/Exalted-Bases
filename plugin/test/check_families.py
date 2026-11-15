#!/usr/bin/env python3
"""Checks the family lists the generator hands to the picker hooks against the tables.

    check_families.py <data dir> <families file> <codes file> [uniques mask] [sets mask]

The lists are worked out here a second time, straight from the table files, by the rule the
game's own pickers follow (read from the game's code): a unique or set row can be picked only
when its "spawnable" flag is set and its "disabled" flag is not; a flag is set by a cell that
holds a number other than 0, so a blank cell means "no". Every row the generator lists must be
one the game can pick, and every row the game can pick must be listed.

<families file> and <codes file> are what gen_test writes with families=... and savecodes=...
Masks: 1 normal, 2 exceptional, 4 elite (default 7).
"""
import os
import sys


def atoi(text):
    text = text.lstrip(" \t\n\v\f\r")
    sign = 1
    if text[:1] in ("+", "-"):
        sign = -1 if text[0] == "-" else 1
        text = text[1:]
    digits = ""
    for ch in text:
        if not ch.isdigit():
            break
        digits += ch
    return sign * int(digits) if digits else 0


def find(root, rel):
    """A path below root, whatever the letter case of its parts."""
    current = root
    for part in rel.split("/"):
        match = None
        if os.path.isdir(current):
            for name in os.listdir(current):
                if name.lower() == part.lower():
                    match = name
                    break
        if match is None:
            return None
        current = os.path.join(current, match)
    return current


def load(root, rel):
    path = find(root, rel)
    if path is None:
        return None, []
    lines = open(path, "rb").read().decode("latin-1").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    header = [name.lower() for name in lines[0].rstrip("\r").split("\t")]
    rows = []
    for line in lines[1:]:
        cells = line.split("\t")
        if cells:
            cells[-1] = cells[-1].rstrip("\r") if cells[-1].endswith("\r") else cells[-1]
        rows.append(cells)
    return header, rows


def cell(header, row, name):
    if header is None or name not in header:
        return None
    at = header.index(name)
    return row[at] if at < len(row) else ""


def main():
    data, families_file, codes_file = sys.argv[1:4]
    masks = {"unique": int(sys.argv[4]) if len(sys.argv) > 4 else 7, "set": int(sys.argv[5]) if len(sys.argv) > 5 else 7}
    codes = {}
    for line in open(codes_file):
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            elite, exalted = line.split("=", 1)
            codes[elite] = exalted

    expected = set()
    never = 0  # rows on a family's base that the game can never pick
    for bank, folder in ((0, "global/excel"), (1, "global/excel/base")):
        target = {}  # base code -> (exalted code, tier bit)
        usable = True
        for name in ("weapons.txt", "armor.txt"):
            header, rows = load(data, folder + "/" + name)
            if header is None:
                usable = False
                continue
            for row in rows:
                code, ultra, uber = cell(header, row, "code"), cell(header, row, "ultracode"), cell(header, row, "ubercode")
                if not code or ultra not in codes or (cell(header, row, "name") or "").startswith("[xlt] "):
                    continue
                tier = 4 if code == ultra else 2 if code == uber else 1
                target.setdefault(code, (codes[ultra], tier))
        if not usable:
            continue
        for kind, name, code_column in (("unique", "uniqueitems.txt", "code"), ("set", "setitems.txt", "item")):
            header, rows = load(data, folder + "/" + name)
            if header is None or masks[kind] == 0:
                continue
            listed = {}  # (exalted, base) -> [tier, rows]
            order = []
            for row in rows:
                base = cell(header, row, code_column)
                if base not in target or not cell(header, row, "index"):
                    continue
                exalted, tier = target[base]
                if not masks[kind] & tier:
                    continue
                disabled = atoi(cell(header, row, "disabled") or "")
                spawnable = cell(header, row, "spawnable")
                if spawnable is None:
                    spawnable = cell(header, row, "enabled")  # the older name of the column
                if disabled != 0 or (spawnable is not None and atoi(spawnable) == 0):
                    never += 1
                    continue
                key = (exalted, base)
                if key not in listed:
                    listed[key] = [tier, []]
                    order.append(key)
                listed[key][1].append("%d:%d" % (atoi(cell(header, row, "lvl") or ""), max(1, atoi(cell(header, row, "rarity") or ""))))
            for key in order:
                expected.add("%d %s %s %s %d %s" % (bank, kind, key[0], key[1], listed[key][0], " ".join(listed[key][1])))

    got = set(line.rstrip("\n") for line in open(families_file) if line.strip())
    missing, extra = sorted(expected - got), sorted(got - expected)
    for line in missing[:8]:
        print("  FAIL: the game can pick these rows, the generator does not list them: " + line)
    for line in extra[:8]:
        print("  FAIL: the generator lists rows the game cannot pick (or lists them differently): " + line)
    bases = len(expected)
    rows = sum(len(line.split()) - 5 for line in expected)
    ok = not missing and not extra and bases > 0
    print("  family lists: %d bases with %d rows the game can pick, all listed and nothing else (%d rows on those bases can never drop and are left out) -> %s"
          % (bases, rows, never, "ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
