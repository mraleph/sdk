#ifndef IR_TRANSLATOR_H
#define IR_TRANSLATOR_H
#include "vm/compiler/backend/llvm/llvm_headers.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/compiler/backend/il.h"

namespace dart {
class FlowGraph;
class BlockEntryInstr;
namespace dart_llvm {
struct CompilerState;
class Output;
class LivenessAnalysis;

class IRTranslator : public FlowGraphVisitor {
 public:
  explicit IRTranslator(FlowGraph*);
  ~IRTranslator();
  void Translate();

 private:
  struct Impl;
  inline CompilerState& compiler_state() { return *compiler_state_; }
  inline LivenessAnalysis& liveness_analysis() { return *liveness_analysis_; }
  inline Output& output() { return *output_; }
#define DECLARE_VISIT_INSTRUCTION(ShortName, Attrs)                            \
  void Visit##ShortName(ShortName##Instr* instr) override;

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION
  DISALLOW_COPY_AND_ASSIGN(IRTranslator);
  FlowGraph* flow_graph_;
  std::unique_ptr<CompilerState> compiler_state_;
  std::unique_ptr<LivenessAnalysis> liveness_analysis_;
  std::unique_ptr<Output> output_;
  std::unique_ptr<Impl> impl_;
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // IR_TRANSLATOR_H
