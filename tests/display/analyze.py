#!/usr/bin/env python3
"""Evaluates a tests/display/run.sh run.

    analyze.py <work dir> [--gap-limit MS] [--fps-ratio R] [--settle-ms MS] [--title TEXT]

Reads from the work dir:
  samples.log  "second at=<epoch ms> frames=<n> max_gap_ms=<ms> ..." one line per second
               (each covers the second before `at`)
  phases.log   "phase <name> <start ms> <end ms> <on|off>" expected display power per phase
  dpms.log     "<epoch ms> <On|Off|Standby|Suspend>" sampled display power state
  java.log     optional, "EVENT invalid|valid <epoch ms>" from the bot client's isValid()

A phase passes when, after the settle time following its transition, its frame rate stays at
>= fps-ratio of the first ("visible") phase, no gap between two game-thread frames exceeds
gap-limit, no second is without frames and the client never turned invalid. Seconds in which
the display wasn't in the phase's power state (woken by input, ...) don't count; a phase
left with too few seconds is inconclusive. Exit code: 0 pass, 1 fail, 2 inconclusive.
"""
import argparse
import os
import statistics
import sys


def kv(line):
    return dict(tok.split('=', 1) for tok in line.split() if '=' in tok)


def load(path, parse):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [r for r in (parse(l.strip()) for l in f) if r is not None]


def parse_sample(line):
    if not line.startswith('second '):
        return None
    d = kv(line)
    try:
        return int(d['at']), int(d['frames']), int(d['max_gap_ms'])
    except (KeyError, ValueError):
        return None


def parse_phase(line):
    p = line.split()
    if len(p) < 5 or p[0] != 'phase':
        return None
    return p[1], int(p[2]), int(p[3]), p[4]


def parse_dpms(line):
    p = line.split()
    return (int(p[0]), p[1]) if len(p) == 2 and p[0].isdigit() else None


def parse_java(line):
    p = line.split()
    return (int(p[2]), p[1] == 'valid') if len(p) == 3 and p[0] == 'EVENT' else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work')
    ap.add_argument('--gap-limit', type=int, default=250)
    ap.add_argument('--fps-ratio', type=float, default=0.8)
    ap.add_argument('--settle-ms', type=int, default=2000)
    ap.add_argument('--title', default='display test')
    a = ap.parse_args()

    samples = load(os.path.join(a.work, 'samples.log'), parse_sample)
    phases = load(os.path.join(a.work, 'phases.log'), parse_phase)
    dpms = load(os.path.join(a.work, 'dpms.log'), parse_dpms)
    validity = load(os.path.join(a.work, 'java.log'), parse_java)
    if not phases:
        print('no phases recorded')
        return 2

    def display_ok(t0, t1, expect):
        states = [s for t, s in dpms if t0 < t <= t1]
        return all((s == 'On') == (expect == 'on') for s in states)

    def invalid_during(t0, t1):
        state = True  # valid until told otherwise
        for t, ok in validity:
            if t <= t0:
                state = ok
            elif t <= t1 and not ok:
                return True
        return not state

    rows, baseline = [], None
    for name, start, end, expect in phases:
        lo = start + a.settle_ms
        in_phase = [s for s in samples if s[0] - 1000 >= lo - 50 and s[0] <= end + 50]
        settle = [s for s in samples if start < s[0] and s[0] - 1000 < lo - 50]
        good = [s for s in in_phase if display_ok(s[0] - 1000, s[0], expect)]
        expected = max(1, (end - lo) // 1000)
        row = {
            'name': name, 'expect': expect, 'secs': len(good), 'woken': len(in_phase) - len(good),
            'fps_med': statistics.median([s[1] for s in good]) if good else 0,
            'fps_min': min((s[1] for s in good), default=0),
            'gap': max((s[2] for s in good), default=0),
            'zero': sum(1 for s in good if s[1] == 0),
            'settle_gap': max((s[2] for s in settle), default=0),
            'invalid': invalid_during(start, end),
            'enough': len(good) >= max(3, expected // 2),
        }
        if baseline is None:
            baseline = row
        rows.append(row)

    ref = baseline['fps_med']
    worst = 0  # 0 pass, 1 fail, 2 inconclusive (fail wins)
    for r in rows:
        problems = []
        if not r['enough']:
            r['result'] = 'INCONCLUSIVE (' + (f"display not {r['expect']} for {r['woken']} s, woken by input?"
                                             if r['woken'] else 'no samples') + ')'
            worst = max(worst, 2) if worst != 1 else 1
            continue
        if r['gap'] > a.gap_limit:
            problems.append(f"gap {r['gap']} ms > {a.gap_limit} ms")
        if r is not baseline and r['fps_med'] < a.fps_ratio * ref:
            problems.append(f"{r['fps_med']:g}/s < {a.fps_ratio:.0%} of {ref:g}/s")
        if r['zero']:
            problems.append(f"{r['zero']} s without a frame")
        if r['invalid']:
            problems.append('client reported invalid')
        r['result'] = 'FAIL: ' + '; '.join(problems) if problems else 'ok'
        if problems:
            worst = 1

    out = [f"Display test: {a.title}",
           f"  reference {ref:g} frames/s (first phase), gap limit {a.gap_limit} ms, "
           f"first {a.settle_ms / 1000:g} s after each change not judged",
           '',
           f"  {'phase':<22}{'display':<9}{'secs':>5}{'frames/s':>10}{'min':>6}{'worst gap':>11}"
           f"{'(at change)':>13}  result"]
    for r in rows:
        woken = f" +{r['woken']} woken" if r['woken'] else ''
        out.append(f"  {r['name']:<22}{r['expect']:<9}{r['secs']:>5}{r['fps_med']:>10g}{r['fps_min']:>6}"
                   f"{str(r['gap']) + ' ms':>11}{str(r['settle_gap']) + ' ms':>13}  {r['result']}{woken}")
    out.append('')
    out.append({0: 'PASS: the game thread kept its frame rate in every phase',
                1: 'FAIL: the game thread slowed down or stalled (see above)',
                2: 'INCONCLUSIVE: some phases had too little data (display woken during the test?)'}[worst])
    text = '\n'.join(out)
    print(text)
    with open(os.path.join(a.work, 'result.txt'), 'w') as f:
        f.write(text + '\n')
    return worst


if __name__ == '__main__':
    sys.exit(main())
