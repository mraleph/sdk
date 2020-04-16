// Copyright 2019 UCWeb Co., Ltd.

#include "vm/compiler/backend/llvm/stack_map_info.h"
#if defined(DART_ENABLE_LLVM_COMPILER)

namespace dart {
namespace dart_llvm {
StackMapInfo::StackMapInfo(StackMapInfoType type) : type_(type) {}
StackMapInfo::~StackMapInfo() {}

CallInfo::CallInfo(LocationVector&& locations)
    : StackMapInfo(StackMapInfoType::kCallInfo), is_tailcall_(false) {}

}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
