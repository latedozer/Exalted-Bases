#!/usr/bin/env python3
# Checks the loot filter rule panel's layout as the generator wrote it against the game's own:
# one more Quality box (the Exalted one), a copy of the Elite box, and nothing else changed.
#
#   check_filter.py <game's lootfilteritempanelhd.json> <adjusted copy> [label]
import json, re, sys

def load(path):
    text = open(path, encoding="utf-8-sig", newline="").read()
    out, i, in_string = [], 0, False
    while i < len(text):
        c = text[i]
        if in_string:
            out.append(c)
            if c == "\\":
                i += 1
                out.append(text[i])
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
            out.append(c)
        elif text.startswith("//", i):
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    return text, json.loads(re.sub(r",(\s*[}\]])", r"\1", "".join(out)))

def find(node, name):
    if isinstance(node, dict):
        if node.get("name") == name:
            return node
        for child in node.get("children", []):
            hit = find(child, name)
            if hit:
                return hit
    return None

failed = 0
def check(ok, what):
    global failed
    print(("  ok   " if ok else "  FAIL ") + what)
    failed += 0 if ok else 1

stock_text, stock = load(sys.argv[1])
text, layout = load(sys.argv[2])
label = sys.argv[3] if len(sys.argv) > 3 else "Exalted"
quality = find(layout, "QualityContainer")
boxes = [c for c in quality["children"] if c["type"] == "ToggleButtonWidget"]
check([b["name"] for b in boxes] == ["NormalQualityChbx", "ExceptionalQualityChbx", "EliteQualityChbx", "ExaltedQualityChbx"], "the Quality boxes are Normal, Exceptional, Elite, Exalted")
elite, exalted = boxes[2], boxes[-1]
stock_elite = find(stock, "EliteQualityChbx")
check(exalted["fields"]["onClickMessage"] == "LootFilterItemPanelMessage:UpdateQualityFilter:exalted", "the new box sends UpdateQualityFilter:exalted")
check(exalted["fields"]["rect"] == dict(stock_elite["fields"]["rect"], x=stock_elite["fields"]["rect"]["x"] + 400), "it sits 400 to the right of the Elite box, same height and size")
check(exalted["children"][0]["name"] == "ExaltedQualityDesc" and exalted["children"][0]["fields"]["text"] == label, 'its text is "%s"' % label)
check(exalted["fields"]["rect"]["x"] + 81 * 1.1 + exalted["children"][0]["fields"]["rect"]["width"] <= 1687 + 60, "the text ends about inside the 1687-wide list")
check(exalted["fields"]["navigation"]["left"] == {"name": "EliteQualityChbx"} and elite["fields"]["navigation"].get("right") == {"name": "ExaltedQualityChbx"}, "a controller goes Elite <-> Exalted")
same = {k: v for k, v in exalted["fields"].items() if k not in ("rect", "onClickMessage", "navigation")} == {k: v for k, v in stock_elite["fields"].items() if k not in ("rect", "onClickMessage", "navigation")}
check(same, "everything else about the box is the Elite box's (picture, frames, legend)")
# nothing else changed: take the new box and the Elite box's added "right" out again
del quality["children"][-1]
del elite["fields"]["navigation"]["right"]
check(layout == stock, "the rest of the layout is the game's own")
check(text.count("\r\n") == text.count("\n"), "Windows line ends throughout")
print("RESULT: %s" % ("ok" if failed == 0 else "%d FAILED" % failed))
sys.exit(1 if failed else 0)
