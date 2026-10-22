#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Off-CPU (blocked) time analysis from perf context switch events.

Works without root and without patching the VM:

  perf record -e context-switches -c 1 --call-graph fp --switch-events \\
      -o cs.data <command>
  perf script -i cs.data -F tid,time,event,ip,sym,dso --show-switch-events \\
      --show-mmap-events > cs.txt
  offcpu_from_perf.py --binary <dartaotruntime> cs.txt

Each context switch sample carries the stack at which a thread blocked. The
time until the thread is switched back in is attributed to that stack. Waits
in pthread_mutex_lock (__lll_lock_wait) are lock contention; condition
variable waits, epoll and preemption are reported separately. Covers all
threads, including GC helper threads.

Needs perf_event_paranoid <= 1 (kernel frames need <= -1).
"""

import argparse
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
]


def vaddr_delta(binary, file_offset):
    """Returns p_vaddr - p_offset of the segment containing file_offset."""
    out = subprocess.run(['readelf', '-lW', binary], capture_output=True,
                         text=True).stdout
    for m in re.finditer(r'LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+'
                         r'0x([0-9a-f]+)\s+0x[0-9a-f]+\s+R E', out):
        off, vaddr, size = (int(x, 16) for x in m.groups())
        if (off & ~0xfff) <= file_offset < off + size:
            return vaddr - off
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('script_output')
    args = parser.parse_args()
    binname = args.binary.split('/')[-1]

    base = text_start = text_end = None  # mapping of the binary
    samples = []  # (tid, time, [(ip, dso)])
    switches = defaultdict(list)  # tid -> [(time, 'IN'/'OUT', preempt)]
    cur = None
    with open(args.script_output, errors='replace') as f:
        for line in f:
            if line.startswith('\t'):
                if cur is not None:
                    m = re.match(r'\t\s*([0-9a-f]+) (.*) \((.*)\)\s*$', line)
                    if m:
                        ip = int(m.group(1), 16)
                        # perf prints inlined frames as extra lines with the
                        # same address; keep one entry per address.
                        if cur[2] and cur[2][-1][0] == ip:
                            cur[2][-1][2].append(m.group(2))
                        else:
                            cur[2].append((ip, m.group(3), [m.group(2)]))
                continue
            if cur is not None:
                samples.append(cur)
                cur = None
            m = re.match(r'\s*(\d+)\s+([\d.]+):\s+(.*)', line)
            if not m:
                continue
            tid, t, rest = int(m.group(1)), float(m.group(2)), m.group(3)
            if rest.startswith('PERF_RECORD_SWITCH'):
                switches[tid].append((t, 'OUT' if ' OUT' in rest else 'IN',
                                      'preempt' in rest))
            elif rest.startswith('PERF_RECORD_MMAP') and binname in rest and \
                    'r-xp' in rest and base is None:
                mm = re.search(r'\[0x([0-9a-f]+)\((0x[0-9a-f]+)\) @ 0x([0-9a-f]+)', rest)
                if mm:
                    text_start = int(mm.group(1), 16)
                    text_end = text_start + int(mm.group(2), 16)
                    pgoff = int(mm.group(3), 16)
                    base = text_start - pgoff - vaddr_delta(args.binary, pgoff)
            elif rest.startswith('context-switches'):
                cur = (tid, t, [])
    if cur is not None:
        samples.append(cur)
    if base is None:
        sys.exit('could not find the mapping of the binary; '
                 'use --show-mmap-events')

    # Symbolize VM frames with inlining.
    in_vm = lambda ip: text_start <= ip < text_end
    offs = sorted({ip - base for _, _, st in samples for ip, _, _ in st
                   if in_vm(ip)})
    names = {}
    for i in range(0, len(offs), 4000):
        chunk = offs[i:i + 4000]
        out = subprocess.run(['addr2line', '-a', '-f', '-i', '-C', '-e',
                              args.binary] + [hex(o) for o in chunk],
                             capture_output=True, text=True).stdout.splitlines()
        cur_off, k = None, 0
        for line in out:
            if re.fullmatch(r'0x[0-9a-f]+', line):
                cur_off, k = int(line, 16), 0
                names[cur_off] = []
            elif cur_off is not None:
                if k % 2 == 0:
                    names[cur_off].append(re.sub(r'\(.*$', '', line).replace('dart::', ''))
                k += 1

    def frames(st, inline=True):
        out = []
        for ip, dso, syms in st:
            if in_vm(ip):
                chain = names.get(ip - base, ['?'])
                out.extend(chain if inline else chain[-1:])
            elif ip >= 0xffff000000000000:
                continue
            else:
                out.extend(syms)
        return out

    # Pair each sample with the switch-out after it and the next switch-in.
    def wait_time(tid, t):
        sw = switches[tid]
        lo, hi = 0, len(sw)
        while lo < hi:
            mid = (lo + hi) // 2
            if sw[mid][0] < t:
                lo = mid + 1
            else:
                hi = mid
        out_t = preempt = None
        for j in range(lo, len(sw)):
            if out_t is None and sw[j][1] == 'OUT':
                out_t, preempt = sw[j][0], sw[j][2]
            elif out_t is not None and sw[j][1] == 'IN':
                return sw[j][0] - out_t, preempt
        return 0.0, False

    kinds = Counter()
    lock_sites = Counter()
    lock_roles = Counter()
    for tid, t, st in samples:
        dur, preempt = wait_time(tid, t)
        fr = frames(st)
        text = ' | '.join(fr)
        is_lock = '__lll_lock_wait' in text
        if preempt:
            kinds['preempted'] += dur
        elif is_lock:
            kinds['mutex wait (pthread_mutex_lock)'] += dur
            # One name per physical frame (the outermost inlined function).
            fr1 = frames(st, inline=False)
            vm = fr1[[i for i, x in enumerate(fr1) if '__lll_lock_wait' in x][-1] + 1:]
            vm = [x for x in vm if not any(w in x for w in (
                'MutexLocker', 'MonitorLocker', 'Mutex::Lock', 'Monitor::Enter'))]
            lock_sites[' <- '.join(vm[:4])] += dur
            role = next((r for r, ps in ROLES if any(p in text for p in ps)), 'other')
            lock_roles[role] += dur
        elif 'pthread_cond' in text or 'ConditionVariable' in text or 'Monitor::Wait' in text:
            kinds['condition variable wait'] += dur
        else:
            kinds['other blocking (I/O, epoll, ...)'] += dur

    print('off-CPU time by kind (summed over threads):')
    for k, v in kinds.most_common():
        print(f'  {v:9.2f} s  {k}')
    print('\nmutex waits by thread role:')
    for k, v in lock_roles.most_common():
        print(f'  {v:9.2f} s  {k}')
    print('\nmutex waits by call site:')
    for k, v in lock_sites.most_common(args.top):
        print(f'  {v:9.3f} s  {k}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
