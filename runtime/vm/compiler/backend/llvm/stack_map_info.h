// Copyright 2019 UCWeb Co., Ltd.

#ifndef STACK_MAP_INFO_H
#define STACK_MAP_INFO_H
#include <stdint.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "vm/compiler/backend/llvm/llvm_config.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/object.h"

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

class CallInfo final : public StackMapInfo {
 public:
  enum class CallTargetType {
    kReg,
    kCodeRelative,
    kCodeObject,
  };
  struct CallTarget {
    CallTargetType type;
    union {
      RawCode* code;
      int reg;
    };
  };
  explicit CallInfo(const CallTarget&);
  ~CallInfo() override = default;
  bool is_tailcall() const { return is_tailcall_; }
  void set_is_tailcall(bool _tailcall) { is_tailcall_ = _tailcall; }
  const CallTarget& target() const { return target_; }

 private:
  CallTarget target_;
  bool is_tailcall_;
};

// By zuojian.lzj, should be int64_t. But I believe there will not be any number
// greater.
typedef std::unordered_map<int, std::unique_ptr<StackMapInfo>> StackMapInfoMap;
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // STACK_MAP_INFO_H
