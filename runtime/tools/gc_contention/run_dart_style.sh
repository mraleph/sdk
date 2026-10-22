#!/bin/bash
# Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
#
# Runs the dart_style benchmark (format all files under a directory with a pool
# of worker isolates) several times and prints wall/user/sys time and peak RSS
# of each run followed by the median wall time.
#
# Usage:
#   run_dart_style.sh <dartaotruntime> <format.aot> <runs> [VM flags...]
#
# Environment (see dart_style.patch):
#   FORMAT_POOL_SIZE=8        number of worker isolates (default 8 here)
#   FORMAT_ISOLATE_GROUPS=1   spawn each worker in its own isolate group
#   FORMAT_SEQUENTIAL=1       format everything on the main isolate
#   FORMAT_DIR                directory to format (default: SDK tests/)
set -e
RUNTIME=$1
SNAPSHOT=$2
RUNS=$3
shift 3
SDK=$(cd "$(dirname "$0")/../../.." && pwd)
DIR=${FORMAT_DIR:-$SDK/tests}
export FORMAT_POOL_SIZE=${FORMAT_POOL_SIZE:-8}
TIME_FILE=$(mktemp)
WALLS=()
for i in $(seq 1 "$RUNS"); do
  /usr/bin/time -o "$TIME_FILE" -f "%e %U %S %M" "$RUNTIME" "$@" "$SNAPSHOT" \
      -o none "$DIR" > /dev/null 2>&1 || true
  read -r wall user sys rss < <(tail -1 "$TIME_FILE")
  echo "run $i: wall ${wall}s user ${user}s sys ${sys}s rss $((rss / 1024))MB"
  WALLS+=("$wall")
done
rm -f "$TIME_FILE"
printf '%s\n' "${WALLS[@]}" | sort -n | awk '{a[NR]=$1} END {
  m = (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2;
  printf "median wall: %.2fs\n", m }'
