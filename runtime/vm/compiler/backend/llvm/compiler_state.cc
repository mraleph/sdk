// Copyright 2019 UCWeb Co., Ltd.

#include "vm/compiler/backend/llvm/compiler_state.h"

#include <iostream>

#if defined(DART_ENABLE_LLVM_COMPILER)
namespace dart {
namespace dart_llvm {

CompilerState::CompilerState(const char* function_name)
    : stackMapsSection_(nullptr),
      exception_table_(nullptr),
      dwarf_line_(nullptr),
      module_(nullptr),
      function_(nullptr),
      context_(nullptr),
      entryPoint_(nullptr),
      function_name_(function_name),
      code_kind_(0),
      needs_frame_(false) {
  context_ = LLVMContextCreate();
  module_ = LLVMModuleCreateWithNameInContext("main", context_);
#if defined(TARGET_ARCH_ARM)
  LLVMSetTarget(module_, "armv8-unknown-linux-dart");
#elif defined(TARGET_ARCH_ARM64)
  LLVMSetTarget(module_, "aarch64-unknown-linux-dart");
#else
#error unsupported arch
#endif
}

CompilerState::~CompilerState() {
  LLVMContextDispose(context_);
}

const ByteBuffer* CompilerState::FindByteBuffer(const char* name) const {
  auto it = data_section_list_.begin();
  for (auto& s : data_section_names_) {
    if (s == name) break;
    ++it;
  }
  // Check no relocate.
  if (it == data_section_list_.end()) return nullptr;
  return &*it;
}

void CompilerState::DumpData() const {
  auto it = data_section_list_.begin();
  for (auto& s : data_section_names_) {
    using namespace std;
    cerr << "Data Section " << s << " starts at "
         << static_cast<const void*>(it->data()) << "; size: " << it->size()
         << endl;
    ++it;
  }
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
