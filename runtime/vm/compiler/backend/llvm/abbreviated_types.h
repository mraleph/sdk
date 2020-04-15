// Copyright 2019 UCWeb Co., Ltd.
#ifndef ABBREVIATED_TYPES_H
#define ABBREVIATED_TYPES_H
#include "vm/compiler/backend/llvm/llvm_headers.h"
namespace dart {
namespace dart_llvm {
typedef LLVMAtomicOrdering LAtomicOrdering;
typedef LLVMBasicBlockRef LBasicBlock;
typedef LLVMBuilderRef LBuilder;
typedef LLVMCallConv LCallConv;
typedef LLVMContextRef LContext;
typedef LLVMIntPredicate LIntPredicate;
typedef LLVMLinkage LLinkage;
typedef LLVMModuleRef LModule;
typedef LLVMRealPredicate LRealPredicate;
typedef LLVMTypeRef LType;
typedef LLVMValueRef LValue;
typedef LLVMMemoryBufferRef LMemoryBuffer;
}  // namespace dart_llvm
}  // namespace dart
#endif  // ABBREVIATED_TYPES_H
