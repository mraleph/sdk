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
struct LivenessInfo {
  // live_in_map maps basic blocks to their live-in values;
  GrowableArray<BitVector*> live_in_;
  // call_out_map maps calls to their out values.
  LiveValuesMap call_out_map;
};

// This analysis is similar to the SSALivenessAnalysis.
// But it will extra call out info.
class LivenessAnalysis final {
 public:
  explicit LivenessAnalysis(FlowGraph* flow_graph);
  ~LivenessAnalysis() = default;
  void Analyze();

 private:
  void AnalyzeCallOut();
  void SubmitCallsite(Instruction*, BitVector*);
  void Dump();
  Zone* zone();

 private:
  FlowGraph* flow_graph_;
  SSALivenessAnalysis liveness_;
  LiveValuesMap call_out_map;
};
}  // namespace dart_llvm
}  // namespace dart

#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // LIVENESS_ANALYSIS_H
