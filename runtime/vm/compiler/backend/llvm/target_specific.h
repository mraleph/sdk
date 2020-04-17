// Copyright 2019 UCWeb Co., Ltd.
#ifndef TARGET_SPECIFIC_H
#define TARGET_SPECIFIC_H
#include "vm/compiler/backend/llvm/llvm_config.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#if defined(TARGET_ARCH_ARM)
static const int kV8CCRegisterParameterCount = 12;
#else
#error unsupported arch
#endif
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // TARGET_SPECIFIC_H
