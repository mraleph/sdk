#include "vm/compiler/backend/llvm/ir_translator.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/compiler/backend/llvm/compiler_state.h"
#include "vm/compiler/backend/llvm/liveness_analysis.h"
namespace dart {
namespace dart_llvm {
IRTranslator::IRTranslator(FlowGraph* flow_graph) : flow_graph_(flow_graph) {
  compiler_state_.reset(new CompilerState(
      String::Handle(flow_graph->zone(),
                     flow_graph->parsed_function.UserVisibleName())
          ->ToCString()));
  liveness_analysis.reset(new LivenessAnalysis(flow_graph));
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
