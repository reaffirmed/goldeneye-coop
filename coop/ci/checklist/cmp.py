import re,sys,os
# compare objective lines, step failures, desyncs, crashes for each run dir
def objs(path):
    r=[]
    for l in open(path,errors='replace'):
        m=re.search(r'objective (\d+) \((\w)\) is now (\w+)',l)
        if m: r.append((m.group(2),m.group(3)))
    return r
def final(seq):
    d={}
    for k,v in seq: d[k]=v
    return d
for root in sys.argv[1:]:
  for name in sorted(os.listdir(root)):
    d=os.path.join(root,name); h=os.path.join(d,'host.log'); j=os.path.join(d,'join.log')
    if not os.path.exists(h): continue
    ho,jo=objs(h),objs(j)
    hf,jf=final(ho),final(jo)
    ht=open(h,errors='replace').read(); jt=open(j,errors='replace').read()
    notes=[]
    if 'join snapshot applied' not in jt: notes.append('NO JOIN')
    for k in sorted(set(hf)|set(jf)):
        if hf.get(k)!=jf.get(k): notes.append(f"{k}: host={hf.get(k)} joiner={jf.get(k)}")
    jfail=[k for k,v in jo if v=='failed' and hf.get(k)!='failed' and ('failed' not in [vv for kk,vv in ho if kk==k])]
    if jfail: notes.append('joiner-only failed flash: '+','.join(sorted(set(jfail))))
    hfail=[k for k,v in ho if v=='failed']
    if hfail: notes.append('host failed: '+','.join(sorted(set(hfail))))
    nos=re.findall(r'step (\w+): no (chr|object)',jt+ht)
    if nos: notes.append('missing: '+','.join(a for a,b in nos))
    if 'unresolved desync' in ht+jt: notes.append('UNRESOLVED DESYNC')
    nd=len(re.findall('repl: desync',ht+jt))
    if re.search(r'segmentation|signal 11|assert',ht+jt,re.I): notes.append('CRASH?')
    ends=('mission end' in ht, 'mission end' in jt)
    print(f"{name:12s} host:{' '.join(k+'='+v[0] for k,v in sorted(hf.items()))}  join:{' '.join(k+'='+v[0] for k,v in sorted(jf.items()))}  desyncs={nd} {'; '.join(notes)}")
