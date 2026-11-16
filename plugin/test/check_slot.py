#!/usr/bin/env python3
# Exalted Items: the stash slots added to a stash layout.
#
#   check_slot.py <layout before> <layout after> <item code>[,<item code>...] [place=<dx>:<dy>[,<dx>:<dy>...]] [beside]
#
# Each code has exactly one new slot, in the container of the Western Worldstone Shard's slot,
# the size of that slot, over no other slot; nothing else in the layout changed, and no widget
# other than a slot was put beside the slots (the game fails on that). place= gives, per code,
# where the slot must be, counted in pixels from the Worldstone Shard's slot. "beside": the new
# slots stand side by side on one line.
import json, sys

def load(p): return json.loads(open(p, encoding='utf-8-sig').read())
def slots(n, path, out):
    if isinstance(n, dict):
        if n.get('type') == 'AdvancedStashSlotWidget': out.append((path, n))
        for c in n.get('children', []): slots(c, path + '/' + str(n.get('name')), out)
    return out
def strip(n, codes):
    if isinstance(n, dict) and 'children' in n:
        n['children'] = [c for c in n['children'] if not (c.get('type') == 'AdvancedStashSlotWidget' and c['fields'].get('itemCode') in codes)]
        for c in n['children']: strip(c, codes)

before, after, codes = sys.argv[1], sys.argv[2], sys.argv[3].split(',')
opts = dict(a.split('=', 1) if '=' in a else (a, '1') for a in sys.argv[4:])
a = load(after); b = load(before)
sa = slots(a, '', []); sb = slots(b, '', [])
assert len(sa) == len(sb) + len(codes), (len(sa), len(sb))
xa = [(q, m) for q, m in sa if m['fields']['itemCode'] == 'xa1'][0]
xr = xa[1]['fields']['rect']
places = [tuple(int(v) for v in p.split(':')) for p in opts['place'].split(',')] if 'place' in opts else None
rects = []
for i, code in enumerate(codes):
    mine = [(p, n) for p, n in sa if n['fields']['itemCode'] == code]
    assert len(mine) == 1, (code, len(mine))
    p, n = mine[0]; r = n['fields']['rect']
    assert xa[0] == p and n['name'] == code and (r['width'], r['height']) == (xr['width'], xr['height']), code
    for q, m in sa:
        if m is n or q != p: continue
        s = m['fields']['rect']
        assert not (r['x'] < s['x'] + s['width'] and s['x'] < r['x'] + r['width'] and r['y'] < s['y'] + s['height'] and s['y'] < r['y'] + r['height']), ('overlap', code, m['name'])
    if places:
        assert (r['x'] - xr['x'], r['y'] - xr['y']) == places[i], (code, 'is at', (r['x'] - xr['x'], r['y'] - xr['y']), 'from the Worldstone Shard, expected', places[i])
    rects.append(r)
if 'beside' in opts:
    assert len(rects) == 2 and rects[0]['y'] == rects[1]['y'] and abs(abs(rects[0]['x'] - rects[1]['x']) - rects[0]['width']) <= 12, 'the two slots are not side by side'
def others(n, out):
    if isinstance(n, dict):
        for c in n.get('children', []):
            if n.get('name') == xa[0].rsplit('/', 1)[-1] and c.get('type') != 'AdvancedStashSlotWidget': out.append(c.get('type'))
            others(c, out)
    return out
assert others(a, []) == others(b, []), 'a widget that is not a slot was put beside the slots (the game fails on that)'
strip(a, set(codes)); assert a == b, 'something else changed'
print('ok', xa[0], ', '.join('%s at x %d, y %d' % (c, r['x'], r['y']) for c, r in zip(codes, rects)))
