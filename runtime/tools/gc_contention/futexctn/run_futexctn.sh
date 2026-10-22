#!/bin/bash
# Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
# Runs a command under the modified futexctn (see futexctn.patch) and saves the
# raw results for offline analysis with analyze_futexctn.py.
#
# Needs root (the command then runs as $SUDO_USER) or a futexctn binary with
# cap_bpf, cap_perfmon and cap_dac_read_search (see README.md).
#
# Usage:
#   [sudo] ./run_futexctn.sh <futexctn binary> <output dir> [-a] -- <command...>
#
#   -a  also record FUTEX_WAIT_BITSET waits (condition variables, i.e.
#       Monitor::Wait), not only pthread mutex waits.
#
# Output in <output dir>:
#   futexctn.txt  raw per (lock address, user stack) results
#   maps.txt      /proc/<pid>/maps of the command (for symbolization)
#   wall.txt      wall time of the command in seconds
set -e
FUTEXCTN=$1
OUT=$2
shift 2
EXTRA=""
if [[ $1 == -a ]]; then
  EXTRA="-a"
  shift
fi
[[ $1 == -- ]] && shift
mkdir -p "$OUT"

DROP=()
if [[ -n $SUDO_UID ]]; then
  # setpriv execs the command, so it keeps the PID we trace.
  DROP=(setpriv --reuid="$SUDO_UID" --regid="$SUDO_GID" --init-groups)
fi

# Start the command stopped so that futexctn is attached before it runs.
"${DROP[@]}" /bin/bash -c 'kill -STOP $$; exec "$@"' bash "$@" \
    > "$OUT/stdout.txt" 2> "$OUT/stderr.txt" &
PID=$!
while [[ $(ps -o stat= -p "$PID") != T* ]]; do sleep 0.01; done

# Distinct (lock, stack) pairs exceed 64K when stacks go through many Dart
# call sites. A large stack map also keeps stack hash collisions rare. The
# maps take about 0.5 GB of kernel memory.
"$FUTEXCTN" -M -r $EXTRA --perf-max-stack-depth 32 \
    --stack-storage-size 1048576 --max-entries 1048576 -p "$PID" \
    > "$OUT/futexctn.txt" 2> "$OUT/futexctn.err" &
TRACER=$!
# futexctn prints this once the BPF programs are attached.
until grep -q "hit ctrl-c" "$OUT/futexctn.err" 2> /dev/null; do sleep 0.05; done

START=$(date +%s.%N)
kill -CONT "$PID"
sleep 1
cp "/proc/$PID/maps" "$OUT/maps.txt" || true
wait "$PID" || true
END=$(date +%s.%N)
awk -v s="$START" -v e="$END" 'BEGIN { printf "%.2f\n", e - s }' > "$OUT/wall.txt"

kill -INT "$TRACER"
wait "$TRACER" || true
[[ -n $SUDO_UID ]] && chown -R "$SUDO_UID:$SUDO_GID" "$OUT"
echo "wall time $(cat "$OUT/wall.txt") s; results in $OUT"
