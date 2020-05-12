// Copyright 2019 UCWeb Co., Ltd.
#include "vm/compiler/backend/llvm/liveness_analysis.h"
#if defined(DART_ENABLE_LLVM_COMPILER)

#include <algorithm>
#include <deque>
#include <iterator>
#include <unordered_set>

#include "vm/compiler/backend/flow_graph.h"
#include "vm/compiler/backend/il.h"
#include "vm/compiler/backend/llvm/llvm_log.h"

namespace dart {
namespace dart_llvm {
LivenessAnalysis::LivenessAnalysis(FlowGraph* flow_graph)
    : flow_graph_(flow_graph), liveness_(*flow_graph) {}

void LivenessAnalysis::Analyze() {
  liveness_.Analyze();
  AnalyzeCallOut();
#if LLVMLOG_LEVEL >= 20
  Dump();
#endif
}

template <typename Functor>
BitVector* LivenessAnalysis::CalculateBlock(BlockEntryInstr* block,
                                            Functor& f) const {
  BitVector* live = new (zone())
      BitVector(zone(), flow_graph_->max_virtual_register_number());
  live->AddAll(liveness_.GetLiveOutSet(block));
  for (BackwardInstructionIterator it(block); !it.Done(); it.Advance()) {
    Instruction* current = it.Current();
    // Handle define.
    Definition* current_def = current->AsDefinition();
    if ((current_def != nullptr) && current_def->HasSSATemp()) {
      live->Remove(current_def->ssa_temp_index());
      if (current_def->HasPairRepresentation())
        live->Remove(current_def->ssa_temp_index() + 1);
    }
    if (f(current, live)) return live;
    // Initialize location summary for instruction.
    current->InitializeLocationSummary(zone(), true);  // opt

    LocationSummary* locs = current->locs();
    // Handle uses.
    for (intptr_t j = 0; j < current->InputCount(); j++) {
      Value* input = current->InputAt(j);

      ASSERT(!locs->in(j).IsConstant() || input->BindsToConstant());
      if (locs->in(j).IsConstant()) continue;

      live->Add(input->definition()->ssa_temp_index());
      if (input->definition()->HasPairRepresentation()) {
        live->Add(input->definition()->ssa_temp_index() + 1);
      }
    }
  }
  return live;
}

void LivenessAnalysis::AnalyzeCallOut() {
  auto& postorder = flow_graph_->postorder();
  for (int i = 0; i < postorder.length(); ++i) {
    BlockEntryInstr* block = postorder[i];
    // Recognize call site.
    auto f = [&](Instruction* current, BitVector* live) {
      if (current->IsInstanceCall() || current->IsStaticCall() ||
          current->IsClosureCall()) {
        SubmitCallsite(current, live);
      }
      return false;
    };
    CalculateBlock(block, f);
  }
}

void LivenessAnalysis::SubmitCallsite(Instruction* instr, BitVector* live) {
  BitVector* call_out_live = new (zone())
      BitVector(zone(), flow_graph_->max_virtual_register_number());
  call_out_live->AddAll(live);
  call_out_map_.emplace(instr, call_out_live);
}

void LivenessAnalysis::Dump() {
  THR_Print("Print liveness info for function %s\n",
            String::Handle(flow_graph_->function().name()).ToCString());
  liveness_.Dump();
  for (auto& pair : call_out_map_) {
    Instruction* instr = pair.first;
    auto& values = pair.second;

#if !defined(PRODUCT) || defined(FORCE_INCLUDE_DISASSEMBLER)
    char buffer[4000];
    BufferFormatter bf(buffer, 4000);
    instr->PrintTo(&bf);
    THR_Print("%s\n", buffer);
#else
    THR_Print("instr %p:\n", instr);
#endif
    values->Print();
  }
}

Zone* LivenessAnalysis::zone() const {
  return flow_graph_->zone();
}

BitVector* LivenessAnalysis::GetCallOutAt(Instruction* at) const {
  auto found = call_out_map_.find(at);
  // other il instruction may also need the liveness info.
  if (found == call_out_map_.end()) {
    return CalculateLiveness(at);
  }
  return found->second;
}

BitVector* LivenessAnalysis::GetLiveInSet(BlockEntryInstr* block) const {
  return liveness_.GetLiveInSet(block);
}

BitVector* LivenessAnalysis::CalculateLiveness(Instruction* at) const {
  BlockEntryInstr* block = at->GetBlock();
  auto f = [&](Instruction* current, BitVector* live) {
    if (current == at) return true;
    return false;
  };
  return CalculateBlock(block, f);
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
