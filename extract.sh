#!/bin/sh
# Copyright (c) 2020, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

# Helper to extract LLVM and Dart assembly and IL dumps from the full
# code dumps for the given variant.

set -xe

VARIANT=$1
PATTERN=$2
VARIANT_LLVM=$3

function ir4diff() {
    sed -r -e 's/\] [a-f0-9]+ k/\] XXXXXXXX k/g'    \
            -e 's/^[a-f0-9]+-[a-f0-9]+: (Function|null)/XXXXXXXX-XXXXXXXX: \1/g'  \
            -e 's/0x[a-f0-9]+/0xXXXXXXXX/g' \
            -e 's/0xX+\s+[a-f0-9]+\s+//g'   \
            -e 's/:[0-9]+//g'               \
            -e 's/^ *[0-9]+://g'            \
            -e 's/^ *(b\w+) [-+][0-9]+/\1 X/g' \
            -e 's/\<v[a-f0-9]+/vX/g' \
            -e '/^ *;; *B[0-9]+$/d'
}

function extract() {
  local suffix=$1
  local variant=$2
  dart extract.dart /tmp/output-${variant}-${suffix}.asm ${PATTERN} > /tmp/extracted-${variant}-${suffix}.asm
  cat /tmp/extracted-${variant}-${suffix}.asm | ir4diff > /tmp/extracted-${variant}-${suffix}.asm.4diff
}

if [[ -z "${VARIANT_LLVM}" ]]; then
  VARIANT_LLVM=${VARIANT}
fi

extract llvm ${VARIANT_LLVM} &
extract dart ${VARIANT} &
wait
echo "done"
code --diff /tmp/extracted-${VARIANT}-dart.asm.4diff /tmp/extracted-${VARIANT_LLVM}-llvm.asm.4diff

