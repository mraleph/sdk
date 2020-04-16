#ifndef LLVM_CONFIG_H
#define LLVM_CONFIG_H
#include "vm/globals.h"
#if defined(UC_BUILD_LLVM_COMPILER) && defined(DART_PRECOMPILER)
#define DART_ENABLE_LLVM_COMPILER 1
#endif

// test arch
#if defined(DART_ENABLE_LLVM_COMPILER)
#if defined(TARGET_ARCH_ARM)
#else
#undef DART_ENABLE_LLVM_COMPILER
#endif
#endif
#endif  // LLVM_CONFIG_H
