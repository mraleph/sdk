// Copyright 2019 UCWeb Co., Ltd.

#include "vm/compiler/backend/llvm/stack_map_info.h"
#if defined(DART_ENABLE_LLVM_COMPILER)

namespace dart {
namespace dart_llvm {
StackMapInfo::StackMapInfo(StackMapInfoType type) : type_(type), patchid_(0) {}
StackMapInfo::~StackMapInfo() {}

CallInfo::CallInfo(const CallInfo::CallTarget& target)
    : StackMapInfo(StackMapInfoType::kCallInfo),
      target_(target),
      is_tailcall_(false) {}

}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
