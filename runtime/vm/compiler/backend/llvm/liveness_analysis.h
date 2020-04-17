#ifndef LIVENESS_ANALYSIS_H
#define LIVENESS_ANALYSIS_H
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "vm/compiler/backend/linearscan.h"
#include "vm/compiler/backend/llvm/llvm_config.h"
#ifdef DART_ENABLE_LLVM_COMPILER

namespace dart {
class FlowGraph;
class Instruction;
class BlockEntryInstr;
class Zone;
namespace dart_llvm {
using LiveValuesMap = std::unordered_map<Instruction*, BitVector*>;

// This analysis is similar to the SSALivenessAnalysis.
// But it will extra call out info.
class LivenessAnalysis final {
 public:
  explicit LivenessAnalysis(FlowGraph* flow_graph);
  ~LivenessAnalysis() = default;
  void Analyze();

  BitVector* GetLiveInSetAt(intptr_t postorder_number) const {
    return liveness_.GetLiveInSetAt(postorder_number);
  }

  BitVector* GetCallOutAt(Instruction* at);

 private:
  void AnalyzeCallOut();
  void SubmitCallsite(Instruction*, BitVector*);
  void Dump();
  Zone* zone();

 private:
  FlowGraph* flow_graph_;
  SSALivenessAnalysis liveness_;
  LiveValuesMap call_out_map_;
};
}  // namespace dart_llvm
}  // namespace dart

#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // LIVENESS_ANALYSIS_H
