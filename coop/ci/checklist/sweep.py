import re,sys
# joiner goes to and breaks/collects every destroy/collect objective target
rows={}
for line in open(sys.argv[1] if len(sys.argv) > 1 else 'objectives.txt'):
    st,rest=line.split(':',1)
    steps=[];t=12
    for kind,tag in re.findall(r'(destroy|collect):(\d+)',rest):
        steps.append(f"o{tag}@{t}"); steps.append(("b" if kind=='destroy' else "g")+f"{tag}@{t+2}"); t+=4
    if steps: rows[st]=(steps[:24],t)
for st,(steps,t) in rows.items():
    print(f"{st}|{','.join(steps)}|{min(t+25,140)}")
