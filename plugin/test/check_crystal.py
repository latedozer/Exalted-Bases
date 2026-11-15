#!/usr/bin/env python3
"""Checks the Restoration Crystal part of the generator's output.

  check_crystal.py <mod data dir> <vanilla data dir> <output without crystal> <output with crystal>
                   [drop=<percent>] [off=<kind.attribute>,...] [restore=0] [line=0] [slot=0]

The two outputs come from the same settings except for the crystal. Everything is worked out
here a second time from the tables: which kind each exalted base is, what a restore must add,
which loot rows hold an essence.
"""
import json
import re
import os
import struct
import sys

MOD, VAN, WITHOUT, WITH = sys.argv[1:5]
OPT = dict(a.split("=", 1) for a in sys.argv[5:])
DROP = int(OPT.get("drop", "100"))
OFF = set(filter(None, OPT.get("off", "").split(",")))
RESTORE = OPT.get("restore", "1") != "0"
LINE = OPT.get("line", "1") != "0"
MARKER_ID = 500
ESSENCES = {"tes", "ceh", "bet", "fed"}

# The lists as the user set them (doc "Exalted Shard: attributes it could add", 2026-11-15).
KINDS = [
    ("body_armor", ["tors"], [("all_resistances", "res-all", 4, 7), ("enhanced_defense", "ac%", 30, 40), ("maximum_life", "hp%", 3, 4), ("requirements", "ease", -10, -5)]),
    ("helms", ["helm"], [("enhanced_defense", "ac%", 30, 40), ("maximum_life", "hp%", 3, 4), ("replenish_life", "regen", 5, 10), ("requirements", "ease", -10, -5)]),
    ("shields", ["shld"], [("all_resistances", "res-all", 4, 7), ("enhanced_defense", "ac%", 30, 40), ("chance_to_block", "block", 5, 10), ("requirements", "ease", -10, -5)]),
    ("belts", ["belt"], [("faster_run_walk", "move1", 5, 10), ("maximum_mana", "mana%", 5, 5), ("attack_speed", "swing1", 5, 10), ("requirements", "ease", -10, -5)]),
    ("gloves", ["glov"], [("attack_speed", "swing1", 5, 10), ("attack_rating", "att%", 5, 10), ("faster_hit_recovery", "balance1", 5, 10), ("requirements", "ease", -10, -5)]),
    ("boots", ["boot"], [("faster_run_walk", "move1", 5, 10), ("faster_hit_recovery", "balance1", 5, 10), ("maximum_mana", "mana%", 5, 5), ("requirements", "ease", -10, -5)]),
    ("staves_wands_orbs", ["staf", "wand", "orb"], [("faster_cast_rate", "cast1", 5, 10), ("maximum_mana", "mana%", 5, 5), ("all_resistances", "res-all", 4, 7), ("requirements", "ease", -10, -5)]),
    ("weapons", ["weap"], [("enhanced_damage", "dmg%", 15, 30), ("attack_speed", "swing1", 5, 10), ("attack_rating", "att%", 5, 10), ("requirements", "ease", -10, -5)]),
]

failures = []


def fail(text):
    failures.append(text)
    print("FAIL:", text)


def find(root, rel):
    """A path below root, without caring about letter case (None when it is not there)."""
    path = root
    for part in rel.split("/"):
        try:
            match = [n for n in os.listdir(path) if n.lower() == part.lower()]
        except OSError:
            return None
        if not match:
            return None
        path = os.path.join(path, match[0])
    return path


def table(root, rel):
    path = find(root, rel)
    if path is None:
        return None
    with open(path, "rb") as f:
        lines = f.read().decode("latin-1").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    rows = [line.rstrip("\r").split("\t") for line in lines]
    header = {name.lower(): i for i, name in enumerate(rows[0])}
    return header, rows[1:]


def cell(header, row, name):
    i = header.get(name.lower(), -1)
    return row[i] if 0 <= i < len(row) else ""


def source(rel):
    """The table the generator started from: the mod's, else the game's."""
    return table(MOD, rel) or table(VAN, rel)


def written(root, rel):
    return table(root, rel)


crystal = None
for bank in ("global/excel", "global/excel/base"):
    tag = "[" + bank + "] "
    misc_before = source(bank + "/misc.txt")
    misc_after = written(WITH, bank + "/misc.txt")
    if misc_before is None or source(bank + "/armor.txt") is None:
        continue
    has_template = any(cell(misc_before[0], r, "code") in ("xa1", "tes") and cell(misc_before[0], r, "type").strip() for r in misc_before[1])
    if not has_template:
        if misc_after is not None:
            fail(tag + "misc.txt was written although there is no row to build the crystal from")
        continue
    if misc_after is None:
        fail(tag + "misc.txt was not written")
        continue

    # --- the item
    h, rows = misc_after
    if rows[: len(misc_before[1])] != misc_before[1] or len(rows) != len(misc_before[1]) + 1:
        fail(tag + "misc.txt: expected the mod's rows unchanged and one row added")
        continue
    row = rows[-1]
    code = cell(h, row, "code")
    crystal = crystal or code
    if code != crystal:
        fail(tag + "the crystal has another code here: %s, not %s" % (code, crystal))
    taken = {cell(misc_before[0], r, "code") for r in misc_before[1]}
    for other in ("armor.txt", "weapons.txt"):
        t = written(WITH, bank + "/" + other) or source(bank + "/" + other)
        taken |= {cell(t[0], r, "code") for r in t[1]}
    if code in taken or len(code) != 3:
        fail(tag + "the crystal's code '%s' is not a free three-character code" % code)
    wanted = {"useable": "0", "pspell": "-1", "calc1": "", "spelldesc": "", "spelldescstr": "", "usageconditioncalc": "", "advancedstashstackable": "" if OPT.get("slot") == "0" else "1", "uicatoverride": "", "namestr": "xlt_crystal"}
    for name, value in wanted.items():
        if name in h and cell(h, row, name) != value:
            fail(tag + "crystal row: %s is '%s', expected '%s'" % (name, cell(h, row, name), value))
    if not cell(h, row, "name").startswith("[xlt] ") or not cell(h, row, "type").strip():
        fail(tag + "crystal row: not marked, or without an item type")

    # --- the marker stat and its property
    stats_before = source(bank + "/itemstatcost.txt")
    stats_after = written(WITH, bank + "/itemstatcost.txt")
    props_before = source(bank + "/properties.txt")
    props_after = written(WITH, bank + "/properties.txt")
    marked = RESTORE
    if not RESTORE:
        if stats_after is not None or props_after is not None:
            fail(tag + "stat tables were written although restoring is off")
    elif stats_after is None or props_after is None:
        fail(tag + "itemstatcost.txt / properties.txt were not written")
        marked = False
    else:
        sh, srows = stats_after
        if srows[: len(stats_before[1])] != stats_before[1]:
            fail(tag + "itemstatcost.txt: the mod's rows changed")
        if len(srows) != MARKER_ID + 1 or cell(sh, srows[MARKER_ID], "stat") != "xlt_restored":
            fail(tag + "itemstatcost.txt: the marker is not stat number %d" % MARKER_ID)
        else:
            m = srows[MARKER_ID]
            if cell(sh, m, "save bits") in ("", "0"):
                fail(tag + "the marker stat is not saved on items")
            if (cell(sh, m, "descstrpos") == "xlt_restored") != LINE:
                fail(tag + "the marker's line of text does not follow the setting")
        names = [cell(sh, r, "stat") for r in srows]
        if len(set(names)) != len(names) or any(not n for n in names):
            fail(tag + "itemstatcost.txt: a stat name is empty or used twice")
        for i in range(len(stats_before[1]), min(MARKER_ID, len(srows))):
            if any(c for k, c in enumerate(srows[i]) if k not in (sh["stat"], sh.get("*id", -1), sh.get("*eol", -1))):
                fail(tag + "itemstatcost.txt: filler row %d is not empty" % i)
                break
        ph, prows = props_after
        if prows[:-1] != props_before[1] or cell(ph, prows[-1], "code") != "xlt-restored" or cell(ph, prows[-1], "stat1") != "xlt_restored" or cell(ph, prows[-1], "func1") != "1":
            fail(tag + "properties.txt: expected the mod's rows and one 'xlt-restored' row")

    # --- restore recipes: one per exalted base, with exactly the attributes of its kind
    types = source(bank + "/itemtypes.txt")
    parents = {cell(types[0], r, "code"): (cell(types[0], r, "equiv1"), cell(types[0], r, "equiv2")) for r in types[1] if cell(types[0], r, "code")}

    def is_a(kind, wanted_type, depth=0):
        if not kind or depth > 12:
            return False
        if kind == wanted_type:
            return True
        first, second = parents.get(kind, ("", ""))
        return is_a(first, wanted_type, depth + 1) or is_a(second, wanted_type, depth + 1)

    expected = {}
    for other in ("armor.txt", "weapons.txt"):
        ih, irows = written(WITH, bank + "/" + other)
        for r in irows:
            if not cell(ih, r, "name").startswith("[xlt] "):
                continue
            for key, kind_types, attributes in KINDS:
                if any(is_a(cell(ih, r, "type").strip(), t) or is_a(cell(ih, r, "type2").strip(), t) for t in kind_types):
                    mods = [(prop, lo, hi) for name, prop, lo, hi in attributes if key + "." + name not in OFF]
                    if mods and marked:
                        expected[cell(ih, r, "code")] = (key, mods)
                    break
    ch, crows = written(WITH, bank + "/cubemain.txt")
    before = written(WITHOUT, bank + "/cubemain.txt")
    extra = crows[len(before[1]):]
    if crows[: len(before[1])] != before[1]:
        fail(tag + "cubemain.txt: rows other than the added restore recipes differ")
    seen = {}
    for r in extra:
        base = cell(ch, r, "input 1")
        if base in seen:
            fail(tag + "two restore recipes for base " + base)
        seen[base] = r
        if base not in expected:
            fail(tag + "restore recipe for '%s', which should have none" % base)
            continue
        key, mods = expected[base]
        got = [(cell(ch, r, "mod %d" % i), cell(ch, r, "mod %d min" % i), cell(ch, r, "mod %d max" % i)) for i in range(1, 6)]
        want = [("xlt-restored", "1", "1")] + [(p, str(lo), str(hi)) for p, lo, hi in mods]
        want += [("", "", "")] * (5 - len(want))
        if got != want:
            fail(tag + "restore recipe of %s (%s): mods are %s" % (base, key, got))
        fixed = {"input 2": code, "numinputs": "2", "op": "18", "param": str(MARKER_ID), "value": "0", "output": "useitem", "enabled": "1", "input 3": ""}
        for name, value in fixed.items():
            if cell(ch, r, name) != value:
                fail(tag + "restore recipe of %s: %s is '%s', expected '%s'" % (base, name, cell(ch, r, name), value))
    for base in expected:
        if base not in seen:
            fail(tag + "no restore recipe for exalted base " + base)
    counts = {}
    for key, _ in expected.values():
        counts[key] = counts.get(key, 0) + 1
    print(tag + "crystal '%s'; %d restore recipes: %s" % (code, len(extra), ", ".join("%s %d" % (k, counts[k]) for k, _, _ in KINDS if k in counts)))

    # --- drops: beside every essence at its weight; in the Hell item rows of Blood Raven, the
    # Countess and Nihlathak with the same share of a pick as in the first essence row; nowhere else
    th, trows = written(WITH, bank + "/treasureclassex.txt")
    tb = written(WITHOUT, bank + "/treasureclassex.txt")
    listed = extra_listed = 0
    share = None
    if len(trows) != len(tb[1]):
        fail(tag + "treasureclassex.txt: row count changed")

    def entries(r):
        return [(cell(th, r, "item%d" % i), cell(th, r, "prob%d" % i)) for i in range(1, 11)]

    def row_total(r):
        return int(cell(th, r, "nodrop") or 0) + sum(int(p or 0) for n, p in entries(r) if n)

    for old in tb[1]:
        essence = max([int(p or 0) for n, p in entries(old) if n in ESSENCES], default=0)
        if essence > 0 and DROP > 0:
            share = (max(1, (essence * DROP + 50) // 100), row_total(old))
            break
    for new, old in zip(trows, tb[1]):
        items = entries(old)
        essence = max([int(p or 0) for n, p in items if n in ESSENCES], default=0)
        boss = re.match(r"(Blood Raven|Nihlathak)( Item)? \(H\)|Countess Item \(H\)", old[0]) and any(n.strip('"').split(",")[0] == "gld" for n, _ in items)
        if DROP <= 0 or (essence <= 0 and not (boss and share)):
            if new != old:
                fail(tag + "loot row '%s' changed although it is no boss row" % old[0])
            continue
        if essence > 0:
            weight = max(1, (essence * DROP + 50) // 100)
            listed += 1
        else:
            weight = max(1, int(row_total(old) * share[0] / share[1] + 0.5))
            extra_listed += 1
        used = max(i for i, (n, _) in enumerate(items) if n)
        want = list(old)
        want += [""] * (max(th["item%d" % (used + 2)], th["prob%d" % (used + 2)]) + 1 - len(want))
        want[th["item%d" % (used + 2)]] = code
        want[th["prob%d" % (used + 2)]] = str(weight)
        if new[: len(want)] != want or any(new[len(want):]):
            fail(tag + "loot row '%s': the crystal is not listed after the last entry at weight %d" % (old[0], weight))
    print(tag + "the crystal is in %d loot rows with a boss essence%s, and in %d Hell rows of Blood Raven, the Countess and Nihlathak" % (listed, "" if DROP == 100 else " (at %d%% of its weight)" % DROP, extra_listed))
    if DROP > 0 and (listed == 0 or extra_listed != 14):
        fail(tag + "expected essence rows and 14 rows of the three other bosses")

    # --- upgrade recipes that ask for the crystal name its code
    for r in before[1] + extra:
        if "crystal" in [cell(ch, r, "input %d" % i) for i in range(1, 8)]:
            fail(tag + "a recipe still has the word 'crystal' as a material")

# --- nothing else moved: the exalted bases, and every name and model but the crystal's own
for rel in ("global/excel/armor.txt", "global/excel/weapons.txt", "global/excel/base/armor.txt", "global/excel/base/weapons.txt"):
    a, b = find(WITHOUT, rel), find(WITH, rel)
    if (a is None) != (b is None) or (a and open(a, "rb").read() != open(b, "rb").read()):
        fail(rel + " differs between the run without and the run with the crystal")
for rel, own in (("local/lng/strings/item-names.json", lambda e: e.get("Key") in ("xlt_crystal", "xlt_restored")), ("hd/items/items.json", lambda e: crystal in e)):
    with open(find(WITHOUT, rel), encoding="utf-8-sig") as f:
        a = json.load(f)
    with open(find(WITH, rel), encoding="utf-8-sig") as f:
        b = json.load(f)
    if a != [e for e in b if not own(e)]:
        fail(rel + ": entries other than the crystal's own differ")

if crystal is None:
    fail("no crystal was made")
else:
    # --- name, picture, model
    names_path = find(WITH, "local/lng/strings/item-names.json")
    keys = {}
    if names_path:
        with open(names_path, encoding="utf-8-sig") as f:
            keys = {e["Key"]: e for e in json.load(f)}
    COLOR = OPT.get("color", "8")
    if "xlt_crystal" not in keys or keys["xlt_crystal"].get("enUS") != ("" if COLOR == "none" else "\u00ffc" + COLOR) + "Restoration Crystal":
        fail("item-names.json: the crystal's name is %r" % keys.get("xlt_crystal", {}).get("enUS"))
    if ("xlt_restored" in keys) != (RESTORE and LINE):
        fail("item-names.json: the 'Restored' text does not follow the settings")
    ids = [e["id"] for e in keys.values()]
    if len(ids) != len(set(ids)):
        fail("item-names.json: a string id is used twice")
    with open(find(WITH, "hd/items/items.json"), encoding="utf-8-sig") as f:
        models = {k: v for e in json.load(f) for k, v in e.items()}
    asset = models.get(crystal, {}).get("asset")
    src = find(MOD, "hd/global/ui/items/misc/shard/mote_of_anguish.sprite") or find(VAN, "hd/global/ui/items/misc/shard/mote_of_anguish.sprite")
    if src is None:
        if asset != models.get("xa1", {}).get("asset"):
            fail("without the shard's picture the crystal should share the shard's look, but its asset is %s" % asset)
        print("picture: the shard's own (its sprite is not in the test data)")
    else:
        if asset != "shard/xlt_restoration_crystal":
            fail("items.json: the crystal's asset is %s" % asset)
        for name in ("sprite", "lowend.sprite"):
            out = find(WITH, "hd/global/ui/items/misc/shard/xlt_restoration_crystal." + name)
            origin = src[: -len("sprite")] + name
            if out is None or not os.path.exists(origin):
                if name == "sprite":
                    fail("the tinted picture was not written")
                continue
            a, b = open(origin, "rb").read(), open(out, "rb").read()
            if len(a) != len(b) or a[:40] != b[:40] or a[43::4] != b[43::4]:
                fail(name + ": size, header or transparency changed")
                continue
            width, height = struct.unpack_from("<II", b, 8)
            solid = purple = same_light = 0
            for i in range(40, len(b), 4):
                if b[i + 3] == 0:
                    if a[i : i + 3] != b[i : i + 3]:
                        fail(name + ": a fully transparent pixel changed")
                        break
                    continue
                solid += 1
                purple += b[i + 2] >= b[i] >= b[i + 1]
                same_light += max(b[i : i + 3]) == max(a[i : i + 3])
            if solid == 0 or purple != solid or same_light != solid:
                fail("%s: %d of %d pixels are purple, %d kept their brightness" % (name, purple, solid, same_light))
            else:
                print("picture (%s): %dx%d, %d visible pixels, all purple, brightness and transparency kept" % (name, width, height, solid))
        model = find(WITH, "hd/items/misc/shard/xlt_restoration_crystal.json")
        origin = find(MOD, "hd/items/misc/shard/mote_of_anguish.json") or find(VAN, "hd/items/misc/shard/mote_of_anguish.json")
        if model is None or open(model, "rb").read() != open(origin, "rb").read():
            fail("the ground model description is not a copy of the shard's")

print("RESULT:", "ok" if not failures else "%d problem(s)" % len(failures))
sys.exit(1 if failures else 0)
