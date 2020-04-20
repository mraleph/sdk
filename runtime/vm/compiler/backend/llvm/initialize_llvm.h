#ifndef INITIALIZE_LLVM_H
#define INITIALIZE_LLVM_H
#include "vm/compiler/backend/llvm/llvm_config.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
namespace dart {
namespace dart_llvm {
void InitLLVM(void);
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // INITIALIZE_LLVM_H
