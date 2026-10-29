# objs.py: each mission's objectives from the US setup files (run from the repo root):
#   python3 coop/ci/checklist/objs.py > objectives.txt
import re
base = 'assets/obseg/setup/'
files = {'dam': 'UsetupdamZ', 'facility': 'UsetuparkZ', 'runway': 'UsetuprunZ', 'surface': 'UsetupsevxZ', 'bunker1': 'UsetupsevbunkerZ',
         'silo': 'u/UsetupsiloZ', 'frigate': 'u/UsetupdestZ', 'surface2': 'UsetupsevxbZ', 'bunker2': 'UsetupsevbZ', 'statue': 'u/UsetupstatueZ',
         'archives': 'UsetuparchZ', 'streets': 'UsetuppeteZ', 'depot': 'UsetupdepoZ', 'train': 'u/UsetuptraZ', 'jungle': 'u/UsetupjunZ',
         'control': 'UsetupcontrolZ', 'caverns': 'UsetupcaveZ', 'cradle': 'u/UsetupcradZ', 'aztec': 'UsetupaztZ', 'egypt': 'UsetupcrypZ'}
short = {'DestroyObject': 'destroy', 'CollectObject': 'collect', 'CompleteCondition': 'flag', 'FailCondition': 'FAILflag', 'EnterRoom': 'enterpad',
         'DepositObjectInRoom': 'deposit', 'Photograph': 'photo', 'Copy_Item': 'copy', 'DepositObject': 'deposit'}
for st, f in files.items():
    s = open(base + f + '.c').read()
    objs = []
    cur = None
    for m in re.finditer(r'/\* Type = (Objective\w+)[^*]*\*/\s*_mkword\(0, _mkshort\(0, \d+\)\),?[ \t]*([^\n]*)', s):
        t = m.group(1)[9:]
        v = [x.strip() for x in m.group(2).split(',') if x.strip()]
        if t == 'Start':
            cur = [f"{chr(65 + len(objs))}(d{v[2]})"]
            objs.append(cur)
        elif t == 'End':
            cur = None
        elif cur is not None:
            if t in ('CompleteCondition', 'FailCondition'):
                v = [hex(int(v[0]))]
            cur.append(short.get(t, t) + ':' + '/'.join(v[:2]))
    print(st + ': ' + ' ; '.join(' '.join(o) for o in objs))
