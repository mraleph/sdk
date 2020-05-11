// Copyright 2019 UCWeb Co., Ltd.

#ifndef STACK_MAP_INFO_H
#define STACK_MAP_INFO_H
#include <stdint.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "vm/compiler/backend/llvm/llvm_config.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/compiler/backend/locations.h"
#include "vm/object.h"
#include "vm/token_position.h"

namespace dart {
namespace dart_llvm {

enum class StackMapInfoType {
  kCallInfo,
};

class StackMapInfo {
 public:
  explicit StackMapInfo(StackMapInfoType);
  virtual ~StackMapInfo();

  StackMapInfoType GetType() const { return type_; }
  int patchid() const { return patchid_; }
  void set_patchid(int _patchid) { patchid_ = _patchid; }

 private:
  const StackMapInfoType type_;
  int patchid_;
};

#define DEFINE_ACCESSOR(type, name, class_name)                                \
  type name() const { return name##_; }                                        \
  class_name* set_##name(type _##name) {                                       \
    name##_ = _##name;                                                         \
    return this;                                                               \
  }

class CallSiteInfo final : public StackMapInfo {
 public:
  enum class CallTargetType {
    kUnspecify,
    // target passed in the statepoint
    kTarget,
    // target passed in the GP reg.
    kReg,
    // relative call target in the function
    kCallRelative,
    // relative call target in the code
    kStubRelative,
    // Code object call is the call that code object alreadly loaded to CODE_REG
    kCodeObject,
    kCFunction,
    kNative,
  };
  explicit CallSiteInfo();
  ~CallSiteInfo() override = default;
#define CALLSITE_ACCESSOR(V)                                                   \
  V(CallTargetType, type)                                                      \
  V(TokenPosition, token_pos)                                                  \
  V(intptr_t, deopt_id)                                                        \
  V(size_t, stack_parameter_count)                                             \
  V(intptr_t, try_index)                                                       \
  V(RawPcDescriptors::Kind, kind)                                              \
  V(int, reg)                                                                  \
  V(const Function*, target)                                                   \
  V(const Code*, code)                                                         \
  V(CodeEntryKind, entry_kind)                                                 \
  V(bool, is_tailcall)                                                         \
  V(bool, return_on_stack)

#define CALLSITE_WAPPER(type, name) DEFINE_ACCESSOR(type, name, CallSiteInfo)

  CALLSITE_ACCESSOR(CALLSITE_WAPPER)

#undef CALLSITE_WAPPER
#undef CALLSITE_ACCESSOR

 private:
  CallTargetType type_;
  TokenPosition token_pos_;
  intptr_t deopt_id_;
  // for stack maps generate, we will mark the lowest stack_parameter_count's stack slot 1.
  size_t stack_parameter_count_;
  intptr_t try_index_;
  RawPcDescriptors::Kind kind_;
  union {
    int reg_;
    struct {
      const Function* target_;
      Code::EntryKind entry_kind_;
    };
    const Code* code_;
  };
  bool is_tailcall_;
  bool return_on_stack_;
};

#undef DEFINE_ACCESSOR
// By zuojian.lzj, should be int64_t. But I believe there will not be any number
// greater.
typedef std::unordered_map<int, std::unique_ptr<StackMapInfo>> StackMapInfoMap;
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // STACK_MAP_INFO_H
