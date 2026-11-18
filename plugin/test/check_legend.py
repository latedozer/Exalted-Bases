#!/usr/bin/env python3
"""The legend lists every exalted base and material of codes.txt exactly once, with the right
elite code, and names each base from item-names.json (its English name)."""
import json, re, sys

legend_path, codes_path, names_path = sys.argv[1:4]
codes = {}
for line in open(codes_path, encoding='utf-8'):
    line = line.strip()
    if line and not line.startswith('#'):
        elite, code = line.split('=', 1)
        codes[elite] = code
english = {}
raw = open(names_path, encoding='utf-8-sig').read()
for entry in json.loads(raw):
    if entry.get('Key', '').startswith('xlt_'):
        english[entry['Key'][4:]] = entry.get('enUS', '')
text = open(legend_path, encoding='utf-8', newline='').read()
fails = 0
rows = {}
for line in text.split('\r\n'):
    m = re.match(r'^  (.+?)\s{2,}([0-9a-z]{3})\s{2,}([0-9a-z]{3})\s+(\d+)\s{2}(.+)$', line)
    if m:
        name, code, elite = m.group(1), m.group(2), m.group(3)
        if code in rows:
            print(f'FAIL: {code} listed twice'); fails += 1
        rows[code] = (name, elite)
bases = {e: c for e, c in codes.items() if not e.startswith('*')}
for elite, code in bases.items():
    if code not in rows:
        print(f'FAIL: {code} ({elite}) missing'); fails += 1
    elif rows[code][1] != elite:
        print(f'FAIL: {code} listed with elite {rows[code][1]}, codes.txt says {elite}'); fails += 1
    elif code in english and rows[code][0] != english[code]:
        print(f'FAIL: {code} named {rows[code][0]!r}, item-names.json says {english[code]!r}'); fails += 1
extra = set(rows) - set(bases.values())
if extra:
    print(f'FAIL: codes in the legend that codes.txt does not have: {sorted(extra)}'); fails += 1
for key, word in (('*crystal', 'crystal'), ('*desecration', 'desecration')):
    if key in codes and not re.search(rf'  {codes[key]}\s+{word}\r\n', text):
        print(f'FAIL: material {word} ({codes[key]}) missing'); fails += 1
print(f'RESULT: {len(rows)} bases listed, {fails} failure(s)')
sys.exit(1 if fails else 0)
