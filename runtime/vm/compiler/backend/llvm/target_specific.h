// Copyright 2019 UCWeb Co., Ltd.
#ifndef TARGET_SPECIFIC_H
#define TARGET_SPECIFIC_H
#include "vm/compiler/backend/llvm/llvm_config.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/constants.h"
namespace dart {
#if defined(TARGET_ARCH_ARM)
static const int kV8CCRegisterParameterCount = 12;
static const Register kReceiverReg = R0;
static const Register kICReg = R9;
static const size_t kInstanceCallTargetReg = R1;
static const size_t kNativeCallInstrSize = 12 * Instr::kInstrSize;
#else
#error unsupported arch
#endif
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // TARGET_SPECIFIC_H
