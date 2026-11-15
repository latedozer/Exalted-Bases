import json,sys,re
def load(p): return json.loads(open(p,encoding='utf-8-sig').read())
def slots(n,path,out):
    if isinstance(n,dict):
        if n.get('type')=='AdvancedStashSlotWidget': out.append((path,n))
        for c in n.get('children',[]): slots(c,path+'/'+str(n.get('name')),out)
    return out
def strip(n,code):
    if isinstance(n,dict) and 'children' in n:
        frames.extend(c for c in n['children'] if c.get('type')=='ImageWidget' and c.get('name')==code+'_frame')
        n['children']=[c for c in n['children'] if not (c.get('type')=='AdvancedStashSlotWidget' and c['fields'].get('itemCode')==code) and not (c.get('type')=='ImageWidget' and c.get('name')==code+'_frame')]
        for c in n['children']: strip(c,code)
frames=[]
before,after,code=sys.argv[1:4]
a=load(after); b=load(before)
sa=slots(a,'',[]); sb=slots(b,'',[])
mine=[(p,n) for p,n in sa if n['fields']['itemCode']==code]
assert len(mine)==1 and len(sa)==len(sb)+1, (len(mine),len(sa),len(sb))
p,n=mine[0]; r=n['fields']['rect']
xa=[(q,m) for q,m in sa if m['fields']['itemCode']=='xa1'][0]
assert xa[0]==p and n['name']==code and (r['width'],r['height'])==(xa[1]['fields']['rect']['width'],xa[1]['fields']['rect']['height'])
for q,m in sa:
    if m is n or q!=p: continue
    s=m['fields']['rect']
    assert not (r['x']<s['x']+s['width'] and s['x']<r['x']+r['width'] and r['y']<s['y']+s['height'] and s['y']<r['y']+r['height']), ('overlap',m['name'])
strip(a,code); assert a==b, 'something else changed'
assert len(frames)==0, 'a widget that is not a slot was put beside the slots (the game fails on that)'
want_frame = len(sys.argv)>4 and sys.argv[4]=='frame'
if want_frame:
    # the slot must continue a row: a slot right beside it on the same line
    assert any(q==p and m is not n and abs(m['fields']['rect']['y']-r['y'])<=8 and abs(abs(m['fields']['rect']['x']-r['x'])-r['width'])<=12 for q,m in sa), 'no slot beside it'
near=sorted(((m['fields']['rect']['x']-r['x'])**2+(m['fields']['rect']['y']-r['y'])**2,m['name']) for q,m in sa if q==p and m is not n)[:3]
print('ok',p,r,'neighbours',[x[1] for x in near])
