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
static const size_t kRuntimeCallInstrSize = Instr::kInstrSize;
static const size_t kRuntimeCallReturnOnStackInstrSize = 2 * Instr::kInstrSize;

// instance of
static const Register kInstanceOfInstanceReg = R0;
static const Register kInstanceOfFunctionTypeReg = R1;
static const Register kInstanceOfInstantiatorTypeReg = R2;

// array
static const Register kCreateArrayLengthReg = R2;
static const Register kCreateArrayElementTypeReg = R1;
#else
#error unsupported arch
#endif
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // TARGET_SPECIFIC_H
