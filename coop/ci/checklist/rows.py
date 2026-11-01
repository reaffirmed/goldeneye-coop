#!/usr/bin/env python3
"""Run rows of coop/TEST-CHECKLIST.md in the rig and judge them from the logs.

    python3 coop/ci/checklist/rows.py <out dir> [ROW ...]      # default: every row below
    python3 coop/ci/checklist/rows.py <out dir> sweep          # the SW-<stage> objective sweep
    python3 coop/ci/checklist/rows.py --judge <out dir> [ROW ...]   # judge earlier logs again, no runs
    PAR=2 SECSX=2.5 python3 coop/ci/checklist/rows.py out/rows D2 F5

PAR pairs run at once (default 2). Seconds below are mission seconds; a pair
is killed after SECSX (default 2.5) times that in wall time, since software
GL runs a loaded machine well under full speed (each pair stops early once
its steps are done). On 4 cores PAR=3 ran the game at about a third of real
time and the steps never got to run: keep PAR at most cores / 2.

Each row is one rig run (pair.sh, or party.sh for 3 and 4 players) plus the
log lines that decide it. A row prints PASS when every check holds, FAIL with
the first check that didn't. A FAIL here is a lead, not a verdict: read the
logs (<out>/<row>/host.log, join.log, or p1.log..p4.log) before calling it a
bug, because a rig step that missed its target (a chr not there yet, an
object out of reach) also fails a check. Rows that need a person at the
controls are not here; TEST-CHECKLIST.md says which.

Checks, each a regex over a log:
    h:<re>   the host's log has it        j:<re>   the joiner's log has it
    b:<re>   both logs have it            !h: !j: !b:  must not have it
    pN:<re>  player N's log (party runs)
    =h:<re>  exactly once in the host's log (=j: =b: likewise)
Every row also checks that the joiner got in (join snapshot applied), that
no log has an unresolved desync and that nothing crashed.
"""
import os
import re
import subprocess
import queue
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
INV = 'GE_CHEATS=invuln'

# row: (stage, diff, host env, joiner env, seconds, join after, checks[, players, extra env per player])
ROWS = {
    # ---- Dam
    'D1': ('dam', 0, f'{INV} GE_COOP_TESTWIN=1', f'{INV} GE_COOP_TESTPAD=330:10', 90, 2,
           ['h:objective 3 \\(A\\) is now complete', 'b:debrief: stage', '=h:mission end', '=j:mission end']),
    'D2': ('dam', 2, INV, f'{INV} GE_COOP_STEPS=o0@12,b0@14,o1@16,b1@18,o2@20,b2@22,o3@24,b3@26', 60, 2,
           ['b:objective 0 \\(A\\) is now complete']),
    # the backup needs the modem in (stage flag 0x10000, set by the host here)
    'D4': ('dam', 2, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_TESTFLAG=10000:5', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=o6@8,u6@9,o7@11,u7@12', 70, 2,
           ['b:objective 2 \\(C\\) is now complete', '!b:objective 2 \\(C\\) is now failed']),
    'D4-solo': ('dam', 2, f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_TESTFLAG=10000:5 GE_COOP_STEPS=o6@8,u6@9,o7@11,u7@12', '', 70, 2,
                ['h:objective 2 \\(C\\) is now complete'], 1),
    'D5': ('dam', 0, f'{INV} GE_COOP_TESTWORLD=1 GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=p1@50', 75, 18,
           ['j:watch: alarm on', 'j:repl: join snapshot applied']),
    # the host watches its whole intro (no A taps), the joiner starts 8 s after it: a drop-in during the swirl
    'D6': ('dam', 0, f'{INV} GE_SKIPINTRO=0', f'{INV} GE_COOP_STEPS=p278@20', 60, 8,
           ['h:intro: overview camera ended .* by its timer', 'j:cutscene: the host.s intro of stage \\d+ is over', 'j:repl: join snapshot applied', 'j:step p278 at \\d+ s done']),
    # ---- Facility
    'F5': ('facility', 0, f'{INV} GE_COOP_TESTDUMP=30 GE_COOP_STEPS=o1@10,b1@14,o2@16,b2@20', f'{INV} GE_COOP_TESTDUMP=30 GE_COOP_STEPS=o1@10,b1@14,o2@16,b2@20', 50, 2,
           ['b:dump: tag 1 .* destroyed [1-9]', 'b:dump: tag 2 .* destroyed [1-9]', '!b:objective 3 \\(D\\) is now failed']),
    'F8': ('facility', 0, INV, f'{INV} GE_COOP_STEPS=p309@10', 60, 2,
           ['=h:mission end', '=j:mission end']),
    # ---- Runway
    'R1': ('runway', 0, INV, f'{INV} GE_COOP_STEPS=o2@10,g2@12', 40, 2,
           ['b:objective 0 \\(A\\) is now complete']),
    'R3': ('runway', 0, INV, f'{INV} GE_COOP_STEPS=o2@10,g2@12,o3@14,u3@16', 80, 2,
           ['=h:mission end', '=j:mission end']),
    # ---- Surface 1
    'S3': ('surface', 0, INV, f'{INV} GE_COOP_STEPS=p46@10', 80, 2,
           ['=h:mission end', '=j:mission end']),
    # ---- Frigate
    # the joiner shoots hostage-taker 1 from 3 to 10 m; FR2-solo is the host doing the same alone
    'FR2': ('frigate', 0, f'{INV} GE_COOP_WATCH=1,44', f'{INV} GE_COOP_WATCH=1,44 GE_COOP_STEPS=n1@10,k1@11', 45, 2,
            ['b:watch: chr 1 gone']),
    'FR2-solo': ('frigate', 0, f'{INV} GE_COOP_WATCH=1,44 GE_COOP_STEPS=n1@10,k1@11', '', 45, 2,
                 ['h:watch: chr 1 gone'], 1),
    # the boat's exit arms once "Bond" has left the boat's room, and in co-op that is every player: both step off first
    'FR5': ('frigate', 0, f'{INV} GE_COOP_STEPS=p60@8', f'{INV} GE_COOP_STEPS=p60@8 GE_COOP_TESTPAD=153:20', 80, 2,
            ['=h:mission end', '=j:mission end']),
    'FR5-hoststays': ('frigate', 0, INV, f'{INV} GE_COOP_STEPS=p60@8 GE_COOP_TESTPAD=153:20', 80, 2,
                      ['=h:mission end', '=j:mission end']),
    # ---- Surface 2
    'SU3': ('surface2', 0, INV, f'{INV} GE_COOP_STEPS=p3@10', 50, 2,
            ['b:objective 3 \\(\\w\\) is now complete', '=h:mission end', '=j:mission end']),
    # ---- Bunker 2 (Natalya is chr 0; the joiner opens her cell, tag 3, with GE_COOP_TESTDOOR)
    'BU1': ('bunker2', 1, f'{INV} GE_COOP_TESTDOOR=1 GE_COOP_STEPS=c27@8', f'{INV} GE_COOP_TESTDOOR=1 GE_COOP_STEPS=o3@8,c13@18,c12@30', 70, 2,
            ['h:chr 0 goes to player 2', '!h:chr 0 goes to player 1']),
    # pair.sh stops both once either side's steps are done: the host's last step waits for the joiner's death
    'BU5': ('bunker2', 1, f'{INV} GE_COOP_TESTDOOR=1 GE_COOP_STEPS=c27@8,c27@60', 'GE_COOP_TESTDOOR=1 GE_COOP_STEPS=o3@8,c13@18,c12@30,d@40', 80, 2,
            ['h:chr 0 goes to player 2', 'h:chr 0 goes to player 1']),
    # ---- Statue
    'ST5': ('statue', 0, INV, f'{INV} GE_COOP_STEPS=g1@12', 60, 2,
            ['b:objective 4 \\(E\\) is now complete']),
    # ---- Archives
    'A1': ('archives', 0, f'{INV} GE_SKIPINTRO=0', f'{INV} GE_COOP_STEPS=p152@25', 60, 8,
           ['h:intro: overview camera ended .* by its timer', 'j:cutscene: the host.s intro of stage \\d+ is over', 'j:step p152 at \\d+ s done', 'b:objective 0 \\(A\\) is now complete']),
    'A3': ('archives', 0, INV, f'{INV} GE_COOP_STEPS=p152@10', 40, 2,
           ['b:objective 0 \\(A\\) is now complete']),
    # ---- Streets
    'SR2': ('streets', 0, INV, f'{INV} GE_COOP_STEPS=p248@10', 70, 2,
            ['=h:mission end', '=j:mission end']),
    # ---- Depot
    'DE4': ('depot', 0, INV, f'{INV} GE_COOP_STEPS=p271@10', 80, 2,
            ['=h:mission end', '=j:mission end']),
    # ---- Train
    'T1': ('train', 0, f'{INV} GE_COOP_WATCH=4', f'{INV} GE_COOP_WATCH=4 GE_COOP_STEPS=c4@10,k4@12', 40, 2,
           ['b:objective 1 \\(B\\) is now failed']),
    'T1-solo': ('train', 0, f'{INV} GE_COOP_WATCH=4 GE_COOP_STEPS=c4@10,k4@12', '', 40, 2,
                ['h:objective 1 \\(B\\) is now failed'], 1),
    # ---- Caverns
    'CA3': ('caverns', 0, INV, f'{INV} GE_COOP_STEPS=p369@10', 80, 2,
            ['=h:mission end', '=j:mission end']),
    # ---- Jungle / Control exits (SR2's note)
    'J-exit': ('jungle', 0, INV, f'{INV} GE_COOP_STEPS=p468@10', 80, 2,
               ['=h:mission end', '=j:mission end']),
    'C-exit': ('control', 0, INV, f'{INV} GE_COOP_STEPS=p168@10', 80, 2,
               ['=h:mission end', '=j:mission end']),
    'X4': ('dam', 0, f'{INV} GE_COOP_TESTWORLD=1 GE_COOP_TESTABORT=18:handover', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=p1@60', 90, 2,
           ['h:testabort \\(hand over\\)', 'j:host migration: player 2 hosts', 'j:watch: alarm on']),
    # ---- Across every mission
    'X3': ('dam', 0, f'{INV} GE_COOP_TESTWORLD=1 GE_COOP_WATCHWORLD=1', f'{INV} GE_COOP_WATCHWORLD=1 GE_COOP_STEPS=p1@50', 75, 18,
           ['j:watch: alarm on', 'j:watch: stage flags 4']),
    'X5': ('dam', 0, f'{INV} GE_COOP_TESTPAD=330:10 GE_COOP_TESTWIN=1', INV, 90, 24,
           ['=j:mission end', '=h:mission end']),
    'X8-doak3': ('facility', 1, INV, INV, 90, 2,
                 ['p3:step c79 at', '!p1:objective 1 \\(B\\) is now failed'], 3, {3: f'{INV} GE_COOP_STEPS=p11@10,c79@25'}),
    'X8-nat4': ('statue', 0, f'{INV} GE_COOP_TESTFLAG=8000:5', INV, 100, 2,
                ['p1:objective 3 \\(D\\) is now complete'], 4, {4: f'{INV} GE_COOP_TESTGOTO=0'}),
}


def add_sweep():
    """SW-<stage>: the joiner breaks or collects every destroy/collect target (objs.py, sweep.py), 00 Agent."""
    objs = subprocess.run([sys.executable, os.path.join(HERE, 'objs.py')], capture_output=True, text=True).stdout
    tmp = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'rows-objectives.txt')
    open(tmp, 'w').write(objs)
    lines = subprocess.run([sys.executable, os.path.join(HERE, 'sweep.py'), tmp], capture_output=True, text=True).stdout
    for line in lines.split('\n'):
        if line.count('|') == 2:
            st, steps, secs = line.split('|')
            ROWS['SW-' + st] = (st, 2, INV, f'{INV} GE_COOP_STEPS={steps}', int(secs), 2, ['objsame'])


def objstates(text):
    final, seq = {}, []
    for m in re.finditer(r'objective (\d+) \((\w)\) is now (\w+)', text):
        final[m.group(2)] = m.group(3)
        seq.append((m.group(2), m.group(3)))
    return final, seq


def logs_of(d, players):
    if players == 1:
        return {'h': os.path.join(d, 'host.log')}
    if players > 2:
        return {f'p{i}': os.path.join(d, f'p{i}.log') for i in range(1, players + 1)}
    return {'h': os.path.join(d, 'host.log'), 'j': os.path.join(d, 'join.log')}


def read(p):
    try:
        return open(p, errors='replace').read()
    except OSError:
        return ''


def judge(name, d, checks, players, je=''):
    L = {k: read(v) for k, v in logs_of(d, players).items()}
    if players == 2:
        L['b'] = None
    host = L.get('h', L.get('p1', ''))
    joiners = [v for k, v in L.items() if k not in ('h', 'p1', 'b')]
    why = []
    if not all('join snapshot applied' in j or 'following the host into stage' in j for j in joiners):
        why.append('a joiner never got in')
    alltext = ''.join(v for v in L.values() if v)
    if 'unresolved desync' in alltext:
        why.append('unresolved desync')
    if re.search(r'segmentation|signal 11|assertion', alltext, re.I):
        why.append('crash in a log')
    for c in checks:
        if c == 'objsame':
            (hf, hs), (jf, js) = objstates(L['h']), objstates(L['j'])
            for k in sorted(set(hf) | set(jf)):
                if hf.get(k) != jf.get(k):
                    why.append(f'objective {k}: host {hf.get(k)}, joiner {jf.get(k)}')
            flash = sorted({k for k, v in js if v == 'failed' and ('failed' not in [w for kk, w in hs if kk == k])})
            if flash:
                why.append('joiner-only failed flash: ' + ','.join(flash))
            if not hf:
                why.append('no objective changed')
            want = set(re.findall(r'(?:^|,)(\w+)@', (re.search(r'GE_COOP_STEPS=(\S+)', je or '') or [None, ''])[1]))
            done = set(re.findall(r'step (\w+) at \d+ s done', L['j']))
            if want - done:
                why.append(f"joiner's steps not all done (missing {','.join(sorted(want - done))})")
            continue
        neg = c.startswith('!')
        exact = c.startswith('=')
        cc = c[1:] if (neg or exact) else c
        who, rx = cc.split(':', 1)
        if who == 'b':
            texts = [('h', L['h']), ('j', L['j'])]
        else:
            texts = [(who, L.get(who, ''))]
        for w, t in texts:
            n = len(re.findall(rx, t or ''))
            if neg and n:
                why.append(f'{w} log has "{rx}"')
            elif exact and n != 1:
                why.append(f'{w} log has "{rx}" {n} times, not once')
            elif not neg and not exact and n == 0:
                why.append(f'{w} log lacks "{rx}"')
    return why


def run(name, out, slot):
    spec = ROWS[name]
    st, diff, he, je, secs, ja, checks = spec[:7]
    players = spec[7] if len(spec) > 7 else 2
    extra = spec[8] if len(spec) > 8 else {}
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, STAGE=st, DIFF=str(diff), SECS=str(int(secs * float(os.environ.get('SECSX', '2.5')))), SLOT=str(slot), JOINAFTER=str(ja))
    if players == 1:
        env.update(HOSTENV=he)
        script = os.path.join(HERE, 'solo.sh')
    elif players > 2:
        env.update(PLAYERS=str(players), P1ENV=he)
        for i in range(2, players + 1):
            env[f'P{i}ENV'] = extra.get(i, je)
        script = os.path.join(HERE, 'party.sh')
    else:
        env.update(HOSTENV=he, JOINENV=je)
        script = os.path.join(HERE, 'pair.sh')
    subprocess.run([script, d], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    why = judge(name, d, checks, players, je)
    return f"{name:9s} {'PASS' if not why else 'FAIL: ' + '; '.join(why)}"


def main():
    rejudge = sys.argv[1] == '--judge'
    if rejudge:
        del sys.argv[1]
    out = sys.argv[1]
    add_sweep()
    names = sys.argv[2:] or list(ROWS)
    if names == ['sweep']:
        names = [n for n in ROWS if n.startswith('SW-')]
    bad = [n for n in names if n not in ROWS]
    if bad:
        sys.exit('no such row: ' + ' '.join(bad))
    par = int(os.environ.get('PAR', '2'))
    os.makedirs(out, exist_ok=True)
    if rejudge:
        for n in names:
            spec = ROWS[n]
            why = judge(n, os.path.join(out, n), spec[6], spec[7] if len(spec) > 7 else 2, spec[3])
            print(f"{n:9s} {'PASS' if not why else 'FAIL: ' + '; '.join(why)}")
        return
    slots = queue.Queue()
    for i in range(par):
        slots.put(i)

    def one(name):
        slot = slots.get()
        try:
            return run(name, out, slot)
        finally:
            slots.put(slot)

    with ThreadPoolExecutor(par) as ex:
        res = list(ex.map(one, names))
    with open(os.path.join(out, 'summary.txt'), 'a') as f:
        for r in res:
            print(r)
            f.write(r + '\n')


if __name__ == '__main__':
    main()
