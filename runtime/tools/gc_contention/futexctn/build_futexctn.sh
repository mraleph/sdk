#!/bin/bash
# Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
#
# Builds futexctn from bcc's libbpf-tools with futexctn.patch applied.
#
# Usage:
#   build_futexctn.sh <work dir>
#
# Requires git, clang with the BPF target, libelf-dev and zlib1g-dev. Prints
# the path of the built binary.
set -e
WORK=$1
[[ -n $WORK ]] || { echo "usage: $0 <work dir>" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=$(cd "$HERE/../../../.." && pwd)
BCC_REV=c398a9a7a41cba623b520cda27457b92d7fcad2b

mkdir -p "$WORK"
cd "$WORK"
if [[ ! -d bcc ]]; then
  git clone -q https://github.com/iovisor/bcc.git
fi
cd bcc
git checkout -q "$BCC_REV"
git submodule update -q --init --depth 1 src/cc/libbpf libbpf-tools/bpftool
(cd libbpf-tools/bpftool && git submodule update -q --init --depth 1)
git checkout -q -- libbpf-tools/futexctn.c libbpf-tools/futexctn.bpf.c \
    libbpf-tools/futexctn.h
git apply "$HERE/futexctn.patch"

# Use llvm-strip from the SDK toolchain if none is installed.
LLVM_STRIP=$(command -v llvm-strip || echo "$SDK/buildtools/linux-x64/clang/bin/llvm-strip")
make -C libbpf-tools -j"$(nproc)" futexctn USE_BLAZESYM=0 \
    LLVM_STRIP="$LLVM_STRIP" > build.log 2>&1 || { tail -20 build.log; exit 1; }
echo "$WORK/bcc/libbpf-tools/futexctn"
