// Copyright 2019 UCWeb Co., Ltd.

#ifndef LLVM_COMPILE_H
#define LLVM_COMPILE_H
#include "vm/compiler/backend/llvm/llvm_config.h"
#if defined(DART_ENABLE_LLVM_COMPILER)

namespace dart {
namespace dart_llvm {
struct CompilerState;
void Compile(CompilerState& state);

}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // LLVM_COMPILE_H
