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

#if defined(DART_ENABLE_LLVM_COMPILER)
namespace dart {
class Instruction;
namespace dart_llvm {
class StackMapInfo;
class ByteBuffer {
 public:
  const uint8_t* data() const {
    return reinterpret_cast<const uint8_t*>(this + 1);
  }
  uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }

  size_t size() const { return size_; }

 private:
  ByteBuffer() = delete;
  ByteBuffer(const ByteBuffer&) = delete;
  friend struct CompilerState;
  size_t size_;
};

typedef std::list<ByteBuffer*> BufferList;

struct Section {
  unsigned id;
  unsigned alignment;
  std::string name;
  ByteBuffer* bb;
  bool is_code;
};

struct CompilerState {
  BufferList code_section_list_;
  BufferList data_section_list_;
  std::vector<Section> sections_;
  StackMapInfoMap stack_map_info_map_;
  std::vector<Instruction*> debug_instrs_;
  ByteBuffer* stackMapsSection_;
  ByteBuffer* exception_table_;
  ByteBuffer* dwarf_line_;
  LLVMModuleRef module_;
  LLVMValueRef function_;
  LLVMContextRef context_;
  void* entryPoint_;
  std::unique_ptr<char[]> buffer_as_whole_;
  size_t buffer_top_;
  std::string function_name_;
  int code_kind_;
  bool needs_frame_;
  CompilerState(const char* function_name);
  ~CompilerState();
  CompilerState(const CompilerState&) = delete;
  const CompilerState& operator=(const CompilerState&) = delete;
  const ByteBuffer* FindByteBuffer(const char* name) const;
  void DumpData() const;
  void AddSection(unsigned id,
                  std::string name,
                  ByteBuffer* bb,
                  unsigned alignment,
                  bool is_code);
  ByteBuffer* AllocateByteBuffer(size_t size);
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // COMPILER_STATE_H
