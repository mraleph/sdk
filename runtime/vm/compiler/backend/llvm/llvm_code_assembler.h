#ifndef LLVM_CODE_ASSEMBLER_H
#define LLVM_CODE_ASSEMBLER_H
#include "vm/compiler/backend/llvm/llvm_config.h"
#include "vm/compiler/backend/llvm/llvm_log.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include <functional>
#include <map>
#include <tuple>

#include "vm/compiler/assembler/assembler.h"
#include "vm/compiler/backend/llvm/dwarf_info.h"

namespace dart {
class Instruction;
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
  // returns the try index;
  intptr_t CollectExceptionInfo(const CallSiteInfo*);
  void RecordSafePoint(const CallSiteInfo*, const StackMaps::Record&);
  void EmitExceptionHandler();
  void EndLastInstr();
  void AddAction(size_t pc_offset, std::function<void()> action);
  void CallWithCallReg(const CallSiteInfo*, dart::Register);
  void GenerateNativeCall(const CallSiteInfo* call_site_info);
  void GeneratePatchableCall(const CallSiteInfo* call_site_info);
  template <typename T>
  std::function<void()> WrapAction(T f);

  std::map<size_t /* offset */, std::vector<std::function<void()>> /* action */>
      action_map_;
  std::map<intptr_t /* try_idx */,
           std::tuple<size_t /* ehb offset */, intptr_t /* origin try idx */>>
      exception_map_;
  std::map<size_t /* ehb offset */, intptr_t /* extended try index */>
      emited_idx_crf_;
  std::vector<std::tuple<int, int, int>> exception_tuples_;
  FlowGraphCompiler* compiler_;
  const uint8_t* code_start_ = nullptr;
  Instruction* last_instr_ = nullptr;
  size_t offset_ = 0;
  size_t bytes_left_ = 0;
  size_t slot_count_ = 0;
  intptr_t exception_extend_id_ = 0;
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // LLVM_CODE_ASSEMBLER_H
