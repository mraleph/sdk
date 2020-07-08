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
  for (auto& s : sections_) {
    if (s.is_code) continue;
    if (s.name == name) return s.bb;
  }
  return nullptr;
}

void CompilerState::DumpData() const {
  for (auto& s : sections_) {
    if (s.is_code) continue;
    using namespace std;
    cerr << "Data Section " << s.name << " starts at "
         << static_cast<const void*>(s.bb->data()) << "; size: " << s.bb->size()
         << endl;
  }
}

void CompilerState::AddSection(unsigned id,
                               std::string name,
                               ByteBuffer* bb,
                               unsigned alignment,
                               bool is_code) {
  sections_.emplace_back();
  auto& new_section = sections_.back();
  new_section.id = id;
  new_section.alignment = alignment;
  new_section.name = std::move(name);
  new_section.bb = bb;
  new_section.is_code = is_code;
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
