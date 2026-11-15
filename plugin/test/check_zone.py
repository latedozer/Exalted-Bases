#!/usr/bin/env python3
# Checks the "only in terror zones" setting: check_zone.py <output without it> <output with it>
# (two runs of gen_test on the same mod with the same settings otherwise).
#
#  * armor/weapons: the only cells that differ are the DropConditionCalc cells of the exalted
#    rows, and each of those is the game's terror zone condition.
#  * treasureclassex: the only differences are that a level table above the game's own is named
#    through a row carrying that condition, and those rows themselves. A row that names such a
#    row always has something else to give (outside a terror zone the game leaves them out).
#  * every other adjusted file is the same.
import os, re, sys

COND = "cond('Desecrated')"
MARK = "[xlt] "
off, on = sys.argv[1], sys.argv[2]
fails = 0


def fail(text):
    global fails
    fails += 1
    if fails <= 20:
        print("  FAIL:", text)


def files(root):
    found = {}
    for folder, _, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            found[os.path.relpath(path, root).replace(os.sep, "/")] = path
    return found


def table(path):
    lines = open(path, "rb").read().decode("latin-1").split("\n")
    rows = [line.rstrip("\r").split("\t") for line in lines]
    while rows and rows[-1] == [""]:
        rows.pop()
    return rows[0], rows[1:]


a, b = files(off), files(on)
if set(a) != set(b):
    fail("different files written: %s" % sorted(set(a) ^ set(b)))
cells = zone_rows = wrapped = 0
for rel in sorted(set(a) & set(b)):
    name = rel.rsplit("/", 1)[-1]
    if name in ("armor.txt", "weapons.txt"):
        (ha, ra), (hb, rb) = table(a[rel]), table(b[rel])
        if [h.lower() for h in hb[: len(ha)]] != [h.lower() for h in ha] or len(ra) != len(rb):
            fail("%s: columns or row count differ" % rel)
            continue
        col = [h.lower() for h in hb].index("dropconditioncalc")
        ncol = [h.lower() for h in hb].index("name")
        for x, y in zip(ra, rb):
            exalted = len(y) > ncol and y[ncol].startswith(MARK)
            for i in range(max(len(x), len(y))):
                cx = x[i] if i < len(x) else ""
                cy = y[i] if i < len(y) else ""
                if i == col and exalted:
                    want = COND if not cx.strip() else "(" + cx.strip() + ")*" + COND
                    if cy != want:
                        fail("%s: %s has condition %r" % (rel, y[ncol], cy))
                    cells += 1
                elif cx != cy:
                    fail("%s: row %s column %s changed" % (rel, y[ncol] if len(y) > ncol else "?", hb[i] if i < len(hb) else i))
    elif name == "treasureclassex.txt":
        (ha, ra), (hb, rb) = table(a[rel]), table(b[rel])
        low = [h.lower() for h in hb]
        cc, picks = low.index("conditioncalc"), low.index("picks")
        items = [low.index("item%d" % i) for i in range(1, 11)]
        probs = [low.index("prob%d" % i) for i in range(1, 11)]
        zone = {}
        kept, first_zone, first_user = [], None, None
        for at, y in enumerate(rb):
            m = re.fullmatch(r"xlt ([a-z]+\d+) terror zone", y[0])
            if not m:
                kept.append(y)
                continue
            filled = [(y[i], y[p]) for i, p in zip(items, probs) if i < len(y) and y[i]]
            if y[cc] != COND or y[picks] != "1" or filled != [(m.group(1), "1")] or any(y[c] for c in (low.index("group"), low.index("level"))):
                fail("%s: unexpected terror zone row %r" % (rel, y[:12]))
            zone[y[0]] = m.group(1)
            zone_rows += 1
            first_zone = at if first_zone is None else first_zone
        for at, y in enumerate(rb):
            if first_user is None and y[0] not in zone and any(i < len(y) and y[i] in zone for i in items):
                first_user = at
        if zone and (first_user is None or first_user < first_zone + len(zone)):
            fail("%s: a terror zone row comes after a row that names it" % rel)
        if len(kept) != len(ra):
            fail("%s: row counts do not add up" % rel)
        rb = kept
        for x, y in zip(ra, rb):
            if len(x) <= 1 and len(y) <= 1:
                continue
            names_zone, other = 0, 0
            for i in range(max(len(x), len(y))):
                cx = x[i] if i < len(x) else ""
                cy = y[i] if i < len(y) else ""
                if i in items and cy in zone:
                    names_zone += 1
                    wrapped += 1
                    if cx != zone[cy]:
                        fail("%s: %s names %s where it named %s" % (rel, y[0], cy, cx))
                    if not y[0].startswith("xlt "):
                        fail("%s: a row of the game, %s, was pointed at a terror zone row" % (rel, y[0]))
                elif cx != cy:
                    fail("%s: row %s column %s changed" % (rel, y[0], hb[i] if i < len(hb) else i))
                elif i in items and cy:
                    other += 1
            if names_zone and not other:
                fail("%s: %s gives nothing outside a terror zone" % (rel, y[0]))
        # No table that only a terror zone row should open is named directly.
        tables = set(zone.values())
        for y in rb:
            if len(y) > 1 and y[0] not in zone:
                for i in items:
                    if i < len(y) and y[i].strip('"').split(",")[0] in tables:
                        fail("%s: %s names %s directly" % (rel, y[0], y[i]))
    elif open(a[rel], "rb").read() != open(b[rel], "rb").read():
        fail("%s differs" % rel)
print("  %d exalted rows carry the terror zone condition; %d terror zone rows, named %d times" % (cells, zone_rows, wrapped))
print("  RESULT:", "ok" if fails == 0 else "%d problems" % fails)
sys.exit(1 if fails else 0)
