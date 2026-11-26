#!/usr/bin/env python3
# A mod that brings some of Exalted Items' cube rows itself (exported from a game that ran the
# plugin, or written by hand) keeps them exactly as they are; the plugin makes the rest.
#   check_mod_rows.py make <plugin's cubemain> <mod cubemain to extend>
#   check_mod_rows.py check <cubemain made on that mod>
import sys

def read(path):
    text = open(path, encoding="latin-1", newline="").read()
    eol = "\r\n" if "\r\n" in text else "\n"
    lines = text.rstrip("\r\n").split(eol)
    head = lines[0].split("\t")
    return text, eol, head, [dict(zip(head, l.split("\t"))) for l in lines[1:]]

if sys.argv[1] == "make":
    _, _, head, rows = read(sys.argv[2])
    new = []
    for r in rows:
        if "restored: Body armor" in r["description"]:
            r = dict(r)
            for i in (2, 3, 4, 5):
                for s in ("", " min", " max"):
                    r[f"mod {i}{s}"] = ""
            r["mod 2"], r["mod 2 min"], r["mod 2 max"] = "xlt-r1", "30", "40"
            r["description"] = "[xlt] my mod: " + r["description"][6:]
            new.append(r)
    helm = dict(next(r for r in rows if "restored: Helms" in r["description"])); helm["enabled"] = "0"; new.append(helm)
    down = dict(next(r for r in rows if "-> " in r["description"] and r["description"].startswith("[xlt] Exalted"))); down["input 2"] = "r05"; new.append(down)
    up = dict(next(r for r in rows if r["description"].startswith("[xlt] Elite"))); up["input 2"] = "r33"; new.append(up)
    text, eol, mhead, _ = read(sys.argv[3])
    text = text.rstrip("\r\n") + eol + eol.join("\t".join(r.get(c, "").rstrip("\r") for c in mhead) for r in new) + eol
    open(sys.argv[3], "w", encoding="latin-1", newline="").write(text)
    open(sys.argv[3] + ".expect", "w").write(f"{helm['input 1']}\n{down['input 1']}\n{up['input 1']}\n")
    sys.exit(0)

_, _, head, rows = read(sys.argv[2])
helm, down, up = open(sys.argv[2] + ".expect").read().split()
x = [r for r in rows if r["description"].startswith("[xlt] ")]
bad = []
restores = [r for r in x if " restored: " in r["description"]]
body = [r for r in restores if "Body armor" in r["description"]]
if not body or not all(r["description"].startswith("[xlt] my mod") and r["mod 2"] == "xlt-r1" and r["mod 3"] == "" for r in body):
    bad.append("the mod's body armor restore rows were not kept as they are")
per = {}
for r in restores:
    per[r["input 1"]] = per.get(r["input 1"], 0) + 1
if max(per.values()) != 1:
    bad.append("a base has two restore rows")
if [r["enabled"] for r in restores if r["input 1"] == helm] != ["0"]:
    bad.append("the switched-off helm restore is not the only one for its base, switched off")
downs = [r for r in x if "-> " in r["description"] and r["input 1"] == down]
if [r["input 2"] for r in downs] != ["r05"]:
    bad.append("the mod's downgrade row was not kept, or the plugin made one too")
ups = [r for r in x if r["description"].startswith("[xlt] Elite") and r["input 1"] == up]
if [r["input 2"] for r in ups] != ["r33"]:
    bad.append("the mod's upgrade row was not kept, or the plugin made one too")
others = [r for r in restores if "Body armor" not in r["description"] and r["input 1"] != helm]
if not others or not all(r["description"].startswith("[xlt] Exalted") for r in others):
    bad.append("the other restore rows are not the plugin's")
print(f"  {len(body)} body armor restores from the mod, {len(others)} made by the plugin, helm {helm} off, downgrade {down} and upgrade {up} the mod's ->", "ok" if not bad else "FAILED")
for b in bad:
    print("  FAILED:", b)
sys.exit(1 if bad else 0)
