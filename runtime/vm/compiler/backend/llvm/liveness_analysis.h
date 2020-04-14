#ifndef LIVENESS_ANALYSIS_H
#define LIVENESS_ANALYSIS_H
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "vm/compiler/backend/flow_graph.h"
#include "vm/compiler/backend/llvm/llvm_config.h"
#ifdef DART_ENABLE_LLVM_COMPILER

namespace dart {
class FlowGraph;
class Instruction;
class BlockEntryInstr;
class GraphEntryInstr;
class Zone;
namespace dart_llvm {
class LivenessAnalysis;

class SSALivenessAnalysis : public dart::LivenessAnalysis {
 public:
  explicit SSALivenessAnalysis(const dart_llvm::LivenessAnalysis&);

  inline bool broken() const { return broken_; }

 private:
  // Compute initial values for live-out, kill and live-in sets.
  void ComputeInitialSets() override;
  const dart_llvm::LivenessAnalysis& liveness_analysis_;
  GraphEntryInstr* graph_entry_;
  bool broken_;
};
using LiveValuesMap = std::unordered_map<Instruction*, BitVector*>;

// This analysis is similar to the SSALivenessAnalysis.
// But it will extra call out info.
class LivenessAnalysis final {
 public:
  explicit LivenessAnalysis(FlowGraph* flow_graph);
  ~LivenessAnalysis() = default;
  bool Analyze();

  BitVector* GetLiveInSet(BlockEntryInstr*) const;
  BitVector* GetLiveOutSet(BlockEntryInstr*) const;
  BitVector* GetCallOutAt(Instruction* at) const;
  int GetPPValueSSAIdx() const;
  int MaxSSANumber() const;

  const FlowGraph& flow_graph() const { return *flow_graph_; }
  BitVector* NewLiveBitVector() const;

 private:
  void AnalyzeCallOut();
  BitVector* CalculateLiveness(Instruction* at) const;
  void SubmitCallsite(Instruction*, BitVector*);
  template <typename Functor>
  BitVector* CalculateBlock(BlockEntryInstr* block, Functor& f) const;
  void Dump();
  Zone* zone() const;

  FlowGraph* flow_graph_;
  SSALivenessAnalysis liveness_;
  LiveValuesMap call_out_map_;
};
}  // namespace dart_llvm
}  // namespace dart

#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // LIVENESS_ANALYSIS_H
