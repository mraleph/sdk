#ifndef LLVM_CODE_ASSEMBLER_H
#define LLVM_CODE_ASSEMBLER_H
#include "vm/compiler/backend/llvm/llvm_config.h"
#include "vm/compiler/backend/llvm/llvm_log.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include <functional>
#include <map>
#include <tuple>

#include "vm/compiler/backend/llvm/dwarf_info.h"

namespace dart {
namespace compiler {
class Assembler;
}
class FlowGraphCompiler;
namespace dart_llvm {
class CallSiteInfo;
class CodeAssembler {
 public:
  explicit CodeAssembler(FlowGraphCompiler* compiler);
  void AssembleCode();

 private:
  FlowGraphCompiler& compiler();
  const CompilerState& compiler_state();
  compiler::Assembler& assembler();

  void PrepareExceptionTable();
  void PrepareInstrActions();
  void PrepareDwarfAction();
  void PrepareStackMapAction();
  void AddMetaData(const CallSiteInfo*, const StackMaps::Record&);
  void CollectExceptionInfo(const CallSiteInfo*);
  void RecordSafePoint(const CallSiteInfo*, const StackMaps::Record&);
  void EmitExceptionHandler();
  template <typename T>
  std::function<void()> WrapAction(T f);

  std::map<size_t /* offset */, std::function<void()> /* action */> action_map_;
  std::map<int /* try_idx */, size_t /* offset */> exception_map_;
  std::vector<std::tuple<int, int, int>> exception_tuples_;
  FlowGraphCompiler* compiler_;
  const uint8_t* code_start_ = nullptr;
  size_t offset_ = 0;
  size_t bytes_left_ = 0;
  size_t slot_count_ = 0;
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // LLVM_CODE_ASSEMBLER_H
