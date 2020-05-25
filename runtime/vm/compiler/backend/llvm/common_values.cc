// Copyright 2019 UCWeb Co., Ltd.

#include "vm/compiler/backend/llvm/common_values.h"

#if defined(DART_ENABLE_LLVM_COMPILER)
namespace dart {
namespace dart_llvm {

CommonValues::CommonValues(LContext context)
    : void_type(dart_llvm::voidType(context)),
      boolean(int1Type(context)),
      int8(int8Type(context)),
      int16(int16Type(context)),
      int32(int32Type(context)),
      int64(int64Type(context)),
      intPtr(intPtrType(context)),
      floatType(dart_llvm::floatType(context)),
      doubleType(dart_llvm::doubleType(context)),
      tokenType(LLVMTokenTypeInContext(context)),
      ref8(pointerType(int8)),
      ref16(pointerType(int16)),
      ref32(pointerType(int32)),
      ref64(pointerType(int64)),
      refPtr(pointerType(intPtr)),
      refFloat(pointerType(floatType)),
      refDouble(pointerType(doubleType)),
      // address space 1 means gc recognizable.
      tagged_type(
          LLVMPointerType(LLVMStructCreateNamed(context, "TaggedStruct"), 1)),
      meta_type(LLVMMetadataTypeInContext(context)),
      int32x4(LLVMVectorType(int32, 4)),
      float32x4(LLVMVectorType(floatType, 4)),
      float64x2(LLVMVectorType(doubleType, 2)),
      booleanTrue(constInt(boolean, true, ZeroExtend)),
      booleanFalse(constInt(boolean, false, ZeroExtend)),
      int8Zero(constInt(int8, 0, SignExtend)),
      int32Zero(constInt(int32, 0, SignExtend)),
      int32One(constInt(int32, 1, SignExtend)),
      int64Zero(constInt(int64, 0, SignExtend)),
      intPtrZero(constInt(intPtr, 0, SignExtend)),
      intPtrOne(constInt(intPtr, 1, SignExtend)),
      intPtrTwo(constInt(intPtr, 2, SignExtend)),
      intPtrThree(constInt(intPtr, 3, SignExtend)),
      intPtrFour(constInt(intPtr, 4, SignExtend)),
      intPtrEight(constInt(intPtr, 8, SignExtend)),
      intPtrPtr(constInt(intPtr, sizeof(void*), SignExtend)),
      doubleZero(constReal(doubleType, 0)),
      rangeKind(mdKindID(context, "range")),
      profKind(mdKindID(context, "prof")),
      branchWeights(mdString(context, "branch_weights")),
      context_(context),
      module_(0) {}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
