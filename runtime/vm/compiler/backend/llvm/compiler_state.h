// Copyright 2019 UCWeb Co., Ltd.

#ifndef COMPILER_STATE_H
#define COMPILER_STATE_H
#include <stdint.h>

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "vm/compiler/backend/llvm/llvm_headers.h"
#include "vm/compiler/backend/llvm/stack_map_info.h"
#include "vm/compiler/backend/llvm/stack_maps.h"

#if defined(DART_ENABLE_LLVM_COMPILER)
namespace dart {
namespace dart_llvm {
class StackMapInfo;
typedef std::vector<uint8_t> ByteBuffer;
typedef std::list<ByteBuffer> BufferList;
typedef std::list<std::string> StringList;

struct CompilerState {
  BufferList code_section_list_;
  BufferList data_section_list_;
  StringList code_section_names_;
  StringList data_section_names_;
  StackMapInfoMap stack_map_info_map_;
  StackMaps sm_;
  ByteBuffer* stackMapsSection_;
  ByteBuffer* exception_table_;
  LLVMModuleRef module_;
  LLVMValueRef function_;
  LLVMContextRef context_;
  void* entryPoint_;
  const char* function_name_;
  int code_kind_;
  bool needs_frame_;
  CompilerState(const char* FunctionName);
  ~CompilerState();
  CompilerState(const CompilerState&) = delete;
  const CompilerState& operator=(const CompilerState&) = delete;
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // COMPILER_STATE_H
