#ifndef IR_TRANSLATOR_H
#define IR_TRANSLATOR_H
#include "vm/compiler/backend/llvm/llvm_headers.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/compiler/backend/il.h"

namespace dart {
class FlowGraph;
namespace dart_llvm {
struct CompilerState;

class IRTranslator : public FlowGraphVisitor {
 public:
  explicit IRTranslator(FlowGraph*);
  ~IRTranslator();

 private:
#define DECLARE_VISIT_INSTRUCTION(ShortName, Attrs)                            \
  void Visit##ShortName(ShortName##Instr* instr) override;

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION
  DISALLOW_COPY_AND_ASSIGN(IRTranslator);
  FlowGraph* flow_graph_;
  std::unique_ptr<CompilerState> compiler_state_;
  std::unique_ptr<LivenessAnalysis> liveness_analysis_;
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // IR_TRANSLATOR_H
