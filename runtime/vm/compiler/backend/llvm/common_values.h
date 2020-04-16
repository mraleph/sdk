// Copyright 2019 UCWeb Co., Ltd.

#ifndef COMMON_VALUES_H
#define COMMON_VALUES_H
#include "vm/compiler/backend/llvm/abbreviations.h"

#if defined(DART_ENABLE_LLVM_COMPILER)
namespace dart {
namespace dart_llvm {

class CommonValues {
 public:
  CommonValues(LContext context);
  CommonValues(const CommonValues&) = delete;
  const CommonValues& operator=(const CommonValues&) = delete;

  void initialize(LModule module) { module_ = module; }

  const LType voidType;
  const LType boolean;
  const LType int8;
  const LType int16;
  const LType int32;
  const LType int64;
  const LType intPtr;
  const LType floatType;
  const LType doubleType;
  const LType tokenType;
  const LType ref8;
  const LType ref16;
  const LType ref32;
  const LType ref64;
  const LType refPtr;
  const LType refFloat;
  const LType refDouble;
  const LType taggedType;
  const LType metaType;
  const LValue booleanTrue;
  const LValue booleanFalse;
  const LValue int8Zero;
  const LValue int32Zero;
  const LValue int32One;
  const LValue int64Zero;
  const LValue intPtrZero;
  const LValue intPtrOne;
  const LValue intPtrTwo;
  const LValue intPtrThree;
  const LValue intPtrFour;
  const LValue intPtrEight;
  const LValue intPtrPtr;
  const LValue doubleZero;

  const unsigned rangeKind;
  const unsigned profKind;
  const LValue branchWeights;

  LContext const context_;
  LModule module_;
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // COMMON_VALUES_H
