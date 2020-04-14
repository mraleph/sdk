#!/bin/sh
# Copyright (c) 2020, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

# Helper to generate machine code from app.dill using LLVM and baseline Dart
# implementation.

# app.dill is Flutter Gallery build at commit 8374282631cf179d42a5d100602d77c1e124d9f3
# in release mode using Flutter SDK at commit 5f21edf8b66e31a39133177319414395cc5b5f48

set -xe

MODE=Product
ARCH=SIMARM64
DILL_FILE=./app.dill
VARIANT=$1

PREVIOUS_VARIANT=$2

# Note: LLVM does not use multiple entry points so Dart baseline should not
# either for apples-to-apples comparison.
GLOBAL_OPTS=--no_enable_multiple_entrypoints
#GLOBAL_OPTS=

PFG="--print-flow-graph-optimized --disassemble"


function compile() {
  local suffix
  local opts
  if [[ "$1" == "llvm" ]]; then
    suffix="llvm"
    opts="--llvm-compiler"
  else
    suffix="dart"
    opts="--no-llvm-compiler"
  fi
  echo "Building ${suffix}"
  env LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH out/${MODE}${ARCH}/gen_snapshot \
    --deterministic \
    --snapshot_kind=app-aot-elf \
    --elf=/tmp/app-${VARIANT}-${suffix}.so \
    --strip \
    --no-causal-async-stacks \
    --lazy-async-stacks \
    ${GLOBAL_OPTS} \
    ${opts} \
    --print-snapshot-sizes      \
    --print-instructions-sizes-to=/tmp/sizes-${VARIANT}-${suffix}.json \
    ${PFG} \
    ${DILL_FILE} > /tmp/output-${VARIANT}-${suffix}.asm 2>&1
}

time compile dart &
if [[ "${PREVIOUS_VARIANT}" == "" ]]; then
  time compile llvm &
else
  echo "SKIPPING COMPILING WITH LLVM - COMPARING AGAINST ${PREVIOUS_VARIANT}"
fi
wait

if [[ "${PREVIOUS_VARIANT}" == "" ]]; then
  dart pkg/vm/bin/compare_sizes.dart /tmp/sizes-${VARIANT}-dart.json /tmp/sizes-${VARIANT}-llvm.json > /tmp/comparison-${VARIANT}.txt
else
  dart pkg/vm/bin/compare_sizes.dart /tmp/sizes-${VARIANT}-dart.json /tmp/sizes-${PREVIOUS_VARIANT}-llvm.json > /tmp/comparison-${VARIANT}.txt
  dart pkg/vm/bin/compare_sizes.dart /tmp/sizes-${PREVIOUS_VARIANT}-dart.json /tmp/sizes-${VARIANT}-dart.json > /tmp/comparison-${VARIANT}-vs-${PREVIOUS_VARIANT}.txt
fi