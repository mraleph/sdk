// Copyright 2019 UCWeb Co., Ltd.

#include "vm/compiler/backend/llvm/stack_map_info.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/code_descriptors.h"

namespace dart {
namespace dart_llvm {
StackMapInfo::StackMapInfo(StackMapInfoType type) : type_(type), patchid_(0) {}
StackMapInfo::~StackMapInfo() {}

CallSiteInfo::CallSiteInfo()
    : StackMapInfo(StackMapInfoType::kCallInfo),
      type_(CallTargetType::kUnspecify),
      token_pos_(),
      deopt_id_(0),
      stack_parameter_count_(0),
      try_index_(kInvalidTryIndex),
      kind_(RawPcDescriptors::kOther),
      is_tailcall_(false),
      return_on_stack_(false) {}

}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
