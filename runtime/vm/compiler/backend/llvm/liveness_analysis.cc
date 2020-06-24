// Copyright 2019 UCWeb Co., Ltd.
#include "vm/compiler/backend/llvm/liveness_analysis.h"
#if defined(DART_ENABLE_LLVM_COMPILER)

#include <algorithm>
#include <deque>
#include <iterator>
#include <unordered_set>

#include "vm/compiler/backend/il.h"
#include "vm/compiler/backend/llvm/llvm_log.h"

namespace dart {
namespace dart_llvm {
static intptr_t ToSecondPairVreg(intptr_t vreg) {
  // Map vreg to its pair vreg.
  static const intptr_t kNoVirtualRegister = -1;
  static const intptr_t kPairVirtualRegisterOffset = 1;
  EMASSERT((vreg == kNoVirtualRegister) || vreg >= 0);
  return (vreg == kNoVirtualRegister) ? kNoVirtualRegister
                                      : (vreg + kPairVirtualRegisterOffset);
}

LivenessAnalysis::LivenessAnalysis(FlowGraph* flow_graph)
    : flow_graph_(flow_graph), liveness_(*this) {}

bool LivenessAnalysis::Analyze() {
  liveness_.Analyze();
  if (liveness_.broken()) return false;
  AnalyzeCallOut();
#if LLVMLOG_LEVEL >= 20
  Dump();
#endif
  return true;
}

template <typename Functor>
BitVector* LivenessAnalysis::CalculateBlock(BlockEntryInstr* block,
                                            Functor& f) const {
  BitVector* live = new (zone()) BitVector(zone(), MaxSSANumber());
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
    // All the instructions use PP Value implicitly.
    live->Add(GetPPValueSSAIdx());
    if (f(current, live)) return live;
    // Initialize location summary for instruction.

    // Handle uses.
    for (intptr_t j = 0; j < current->InputCount(); j++) {
      Value* input = current->InputAt(j);

      if (input->definition()->IsConstant()) continue;

      live->Add(input->definition()->ssa_temp_index());
      if (input->definition()->HasPairRepresentation()) {
        live->Add(input->definition()->ssa_temp_index() + 1);
      }
    }
    // Handle uses part2: arguments;
    intptr_t argument_count = current->ArgumentCount();
    for (intptr_t j = 0; j < argument_count; ++j) {
      Definition* argument = current->ArgumentAt(j);
      live->Add(argument->ssa_temp_index());
      if (argument->HasPairRepresentation()) {
        live->Add(ToSecondPairVreg(argument->ssa_temp_index()));
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
  BitVector* call_out_live = new (zone()) BitVector(zone(), MaxSSANumber());
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

BitVector* LivenessAnalysis::GetLiveOutSet(BlockEntryInstr* block) const {
  return liveness_.GetLiveOutSet(block);
}

BitVector* LivenessAnalysis::CalculateLiveness(Instruction* at) const {
  BlockEntryInstr* block = at->GetBlock();
  auto f = [&](Instruction* current, BitVector* live) {
    if (current == at) return true;
    return false;
  };
  return CalculateBlock(block, f);
}

SSALivenessAnalysis::SSALivenessAnalysis(
    const dart_llvm::LivenessAnalysis& liveness_analysis)
    : LivenessAnalysis(liveness_analysis.MaxSSANumber(),
                       liveness_analysis.flow_graph().postorder()),
      liveness_analysis_(liveness_analysis),
      graph_entry_(liveness_analysis.flow_graph().graph_entry()),
      broken_(false) {}

void SSALivenessAnalysis::ComputeInitialSets() {
  const intptr_t block_count = postorder_.length();
  for (intptr_t i = 0; i < block_count; i++) {
    BlockEntryInstr* block = postorder_[i];

    BitVector* kill = kill_[i];
    BitVector* live_in = live_in_[i];

    // Iterate backwards starting at the last instruction.
    for (BackwardInstructionIterator it(block); !it.Done(); it.Advance()) {
      Instruction* current = it.Current();

      LocationSummary* locs = current->locs();
#if defined(DEBUG)
      locs->DiscoverWritableInputs();
#endif

      // Handle definitions.
      Definition* current_def = current->AsDefinition();
      if ((current_def != NULL) && current_def->HasSSATemp()) {
        kill->Add(current_def->ssa_temp_index());
        live_in->Remove(current_def->ssa_temp_index());
        if (current_def->HasPairRepresentation()) {
          kill->Add(ToSecondPairVreg(current_def->ssa_temp_index()));
          live_in->Remove(ToSecondPairVreg(current_def->ssa_temp_index()));
        }
      }

      // Handle uses.
      ASSERT(locs->input_count() == current->InputCount());
      int input_count = current->InputCount();
      for (intptr_t j = 0; j < input_count; j++) {
        Value* input = current->InputAt(j);

        if (input->definition()->IsConstant()) continue;

        live_in->Add(input->definition()->ssa_temp_index());
        if (input->definition()->HasPairRepresentation()) {
          live_in->Add(ToSecondPairVreg(input->definition()->ssa_temp_index()));
        }
      }
      // Handle uses part2: arguments;
      intptr_t argument_count = current->ArgumentCount();
      for (intptr_t j = 0; j < argument_count; ++j) {
        Definition* argument = current->ArgumentAt(j);
        live_in->Add(argument->ssa_temp_index());
        if (argument->HasPairRepresentation()) {
          live_in->Add(ToSecondPairVreg(argument->ssa_temp_index()));
        }
      }
      // All the instructions use PP Value implicitly.
      live_in->Add(liveness_analysis_.GetPPValueSSAIdx());

      // Add non-argument uses from the deoptimization environment (pushed
      // arguments are not allocated by the register allocator).
      if (current->env() != NULL) {
        for (Environment::DeepIterator env_it(current->env()); !env_it.Done();
             env_it.Advance()) {
          Definition* defn = env_it.CurrentValue()->definition();
          if (defn->IsMaterializeObject()) {
            // MaterializeObject instruction is not in the graph.
            // Treat its inputs as part of the environment.
            // DeepLiveness(defn->AsMaterializeObject(), live_in);
            broken_ = true;
          } else if (!defn->IsPushArgument() && !defn->IsConstant()) {
            live_in->Add(defn->ssa_temp_index());
            if (defn->HasPairRepresentation()) {
              live_in->Add(ToSecondPairVreg(defn->ssa_temp_index()));
            }
          }
        }
      }
    }

    // Handle phis.
    if (block->IsJoinEntry()) {
      JoinEntryInstr* join = block->AsJoinEntry();
      for (PhiIterator it(join); !it.Done(); it.Advance()) {
        PhiInstr* phi = it.Current();
        EMASSERT(phi != NULL);
        kill->Add(phi->ssa_temp_index());
        live_in->Remove(phi->ssa_temp_index());
        if (phi->HasPairRepresentation()) {
          kill->Add(ToSecondPairVreg(phi->ssa_temp_index()));
          live_in->Remove(ToSecondPairVreg(phi->ssa_temp_index()));
        }

        // If a phi input is not defined by the corresponding predecessor it
        // must be marked live-in for that predecessor.
        for (intptr_t k = 0; k < phi->InputCount(); k++) {
          Value* val = phi->InputAt(k);
          if (val->BindsToConstant()) continue;

          BlockEntryInstr* pred = block->PredecessorAt(k);
          const intptr_t use = val->definition()->ssa_temp_index();
          if (!kill_[pred->postorder_number()]->Contains(use)) {
            live_in_[pred->postorder_number()]->Add(use);
          }
          live_out_[pred->postorder_number()]->Add(use);
          if (phi->HasPairRepresentation()) {
            const intptr_t second_use = ToSecondPairVreg(use);
            if (!kill_[pred->postorder_number()]->Contains(second_use)) {
              live_in_[pred->postorder_number()]->Add(second_use);
            }
            live_out_[pred->postorder_number()]->Add(second_use);
          }
        }
      }
    } else if (auto entry = block->AsBlockEntryWithInitialDefs()) {
      // Process initial definitions, i.e. parameters and special parameters.
      for (intptr_t i = 0; i < entry->initial_definitions()->length(); i++) {
        Definition* def = (*entry->initial_definitions())[i];
        const intptr_t vreg = def->ssa_temp_index();
        kill_[entry->postorder_number()]->Add(vreg);
        live_in_[entry->postorder_number()]->Remove(vreg);
        if (def->HasPairRepresentation()) {
          kill_[entry->postorder_number()]->Add(ToSecondPairVreg((vreg)));
          live_in_[entry->postorder_number()]->Remove(ToSecondPairVreg(vreg));
        }
      }
    }
  }
}

int LivenessAnalysis::GetPPValueSSAIdx() const {
  return flow_graph().max_virtual_register_number();
}

int LivenessAnalysis::MaxSSANumber() const {
  return flow_graph().max_virtual_register_number() + 1;
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
