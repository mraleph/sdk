#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Symbolizes and summarizes raw output of the modified futexctn.

Usage:
  analyze_futexctn.py --binary <dartaotruntime> [--json out.json] <dir>...

Reports futex wait time (time threads spent blocked in the kernel waiting for a
contended lock) by thread role, by call site and by lock address. Times are
summed over threads. With several run directories (of the same binary) times
are averaged over runs and the per-lock breakdown is omitted.
"""

import argparse
import bisect
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

ROLES = [
    ('concurrent sweeper task', ('ConcurrentSweeperTask', )),
    ('concurrent marker task', ('ConcurrentMarkTask', )),
    ('STW marking task', ('ParallelMarkTask', 'MarkObjects')),
    ('mutator: marking assist', ('IncrementalMarkWith', )),
    ('mutator: sweeping assist', ('IncrementalSweepWithSizeBudget', )),
    ('mutator: write barrier', ('MarkingStackBlockProcess', 'MarkingStackAcquire',
                                'MarkingStackRelease', 'StoreBufferBlockProcess')),
    ('mutator: external allocation check',
     ('CheckExternalGC', 'Dart_TypedDataReleaseData', 'AllocatedExternal')),
    ('mutator: marking phase check', ('CheckConcurrentMarking', )),
    ('mutator: TLAB refill', ('TryAllocateNewTLAB', 'RefillTLAB',
                              'AbandonRemainingTLAB', 'Heap::AllocateNew')),
    ('scavenger task: prune marking stacks', ('PruneNew', 'PruneDeferred')),
    ('scavenger task', ('ScavengerVisitor', 'ScavengerTask', 'Scavenger::')),
    ('concurrent marker task', ('DrainMarkingStackWithPauseChecks', )),
    ('sweeper', ('PageSpace::Sweep', 'GCSweeper')),
    ('safepoint', ('Safepoint', )),
    ('other thread pool thread', ('ThreadPool', )),
]
OPS = {0: 'mutex (FUTEX_WAIT)', 9: 'condvar (FUTEX_WAIT_BITSET)'}


def exec_vaddr_delta(path, pgoff):
    out = subprocess.run(['readelf', '-lW', path], capture_output=True,
                         text=True).stdout
    for m in re.finditer(r'LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+'
                         r'0x([0-9a-f]+)\s+0x[0-9a-f]+\s+R E', out):
        off, vaddr, size = (int(x, 16) for x in m.groups())
        if (off & ~0xfff) <= pgoff < off + size:
            return vaddr - off
    return 0


def nm_symbols(path):
    syms = []
    for args in (['nm', '-C', '--defined-only', path],
                 ['nm', '-C', '-D', '--defined-only', path]):
        for line in subprocess.run(args, capture_output=True,
                                   text=True).stdout.splitlines():
            f = line.split(' ', 2)
            if len(f) == 3 and f[1] in 'TtWi':
                syms.append((int(f[0], 16), f[2]))
        if syms:
            break
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def analyze(binary, run_dir):
    maps = []
    for line in open(os.path.join(run_dir, 'maps.txt')):
        f = line.split()
        if len(f) >= 6 and 'x' in f[1] and f[5].startswith('/'):
            start, end = (int(x, 16) for x in f[0].split('-'))
            maps.append((start, end, int(f[2], 16), f[5]))
    maps.sort()
    starts = [m[0] for m in maps]

    entries = []
    dropped = None
    for line in open(os.path.join(run_dir, 'futexctn.txt')):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'FUTEXCTN_DROPPED':
            dropped = dict(waits=int(f[1]), seconds=int(f[2]) / 1e6,
                           stack_errors=int(f[3]))
        if f[0] != 'FUTEXCTN':
            continue
        ips = [int(x, 16) for x in f[7].split(',')] if len(f) > 7 and f[7] else []
        entries.append(dict(op=int(f[2]), uaddr=f[3], count=int(f[4]),
                            total_us=int(f[5]), max_us=int(f[6]), ips=ips))

    def mapping(ip):
        i = bisect.bisect_right(starts, ip) - 1
        if i >= 0 and ip < maps[i][1]:
            return maps[i]
        return None

    binname = os.path.basename(binary)
    vm_addrs = defaultdict(set)
    other = {}
    for e in entries:
        for ip in e['ips']:
            m = mapping(ip)
            if m and os.path.basename(m[3]) == binname:
                vm_addrs[m].add(ip)
    vm_names = {}
    for m, ips in vm_addrs.items():
        delta = exec_vaddr_delta(binary, m[2])
        ips = sorted(ips)
        addrs = [ip - m[0] + m[2] + delta - 1 for ip in ips]
        for i in range(0, len(addrs), 4000):
            out = subprocess.run(['addr2line', '-a', '-f', '-i', '-C', '-e',
                                  binary] + [hex(a) for a in addrs[i:i + 4000]],
                                 capture_output=True, text=True).stdout.splitlines()
            cur, k = None, 0
            for line in out:
                if re.fullmatch(r'0x[0-9a-f]+', line):
                    cur, k = int(line, 16), 0
                    vm_names[cur] = []
                elif cur is not None:
                    if k % 2 == 0:
                        vm_names[cur].append(re.sub(r'\(.*$', '', line).replace('dart::', ''))
                    k += 1
        for ip, a in zip(ips, addrs):
            other[ip] = vm_names.get(a, ['?'])
    nm_cache = {}

    def frame_names(ip):
        if ip in other:
            return other[ip]
        m = mapping(ip)
        if m is None:
            return ['[unknown]']
        path = m[3]
        if path not in nm_cache:
            nm_cache[path] = nm_symbols(path)
        addrs, names = nm_cache[path]
        # Shared objects: symbols are relative to the load address.
        off = ip - m[0] + m[2]
        j = bisect.bisect_right(addrs, off) - 1
        name = names[j] if j >= 0 else '?'
        return [f'{os.path.basename(path)}:{name}']

    by_op = Counter()
    by_role = Counter()
    by_site = Counter()
    by_lock = defaultdict(lambda: [0, 0, 0, Counter(), Counter()])
    for e in entries:
        t = e['total_us'] / 1e6
        by_op[OPS.get(e['op'], e['op'])] += t
        full = [n for ip in e['ips'] for n in frame_names(ip)]
        outer = [frame_names(ip)[-1] for ip in e['ips']]
        text = ' | '.join(full)
        if not e['ips']:
            role = '[no stack]'
        else:
            role = next((r for r, ps in ROLES if any(p in text for p in ps)), 'other')
        vm = [x for x in outer if not x.startswith('libc')]
        site = ' <- '.join(vm[:4]) or '[no stack]'
        key = f'{OPS.get(e["op"], e["op"]).split()[0]}: '
        by_role[key + role] += t
        by_site[key + site] += t
        lk = by_lock[e['uaddr']]
        lk[0] += t
        lk[1] += e['count']
        lk[2] = max(lk[2], e['max_us'])
        lk[3][role] += t
        lk[4][site] += t
    wall = float(open(os.path.join(run_dir, 'wall.txt')).read().strip())
    locks = {k: dict(seconds=v[0], waits=v[1], max_ms=v[2] / 1000,
                     roles=dict(v[3].most_common()),
                     sites=dict(v[4].most_common(5)))
             for k, v in by_lock.items()}
    if dropped is None:
        print(f'warning: {run_dir}: no FUTEXCTN_DROPPED line, futexctn is too '
              'old to report waits lost to a full map', file=sys.stderr)
    elif dropped['waits']:
        print(f'warning: {run_dir}: {dropped["waits"]} waits '
              f'({dropped["seconds"]:.2f} s) not recorded, increase '
              '--max-entries', file=sys.stderr)
    return dict(wall=wall, by_op=dict(by_op), by_role=dict(by_role),
                by_site=dict(by_site), by_lock=locks, dropped=dropped)


def average(runs):
    n = len(runs)
    result = dict(runs=n, wall=sum(r['wall'] for r in runs) / n)
    for key in ('by_op', 'by_role', 'by_site'):
        total = Counter()
        for r in runs:
            total.update(r[key])
        result[key] = {k: v / n for k, v in total.most_common()}
    if n == 1:
        result['by_lock'] = runs[0]['by_lock']
        result['dropped'] = runs[0]['dropped']
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True,
                        help='dartaotruntime (must match the traced binary)')
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('--json', help='also write the summary to this file')
    parser.add_argument('dirs', nargs='+')
    args = parser.parse_args()

    res = average([analyze(args.binary, d) for d in args.dirs])
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(res, f, indent=1)

    def top(d, n=None):
        return sorted(d.items(), key=lambda kv: -kv[1])[:n]

    print(f'runs: {res["runs"]}, mean wall time: {res["wall"]:.2f} s')
    print('\nfutex wait time by kind (summed over threads):')
    for k, v in top(res['by_op']):
        print(f'  {v:9.3f} s  {k}')
    print('\nby thread role:')
    for k, v in top(res['by_role']):
        print(f'  {v:9.3f} s  {k}')
    print('\nby call site (first VM frames):')
    for k, v in top(res['by_site'], args.top):
        print(f'  {v:9.3f} s  {k}')
    if 'by_lock' in res:
        print('\nby lock address:')
        for k, v in sorted(res['by_lock'].items(),
                           key=lambda kv: -kv[1]['seconds'])[:args.top]:
            roles = ', '.join(f'{r} {t:.2f}s' for r, t in list(v['roles'].items())[:3])
            print(f'  {v["seconds"]:9.3f} s  {v["waits"]:9d} waits  '
                  f'max {v["max_ms"]:.2f} ms  {k}  ({roles})')
            for site, t in list(v['sites'].items())[:3]:
                print(f'      {t:9.3f} s  {site}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
