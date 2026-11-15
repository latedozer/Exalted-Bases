#!/usr/bin/env python3
"""Checks the generator's output against the mod's own files.

    check_tables.py <mod data dir> <output dir> [--show]

Reads both folders, changes nothing. Exits 1 when a check fails."""
import math
import os
import re
import sys

BANKS = ("global/excel", "global/excel/base")
STEMS = ("weap", "armo", "mele", "bow", "abow")
failures = []


def fail(text):
    failures.append(text)
    print("FAIL:", text)


def find(root, rel):
    cur = root
    for part in rel.split("/"):
        hit = None
        if os.path.isdir(cur):
            for name in os.listdir(cur):
                if name.lower() == part.lower():
                    hit = name
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def load(root, rel):
    path = find(root, rel)
    if path is None:
        return None, None
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        lines = [l.rstrip("\r") for l in f.read().split("\n")]
    if lines and lines[-1] == "":
        lines.pop()
    header = lines[0].split("\t")
    rows = [dict(zip(header, l.split("\t") + [""] * (len(header) - len(l.split("\t"))))) for l in lines[1:]]
    return header, rows


def raw(root, rel):
    path = find(root, rel)
    return open(path, "rb").read() if path else None


def num(text, default=0):
    try:
        return int(text)
    except (TypeError, ValueError):
        return default


def entries(row):
    out = []
    for k in range(1, 11):
        cell = row.get("Item%d" % k, "")
        if cell:
            out.append((cell.strip('"'), num(row.get("Prob%d" % k))))
    return out


def table_of(name):
    m = re.fullmatch(r"(%s)(\d+)" % "|".join(STEMS), name.split(",")[0])
    return (m.group(1), int(m.group(2))) if m else None


def check_bank(mod, out, bank, show):
    print("== %s ==" % bank)
    types_h, types = load(mod, bank + "/itemtypes.txt")
    parents = {r["Code"]: (r.get("Equiv1", ""), r.get("Equiv2", "")) for r in types if r.get("Code")}

    def is_a(kind, wanted, depth=0):
        if not kind or depth > 12:
            return False
        if kind == wanted:
            return True
        a, b = parents.get(kind, ("", ""))
        return is_a(a, wanted, depth + 1) or is_a(b, wanted, depth + 1)

    # --- item rows -------------------------------------------------------------------
    members = {}
    exalted_levels = {}
    for name in ("weapons.txt", "armor.txt"):
        mh, mrows = load(mod, bank + "/" + name)
        oh, orows = load(out, bank + "/" + name)
        if orows is None:
            fail("%s/%s was not written" % (bank, name))
            continue
        by_code = {r["code"]: r for r in mrows if r.get("code")}
        added = [r for r in orows if r["name"].startswith("[xlt] ")]
        kept = [r for r in orows if not r["name"].startswith("[xlt] ")]
        if len(kept) != len(mrows):
            fail("%s/%s: stock row count changed" % (bank, name))
        for old, new in zip(mrows, kept):
            diff = [k for k in mh if old.get(k, "") != new.get(k, "")]
            if diff and diff != ["ubercode"]:
                fail("%s/%s: stock row %s changed in %s" % (bank, name, old.get("name"), diff))
        for r in added:
            elite = by_code.get(r["ultracode"])
            if elite is None:
                fail("%s: exalted row %s has no elite row" % (name, r["name"]))
                continue
            if r["spawnable"] != elite["spawnable"]:
                fail("%s: %s spawnable differs from its elite row" % (name, r["name"]))
            exalted_levels[r["code"]] = (num(r["level"]), num(elite["level"]))
        for r in orows:
            level = num(r.get("level"))
            if r.get("spawnable") != "1" or level <= 0 or r.get("quest", "") not in ("", "0"):
                continue
            for stem in STEMS:
                if is_a(r.get("type", ""), stem) or is_a(r.get("type2", ""), stem):
                    members.setdefault((stem, math.ceil(level / 3) * 3), []).append(r["name"])
    levels = sorted(set(l for l, _ in exalted_levels.values()))
    print("  exalted bases: %d, item levels %s" % (len(exalted_levels), levels))
    if len(levels) > 10 or any(b - a != 3 for a, b in zip(levels, levels[1:])):
        fail("%s: exalted levels are not ten tables three apart: %s" % (bank, levels))
    order = sorted(exalted_levels.values(), key=lambda v: v[1])
    if any(a[0] > b[0] for a, b in zip(order, order[1:])):
        fail("%s: a lower elite base got a higher exalted level" % bank)
    per_table = {}
    for level, _ in exalted_levels.values():
        per_table[level] = per_table.get(level, 0) + 1
    print("  exalted bases per level:", dict(sorted(per_table.items())))

    # --- unique and set tables are never written ----------------------------------------
    for name in ("uniqueitems.txt", "setitems.txt", "sets.txt", "monstats.txt", "superuniques.txt", "itemtypes.txt", "misc.txt"):
        if find(out, bank + "/" + name):
            fail("%s/%s was written" % (bank, name))

    # --- loot rows --------------------------------------------------------------------
    th, trows = load(mod, bank + "/treasureclassex.txt")
    oh, orows = load(out, bank + "/treasureclassex.txt")
    if orows is None:
        print("  treasureclassex.txt: not written (the mod's own rows reach every exalted table)")
        top = max((t[1] for r in trows for n, _ in entries(r) for t in [table_of(n)] if t), default=0)
        if levels and math.ceil(levels[-1] / 3) * 3 > top:
            fail("%s: exalted bases sit above table %d but no loot rows were added" % (bank, top))
        return
    new = [r for r in orows if r["Treasure Class"].startswith("xlt ")]
    kept = [r for r in orows if not r["Treasure Class"].startswith("xlt ")]
    if len(kept) != len(trows):
        fail("%s: stock loot row count changed" % bank)
    changed = []
    for old, cur in zip(trows, kept):
        diff = [k for k in th if old.get(k, "") != cur.get(k, "")]
        if diff:
            changed.append((old, cur, diff))
            if any(not k.startswith("Item") for k in diff):
                fail("%s: row %s changed outside its item names: %s" % (bank, old["Treasure Class"], diff))
    names = {}
    for i, r in enumerate(orows):
        names.setdefault(r["Treasure Class"], i)
    dup = len(orows) - len(names)
    mod_dup = len(trows) - len(set(r["Treasure Class"] for r in trows))
    if dup != mod_dup:
        fail("%s: a new row repeats an existing name" % bank)
    # every reference resolves and is defined above its use
    item_codes = set()
    for name in ("weapons.txt", "armor.txt", "misc.txt"):
        h, rows = load(out if find(out, bank + "/" + name) else mod, bank + "/" + name)
        item_codes.update(r["code"] for r in rows if r.get("code"))
    for i, r in enumerate(orows):
        for name, prob in entries(r):
            head = name.split(",")[0]
            target = names.get(name, names.get(head))
            if target is not None:
                if target >= i:
                    fail("%s: %s uses %s, which is defined below it" % (bank, r["Treasure Class"], name))
            elif table_of(name):
                stem, level = table_of(name)
                if r["Treasure Class"].startswith("xlt ") and not members.get((stem, level)):
                    fail("%s: %s uses the empty level table %s" % (bank, r["Treasure Class"], name))
            elif head not in item_codes and r["Treasure Class"].startswith("xlt "):
                fail("%s: %s names unknown %s" % (bank, r["Treasure Class"], name))
    # ladders: rows of one group with rising level
    def runs(rows):
        out_runs, cur = [], []
        for r in rows:
            g = r["group"].strip()
            if g in ("", "0"):
                if cur:
                    out_runs.append(cur)
                cur = []
                continue
            if cur and (cur[-1]["group"].strip() != g or num(cur[-1]["level"]) >= num(r["level"])):
                out_runs.append(cur)
                cur = []
            cur.append(r)
        if cur:
            out_runs.append(cur)
        return out_runs

    old_runs = {r[0]["Treasure Class"]: r for r in runs(trows)}
    new_runs = {r[0]["Treasure Class"]: r for r in runs(orows)}
    if set(old_runs) != set(new_runs):
        fail("%s: the ladders were split or joined: %s" % (bank, sorted(set(old_runs) ^ set(new_runs))[:5]))
    extended = 0
    for first, run in new_runs.items():
        old = old_runs.get(first, [])
        if [r["Treasure Class"] for r in run[: len(old)]] != [r["Treasure Class"] for r in old]:
            fail("%s: ladder %s lost or reordered rows" % (bank, first))
        extra = run[len(old):]
        if extra:
            extended += 1
            if any(not r["Treasure Class"].startswith("xlt ") for r in extra):
                fail("%s: ladder %s gained a stock row" % (bank, first))
    grouped_new = [r for r in new if r["group"].strip() not in ("", "0")]
    print("  loot rows: %d stock, %d new (%d in %d levelled ladders, %d ungrouped), %d stock rows re-pointed" % (len(kept), len(new), len(grouped_new), extended, len(new) - len(grouped_new), len(changed)))
    top_table = max((t[1] for r in new for n, _ in entries(r) for t in [table_of(n)] if t), default=0)
    print("  highest level table used by a new row:", top_table, "| new rung levels:", sorted(set(num(r["level"]) for r in grouped_new)))
    if levels and top_table < math.ceil(levels[-1] / 3) * 3:
        fail("%s: no new row reaches the highest exalted table" % bank)
    for r in changed:
        if num(r[0]["level"]) and num(r[0]["level"]) < min(num(x["level"]) for x in grouped_new):
            fail("%s: row %s below the new rungs was changed" % (bank, r[0]["Treasure Class"]))

    # the level walk: the row a monster of a level ends on
    def walk(run, level):
        row = run[0]
        for r in run:
            if num(r["level"]) <= level:
                row = r
        return row["Treasure Class"]

    if show:
        def line(r):
            return "%-44s g=%-3s L=%-3s | %s" % (r["Treasure Class"], r["group"], r["level"], "; ".join("%s x%d" % e for e in entries(r)))
        print("  -- new rows --")
        for r in new:
            print("   ", line(r))
        print("  -- re-pointed stock rows --")
        for old, cur, diff in changed:
            print("    %-40s L=%-3s %s" % (old["Treasure Class"], old["level"], "; ".join("%s -> %s" % (old[k], cur[k]) for k in diff)))
        for first, run in new_runs.items():
            if any("H2H" in r["Treasure Class"] or "Unique C" in r["Treasure Class"] for r in run[-4:]) and len(run) > len(old_runs.get(first, [])):
                print("  -- walk on ladder %s: " % first + ", ".join("level %d -> %s" % (l, walk(run, l)) for l in (85, 87, 88, 90, 91, 94, 99)))
    return


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mod, out = sys.argv[1], sys.argv[2]
    show = "--show" in sys.argv
    for bank in BANKS:
        if find(mod, bank + "/weapons.txt") or find(out, bank + "/weapons.txt"):
            check_bank(mod, out, bank, show)
    print("RESULT:", "FAILED (%d)" % len(failures) if failures else "all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
