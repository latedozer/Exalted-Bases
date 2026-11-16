#!/usr/bin/env python3
# Exalted Items: the Shard of Desecration, against an output made with the crystal alone.
#
#   check_shard.py <mod data dir> <output with the crystal only> <output with the shard too> <saved codes file or ""> [drop=<percent>] [downgrade=<code>]
import csv, json, os, struct, sys

MOD, ONLY, BOTH, CODES = sys.argv[1:5]
OPT = dict(a.split("=", 1) for a in sys.argv[5:])
DROP = int(OPT.get("drop", "100"))
failures = []
def fail(text): failures.append(text); print("FAIL: " + text)

def table(root, rel):
    path = os.path.join(root, rel)
    if not os.path.exists(path): return None
    rows = list(csv.reader(open(path, encoding="latin1", newline=""), delimiter="\t", quoting=csv.QUOTE_NONE))
    return {name.strip().lower(): i for i, name in enumerate(rows[0])}, [r for r in rows[1:]]
def cell(h, row, name):
    i = h.get(name, -1)
    return row[i].strip() if 0 <= i < len(row) else ""

crystal = shard = None
for bank in ("global/excel", "global/excel/base"):
    tag = "[%s] " % bank
    h, before = table(ONLY, bank + "/misc.txt")
    _, after = table(BOTH, bank + "/misc.txt")
    added = [r for r in after if cell(h, r, "name") == "[xlt] Shard of Desecration"]
    crystals = [r for r in after if cell(h, r, "name") == "[xlt] Restoration Crystal"]
    if len(added) != 1 or len(crystals) != 1 or len(after) != len(before) + 1 or [r for r in after if r not in added] != before:
        fail(tag + "misc.txt: expected the crystal-only rows and one shard row")
        continue
    s, c = added[0], crystals[0]
    crystal, shard = cell(h, c, "code"), cell(h, s, "code")
    differing = sorted(k for k in h if cell(h, s, k) != cell(h, c, k))
    if differing != sorted(["name", "code", "namestr"] + (["alternategfx"] if cell(h, c, "alternategfx") == crystal else [])) or cell(h, s, "namestr") != "xlt_desecration" or len(shard) != 3 or shard == crystal:
        fail(tag + "the shard's row is not the crystal's row with its own name, code and name string (differs in %s)" % differing)
    if cell(h, s, "advancedstashstackable") != "1":
        fail(tag + "the shard is not marked as a stacking material")
    # loot: beside the crystal, in the same rows, with the same weight
    th, trows = table(BOTH, bank + "/treasureclassex.txt")
    _, orows = table(ONLY, bank + "/treasureclassex.txt")
    with_shard = 0
    for now, was in zip(trows, orows):
        items = {cell(th, now, "item%d" % i): cell(th, now, "prob%d" % i) for i in range(1, 11) if cell(th, now, "item%d" % i)}
        if crystal in items and DROP > 0:
            want = max(1, (int(items[crystal]) * DROP + 50) // 100) if cell(th, now, "treasure class") else 0
            if shard not in items:
                fail(tag + "loot row '%s' has the crystal but not the shard" % cell(th, now, "treasure class"))
            elif DROP == 100 and items[shard] != items[crystal]:
                fail(tag + "loot row '%s': shard weight %s, crystal weight %s" % (cell(th, now, "treasure class"), items[shard], items[crystal]))
            with_shard += 1
            plain = [("" if v == shard else v) for v in now]
        elif shard in items:
            fail(tag + "loot row '%s' has the shard without the crystal" % cell(th, now, "treasure class"))
    rows_with_crystal = sum(1 for r in trows if crystal in [cell(th, r, "item%d" % i) for i in range(1, 11)])
    if with_shard != (rows_with_crystal if DROP > 0 else 0) or (DROP > 0 and rows_with_crystal != 43):
        fail(tag + "the shard is in %d loot rows, the crystal in %d" % (with_shard, rows_with_crystal))
    # the cube: back to elite takes the shard (or the material named)
    ch, crows = table(BOTH, bank + "/cubemain.txt")
    back = [r for r in crows if cell(ch, r, "output").strip('"') == "useitem,mod,eli" and cell(ch, r, "description").startswith("[xlt] ")]
    material = OPT.get("downgrade", shard)
    if not back or any(cell(ch, r, "input 2") != material or cell(ch, r, "numinputs") != "2" for r in back):
        fail(tag + "the back-to-elite recipes do not take '%s' (%d recipes)" % (material, len(back)))
    if bank == "global/excel":
        print(tag + "shard '%s' beside crystal '%s' in %d loot rows; %d back-to-elite recipes take %s" % (shard, crystal, with_shard, len(back), material))

if shard:
    names = json.load(open(os.path.join(BOTH, "local/lng/strings/item-names.json"), encoding="utf-8-sig"))
    mine = [e for e in names if e.get("Key") == "xlt_desecration"]
    if len(mine) != 1 or mine[0].get("enUS") != "ÿc8Shard of Desecration":
        fail("item-names.json: the shard's name is not 'Shard of Desecration' in orange")
    items = json.load(open(os.path.join(BOTH, "hd/items/items.json"), encoding="utf-8-sig"))
    entry = [e[shard] for e in items if shard in e]
    if len(entry) != 1 or entry[0].get("asset") != "shard/xlt_shard_of_desecration":
        fail("items.json: the shard has no picture of its own (%s)" % entry)
    for name, size in (("xlt_shard_of_desecration.sprite", 98), ("xlt_shard_of_desecration.lowend.sprite", 49)):
        path = os.path.join(BOTH, "hd/global/ui/items/misc/shard", name)
        src = os.path.join(os.path.dirname(os.path.dirname(MOD.rstrip("/"))), "")  # (unused)
        if not os.path.exists(path):
            fail("the shard's picture %s was not made" % name); continue
        d = open(path, "rb").read()
        w, hgt = struct.unpack_from("<II", d, 8)
        px = [d[40 + i * 4:44 + i * 4] for i in range(w * hgt)]
        seen = [p for p in px if p[3] != 0]
        red = sum(1 for p in seen if p[0] >= p[1] and p[0] >= p[2] and (p[0] < 24 or p[0] > 2 * max(p[1], p[2])))
        if (w, hgt) != (size, size) or not seen or red != len(seen):
            fail("picture %s: %dx%d, %d of %d visible pixels red" % (name, w, hgt, red, len(seen)))
        else:
            print("picture (%s): %dx%d, %d visible pixels, all red" % (name.split(".", 1)[1], w, hgt, len(seen)))
    if os.path.exists(os.path.join(BOTH, "hd/items/misc/shard/xlt_shard_of_desecration.json")) is False:
        fail("the shard's model description was not made")
    if CODES:
        saved = dict(l.strip().split("=", 1) for l in open(CODES) if "=" in l)
        if saved.get("*desecration") != shard or saved.get("*crystal") != crystal:
            fail("the saved codes do not hold the shard's and the crystal's code")
else:
    fail("no shard was made")
print("RESULT: " + ("ok" if not failures else "%d problem(s)" % len(failures)))
sys.exit(1 if failures else 0)
