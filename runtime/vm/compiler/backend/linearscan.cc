// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/backend/linearscan.h"

#include "vm/bit_vector.h"
#include "vm/compiler/backend/flow_graph.h"
#include "vm/compiler/backend/flow_graph_compiler.h"
#include "vm/compiler/backend/il.h"
#include "vm/compiler/backend/il_printer.h"
#include "vm/compiler/backend/loops.h"
#include "vm/log.h"
#include "vm/parser.h"
#include "vm/stack_frame.h"

namespace dart {

#undef ASSERT
#define ASSERT RELEASE_ASSERT

#if !defined(PRODUCT)
#define INCLUDE_LINEAR_SCAN_TRACING_CODE
#endif

#if defined(INCLUDE_LINEAR_SCAN_TRACING_CODE)
#define TRACE_ALLOC(statement)                                                 \
  do {                                                                         \
    if (FLAG_trace_ssa_allocator && CompilerState::ShouldTrace()) statement;   \
  } while (0)
#else
#define TRACE_ALLOC(statement)
#endif

#define TRACE_ALLOCF(...) TRACE_ALLOC(THR_Print(__VA_ARGS__))

static const intptr_t kNoVirtualRegister = -1;
static const intptr_t kTempVirtualRegister = -2;
static const intptr_t kIllegalPosition = -1;
static const intptr_t kMaxPosition = 0x7FFFFFFF;
static const intptr_t kPairVirtualRegisterOffset = 1;

// Definitions which have pair representations
// (kPairOfTagged) use two virtual register names.
// At SSA index allocation time each definition reserves two SSA indexes,
// the second index is only used for pairs. This function maps from the first
// SSA index to the second.
static intptr_t ToSecondPairVreg(intptr_t vreg) {
  // Map vreg to its pair vreg.
  ASSERT((vreg == kNoVirtualRegister) || vreg >= 0);
  return (vreg == kNoVirtualRegister) ? kNoVirtualRegister
                                      : (vreg + kPairVirtualRegisterOffset);
}

static constexpr intptr_t kPositionsPerInstruction = 2;

// static intptr_t MinPosition(intptr_t a, intptr_t b) {
//  return (a < b) ? a : b;
//}

static bool IsRealInstructionPosition(intptr_t pos) {
  static_assert(kPositionsPerInstruction == 2);
  return (pos & 1) == 0;
}

static bool IsParallelMovePosition(intptr_t pos) {
  static_assert(kPositionsPerInstruction == 2);
  return (pos & 1) == 1;
}

static intptr_t ToPreviousMovePosition(intptr_t pos) {
  RELEASE_ASSERT(IsRealInstructionPosition(pos));
  return pos - 1;
}

static intptr_t ToNextMovePosition(intptr_t pos) {
  RELEASE_ASSERT(IsRealInstructionPosition(pos));
  return pos + 1;
}

static intptr_t ToClosestRealInstructionPosition(intptr_t pos) {
  RELEASE_ASSERT(pos > 0);
  return (pos + 1) & ~1;
}

static intptr_t ToPreceedingMovePosition(intptr_t pos) {
  return ToPreviousMovePosition(ToClosestRealInstructionPosition(pos));
}

// Additional information on loops during register allocation.
struct ExtraLoopInfo : public ZoneAllocated {
  ExtraLoopInfo(intptr_t s, intptr_t e)
      : start(s), end(e), backedge_interference(nullptr) {}
  intptr_t start;
  intptr_t end;
  BitVector* backedge_interference;
};

// Returns extra loop information.
static ExtraLoopInfo* ComputeExtraLoopInfo(Zone* zone, LoopInfo* loop_info) {
  intptr_t start = loop_info->header()->start_pos();
  intptr_t end = start;
  for (auto back_edge : loop_info->back_edges()) {
    intptr_t end_pos = back_edge->end_pos();
    if (end_pos > end) {
      end = end_pos;
    }
  }
  return new (zone) ExtraLoopInfo(start, end);
}

FlowGraphAllocator::FlowGraphAllocator(const FlowGraph& flow_graph,
                                       bool intrinsic_mode)
    : flow_graph_(flow_graph),
      reaching_defs_(flow_graph),
      value_representations_(flow_graph.max_virtual_register_number()),
      block_order_(flow_graph.reverse_postorder()),
      postorder_(flow_graph.postorder()),
      instructions_(),
      block_entries_(),
      extra_loop_info_(),
      liveness_(flow_graph),
      vreg_count_(flow_graph.max_virtual_register_number()),
      live_ranges_(flow_graph.max_virtual_register_number()),
      unallocated_cpu_(),
      unallocated_xmm_(),
      cpu_regs_(),
      fpu_regs_(),
      blocked_cpu_registers_(),
      blocked_fpu_registers_(),
      spilled_(),
      safepoints_(),
      register_kind_(),
      number_of_registers_(0),
      registers_(),
      blocked_registers_(),
      unallocated_(),
      spill_slots_(),
      quad_spill_slots_(),
      untagged_spill_slots_(),
      cpu_spill_slot_count_(0),
      intrinsic_mode_(intrinsic_mode) {
  for (intptr_t i = 0; i < vreg_count_; i++) {
    live_ranges_.Add(NULL);
  }
  for (intptr_t i = 0; i < vreg_count_; i++) {
    value_representations_.Add(kNoRepresentation);
  }

  // All registers are marked as "not blocked" (array initialized to false).
  // Mark the unavailable ones as "blocked" (true).
  for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
    if ((kDartAvailableCpuRegs & (1 << i)) == 0) {
      blocked_cpu_registers_[i] = true;
    }
  }

  // FpuTMP is used as scratch by optimized code and parallel move resolver.
  blocked_fpu_registers_[FpuTMP] = true;

  // Block additional registers needed preserved when generating intrinsics.
  // TODO(fschneider): Handle saving and restoring these registers when
  // generating intrinsic code.
  if (intrinsic_mode) {
    blocked_cpu_registers_[ARGS_DESC_REG] = true;

#if !defined(TARGET_ARCH_IA32)
    // Need to preserve CODE_REG to be able to store the PC marker
    // and load the pool pointer.
    blocked_cpu_registers_[CODE_REG] = true;
#endif
  }
}

static void DeepLiveness(MaterializeObjectInstr* mat, BitVector* live_in) {
  if (mat->was_visited_for_liveness()) {
    return;
  }
  mat->mark_visited_for_liveness();

  for (intptr_t i = 0; i < mat->InputCount(); i++) {
    if (!mat->InputAt(i)->BindsToConstant()) {
      Definition* defn = mat->InputAt(i)->definition();
      MaterializeObjectInstr* inner_mat = defn->AsMaterializeObject();
      if (inner_mat != NULL) {
        DeepLiveness(inner_mat, live_in);
      } else {
        intptr_t idx = defn->ssa_temp_index();
        live_in->Add(idx);
      }
    }
  }
}

void SSALivenessAnalysis::ComputeInitialSets() {
  const intptr_t block_count = postorder_.length();
  for (intptr_t i = 0; i < block_count; i++) {
    BlockEntryInstr* block = postorder_[i];

    BitVector* kill = kill_[i];
    BitVector* live_in = live_in_[i];

    // Iterate backwards starting at the last instruction.
    for (BackwardInstructionIterator it(block); !it.Done(); it.Advance()) {
      Instruction* current = it.Current();

      // Initialize location summary for instruction.
      current->InitializeLocationSummary(zone(), true);  // opt

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
      for (intptr_t j = 0; j < current->InputCount(); j++) {
        Value* input = current->InputAt(j);

        ASSERT(!locs->in(j).IsConstant() || input->BindsToConstant());
        if (locs->in(j).IsConstant()) continue;

        live_in->Add(input->definition()->ssa_temp_index());
        if (input->definition()->HasPairRepresentation()) {
          live_in->Add(ToSecondPairVreg(input->definition()->ssa_temp_index()));
        }
      }

      // Add non-argument uses from the deoptimization environment (pushed
      // arguments are not allocated by the register allocator).
      if (current->env() != NULL) {
        for (Environment::DeepIterator env_it(current->env()); !env_it.Done();
             env_it.Advance()) {
          Definition* defn = env_it.CurrentValue()->definition();
          if (defn->IsMaterializeObject()) {
            // MaterializeObject instruction is not in the graph.
            // Treat its inputs as part of the environment.
            DeepLiveness(defn->AsMaterializeObject(), live_in);
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
        ASSERT(phi != NULL);
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
          if (phi->HasPairRepresentation()) {
            const intptr_t second_use = ToSecondPairVreg(use);
            if (!kill_[pred->postorder_number()]->Contains(second_use)) {
              live_in_[pred->postorder_number()]->Add(second_use);
            }
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

UsePosition* LiveRange::AddUse(intptr_t pos, Location* location_slot) {
  ASSERT(location_slot != NULL);
  ASSERT((first_use_interval_->start_ <= pos) &&
         (pos <= first_use_interval_->end_));
  if (uses_ != NULL) {
    if ((uses_->pos() == pos) && (uses_->location_slot() == location_slot)) {
      return uses_;
    } else if (uses_->pos() < pos) {
      // If an instruction at position P is using the same value both as
      // a fixed register input and a non-fixed input (in this order) we will
      // add uses both at position P-1 and *then* P which will make
      // uses_ unsorted unless we account for it here.
      UsePosition* insert_after = uses_;
      while ((insert_after->next() != NULL) &&
             (insert_after->next()->pos() < pos)) {
        insert_after = insert_after->next();
      }

      UsePosition* insert_before = insert_after->next();
      while (insert_before != NULL && (insert_before->pos() == pos)) {
        if (insert_before->location_slot() == location_slot) {
          return insert_before;
        }
        insert_before = insert_before->next();
      }

      insert_after->set_next(
          new UsePosition(pos, insert_after->next(), location_slot));
      return insert_after->next();
    }
  }
  uses_ = new UsePosition(pos, uses_, location_slot);
  return uses_;
}

void LiveRange::AddSafepoint(intptr_t pos, LocationSummary* locs) {
  if (spill_slot().IsConstant() &&
      (locs->always_calls() && !locs->callee_safe_call())) {
    // Constants have pseudo spill slot assigned to them from
    // the very beginning. This means that we don't need to associate
    // "always_calls" safepoints with these ranges, because they will never
    // be spilled. We still need to associate slow-path safepoints because
    // a value might be allocated to a register across a slow-path call.
    return;
  }

  ASSERT(IsRealInstructionPosition(pos));
  SafepointPosition* safepoint = new SafepointPosition(pos, locs);

  if (first_safepoint_ == NULL) {
    ASSERT(last_safepoint_ == NULL);
    first_safepoint_ = last_safepoint_ = safepoint;
  } else {
    ASSERT(last_safepoint_ != NULL);
    // We assume that safepoints list is sorted by position and that
    // safepoints are added in this order.
    ASSERT(last_safepoint_->pos() < pos);
    last_safepoint_->set_next(safepoint);
    last_safepoint_ = safepoint;
  }
}

void LiveRange::AddHintedUse(intptr_t pos,
                             Location* location_slot,
                             Location* hint) {
  ASSERT(hint != NULL);
  AddUse(pos, location_slot)->set_hint(hint);
}

void LiveRange::AddUseInterval(intptr_t start, intptr_t end) {
  ASSERT(start < end);

  // Live ranges are being build by visiting instructions in post-order.
  // This implies that use intervals will be prepended in a monotonically
  // decreasing order.
  if (first_use_interval() != NULL) {
    // If the first use interval and the use interval we are adding
    // touch then we can just extend the first interval to cover their
    // union.
    if (start > first_use_interval()->start()) {
      // The only case when we can add intervals with start greater than
      // start of an already created interval is BlockLocation.
      ASSERT(vreg() == kNoVirtualRegister);
      ASSERT(end <= first_use_interval()->end());
      return;
    } else if (start == first_use_interval()->start()) {
      // Grow first interval if necessary.
      if (end <= first_use_interval()->end()) return;
      first_use_interval_->end_ = end;
      return;
    } else if (end == first_use_interval()->start()) {
      first_use_interval()->start_ = start;
      return;
    }

    ASSERT(end <= first_use_interval()->start());
  }

  first_use_interval_ = new UseInterval(start, end, first_use_interval_);
  if (last_use_interval_ == NULL) {
    ASSERT(first_use_interval_->next() == NULL);
    last_use_interval_ = first_use_interval_;
  }
}

void LiveRange::DefineAt(intptr_t pos) {
  // Live ranges are being build by visiting instructions in post-order.
  // This implies that use intervals will be prepended in a monotonically
  // decreasing order.
  // When we encounter a use of a value inside a block we optimistically
  // expand the first use interval to cover the block from the start
  // to the last use in the block and then we shrink it if we encounter
  // definition of the value inside the same block.
  if (first_use_interval_ == NULL) {
    // Definition without a use.
    first_use_interval_ = new UseInterval(pos, pos + 1, NULL);
    last_use_interval_ = first_use_interval_;
  } else {
    // Shrink the first use interval. It was optimistically expanded to
    // cover the block from the start to the last use in the block.
    ASSERT(first_use_interval_->start_ <= pos);
    first_use_interval_->start_ = pos;
  }
}

LiveRange* FlowGraphAllocator::GetLiveRange(intptr_t vreg) {
  if (live_ranges_[vreg] == NULL) {
    Representation rep = value_representations_[vreg];
    ASSERT(rep != kNoRepresentation);
    live_ranges_[vreg] = new LiveRange(vreg, rep);
  }
  return live_ranges_[vreg];
}

LiveRange* FlowGraphAllocator::MakeLiveRangeForTemporary() {
  // Representation does not matter for temps.
  Representation ignored = kNoRepresentation;
  LiveRange* range = new LiveRange(kTempVirtualRegister, ignored);
#if defined(DEBUG)
  temporaries_.Add(range);
#endif
  return range;
}

void FlowGraphAllocator::BlockRegisterLocation(Location loc,
                                               intptr_t from,
                                               intptr_t to,
                                               bool* blocked_registers,
                                               LiveRange** blocking_ranges) {
  if (blocked_registers[loc.register_code()]) {
    return;
  }

  if (blocking_ranges[loc.register_code()] == NULL) {
    Representation ignored = kNoRepresentation;
    LiveRange* range = new LiveRange(kNoVirtualRegister, ignored);
    blocking_ranges[loc.register_code()] = range;
    range->set_assigned_location(loc);
#if defined(DEBUG)
    temporaries_.Add(range);
#endif
  }

  blocking_ranges[loc.register_code()]->AddUseInterval(from, to);
}

// Block location from the start of the instruction to its end.
void FlowGraphAllocator::BlockLocation(Location loc,
                                       intptr_t from,
                                       intptr_t to) {
  if (loc.IsRegister()) {
    BlockRegisterLocation(loc, from, to, blocked_cpu_registers_, cpu_regs_);
  } else if (loc.IsFpuRegister()) {
    BlockRegisterLocation(loc, from, to, blocked_fpu_registers_, fpu_regs_);
  } else {
    UNREACHABLE();
  }
}

void FlowGraphAllocator::BlockCpuRegisters(intptr_t registers,
                                           intptr_t from,
                                           intptr_t to) {
  for (intptr_t r = 0; r < kNumberOfCpuRegisters; r++) {
    if ((registers & (1 << r)) != 0) {
      BlockLocation(Location::RegisterLocation(static_cast<Register>(r)), from,
                    to);
    }
  }
}

void FlowGraphAllocator::BlockFpuRegisters(intptr_t fpu_registers,
                                           intptr_t from,
                                           intptr_t to) {
  for (intptr_t r = 0; r < kNumberOfFpuRegisters; r++) {
    if ((fpu_registers & (1 << r)) != 0) {
      BlockLocation(Location::FpuRegisterLocation(static_cast<FpuRegister>(r)),
                    from, to);
    }
  }
}

void LiveRange::Print() {
  if (first_use_interval() == NULL) {
    return;
  }

  THR_Print("  live range v%" Pd " [%" Pd ", %" Pd ") in ", vreg(), Start(),
            End());
  assigned_location().Print();
  if (!spill_slot_.IsInvalid() && !spill_slot_.IsConstant()) {
    THR_Print(" assigned spill slot: %s", spill_slot_.ToCString());
  }
  THR_Print("\n");

  SafepointPosition* safepoint = first_safepoint();
  while (safepoint != NULL) {
    THR_Print("    Safepoint [%" Pd "]: ", safepoint->pos());
    safepoint->locs()->stack_bitmap().Print();
    THR_Print("\n");
    safepoint = safepoint->next();
  }

  UsePosition* use_pos = uses_;
  for (UseInterval* interval = first_use_interval_; interval != NULL;
       interval = interval->next()) {
    THR_Print("    use interval [%" Pd ", %" Pd ")\n", interval->start(),
              interval->end());
    while ((use_pos != NULL) && (use_pos->pos() <= interval->end())) {
      THR_Print("      use at %" Pd "", use_pos->pos());
      if (use_pos->location_slot() != NULL) {
        THR_Print(" as ");
        use_pos->location_slot()->Print();
      }
      THR_Print("\n");
      use_pos = use_pos->next();
    }
  }

  if (next_sibling() != NULL) {
    next_sibling()->Print();
  }
}

void FlowGraphAllocator::PrintLiveRanges() {
#if defined(DEBUG)
  for (intptr_t i = 0; i < temporaries_.length(); i++) {
    temporaries_[i]->Print();
  }
#endif

  for (intptr_t i = 0; i < live_ranges_.length(); i++) {
    if (live_ranges_[i] != NULL) {
      live_ranges_[i]->Print();
    }
  }
}

// Returns true if all uses of the given range inside the
// given loop boundary have Any allocation policy.
static bool HasOnlyUnconstrainedUsesInLoop(LiveRange* range,
                                           intptr_t boundary) {
  UsePosition* use = range->first_use();
  while ((use != NULL) && (use->pos() < boundary)) {
    if (!use->location_slot()->Equals(Location::Any())) {
      return false;
    }
    use = use->next();
  }
  return true;
}

// Returns true if all uses of the given range have Any allocation policy.
static bool HasOnlyUnconstrainedUses(LiveRange* range) {
  UsePosition* use = range->first_use();
  while (use != NULL) {
    if (!use->location_slot()->Equals(Location::Any())) {
      return false;
    }
    use = use->next();
  }
  return true;
}

void FlowGraphAllocator::ProcessInitialDefinitions(
    BlockEntryWithInitialDefs* block) {
  const auto process_def = [&](Definition* defn, intptr_t index,
                               intptr_t pair_index) {
    LiveRange* range = GetLiveRange(defn->ssa_temp_index() + pair_index);
    range->AddUseInterval(block->start_pos(), block->start_pos() + 1);
    range->DefineAt(block->start_pos());
    ProcessInitialDefinition(defn, range, block, index, pair_index == 1);
  };
  const auto& definitions = *block->initial_definitions();
  for (intptr_t i = 0; i < definitions.length(); i++) {
    const auto defn = definitions[i];
    if (defn->HasPairRepresentation()) {
      // The lower bits are pushed after the higher bits.
      process_def(defn, i, 1);
    }
    process_def(defn, i, 0);
  }
}

void FlowGraphAllocator::BuildLiveRanges() {
  const intptr_t block_count = postorder_.length();
  ASSERT(postorder_.Last()->IsGraphEntry());
  BitVector* current_interference_set = NULL;
  Zone* zone = flow_graph_.zone();
  for (intptr_t i = 0; i < (block_count - 1); i++) {
    BlockEntryInstr* block = postorder_[i];
    ASSERT(BlockEntryAt(block->start_pos()) == block);

    // For every SSA value that is live out of this block, create an interval
    // that covers the whole block.  It will be shortened if we encounter a
    // definition of this value in this block.
    for (BitVector::Iterator it(liveness_.GetLiveOutSetAt(i)); !it.Done();
         it.Advance()) {
      LiveRange* range = GetLiveRange(it.Current());
      range->AddUseInterval(block->start_pos(), block->end_pos());
    }

    LoopInfo* loop_info = block->loop_info();
    if ((loop_info != nullptr) && (loop_info->IsBackEdge(block))) {
      BitVector* backedge_interference =
          extra_loop_info_[loop_info->id()]->backedge_interference;
      if (backedge_interference != nullptr) {
        // Restore interference for subsequent backedge a loop
        // (perhaps inner loop's header reset set in the meanwhile).
        current_interference_set = backedge_interference;
      } else {
        // All values flowing into the loop header are live at the
        // back edge and can interfere with phi moves.
        current_interference_set = new (zone)
            BitVector(zone, flow_graph_.max_virtual_register_number());
        current_interference_set->AddAll(
            liveness_.GetLiveInSet(loop_info->header()));
        extra_loop_info_[loop_info->id()]->backedge_interference =
            current_interference_set;
      }
    }

    // Connect outgoing phi-moves that were created in NumberInstructions
    // and find last instruction that contributes to liveness.
    Instruction* current =
        ConnectOutgoingPhiMoves(block, current_interference_set);

    // Now process all instructions in reverse order.
    while (current != block) {
      // Skip parallel moves that we insert while processing instructions.
      if (!current->IsParallelMove()) {
        ProcessOneInstruction(block, current, current_interference_set);
      }
      current = current->previous();
    }

    // Check if any values live into the loop can be spilled for free.
    if (block->IsLoopHeader()) {
      ASSERT(loop_info != nullptr);
      current_interference_set = nullptr;
      for (BitVector::Iterator it(liveness_.GetLiveInSetAt(i)); !it.Done();
           it.Advance()) {
        LiveRange* range = GetLiveRange(it.Current());
        intptr_t loop_end = extra_loop_info_[loop_info->id()]->end;
        if (HasOnlyUnconstrainedUsesInLoop(range, loop_end)) {
          range->MarkHasOnlyUnconstrainedUsesInLoop(loop_info->id());
        }
      }
    }

    if (auto join_entry = block->AsJoinEntry()) {
      ConnectIncomingPhiMoves(join_entry);
    } else if (auto catch_entry = block->AsCatchBlockEntry()) {
      ProcessEnvironmentUses(catch_entry, catch_entry);  // For lazy deopt
      ProcessInitialDefinitions(catch_entry);
    } else if (auto entry = block->AsBlockEntryWithInitialDefs()) {
      ASSERT(block->IsFunctionEntry() || block->IsOsrEntry());
      ProcessInitialDefinitions(entry);
    }
  }

  // Process incoming parameters and constants.  Do this after all other
  // instructions so that safepoints for all calls have already been found.
  GraphEntryInstr* graph_entry = flow_graph_.graph_entry();
  ProcessInitialDefinitions(graph_entry);
}

static Register RegisterForSpecialParameter(SpecialParameterInstr* param) {
  switch (param->kind()) {
    case SpecialParameterInstr::kException:
      return kExceptionObjectReg;
    case SpecialParameterInstr::kStackTrace:
      return kStackTraceObjectReg;
    case SpecialParameterInstr::kArgDescriptor:
      return ARGS_DESC_REG;
    default:
      UNREACHABLE();
      return kNoRegister;
  }
}

bool FlowGraphAllocator::IsSuspendStateParameter(Definition* defn) {
  if (auto param = defn->AsParameter()) {
    if ((param->GetBlock()->IsOsrEntry() ||
         param->GetBlock()->IsCatchBlockEntry()) &&
        flow_graph_.SuspendStateVar() != nullptr &&
        param->index() == flow_graph_.SuspendStateEnvIndex()) {
      return true;
    }
  }
  return false;
}

void FlowGraphAllocator::AllocateSpillSlotForInitialDefinition(
    intptr_t slot_index,
    intptr_t range_end) {
  if (slot_index < spill_slots_.length()) {
    // Multiple initial definitions could exist for the same spill slot
    // as function could have both OsrEntry and CatchBlockEntry.
    spill_slots_[slot_index] =
        Utils::Maximum(spill_slots_[slot_index], range_end);
    ASSERT(!quad_spill_slots_[slot_index]);
    ASSERT(!untagged_spill_slots_[slot_index]);
  } else {
    while (spill_slots_.length() < slot_index) {
      spill_slots_.Add(kMaxPosition);
      quad_spill_slots_.Add(false);
      untagged_spill_slots_.Add(false);
    }
    spill_slots_.Add(range_end);
    quad_spill_slots_.Add(false);
    untagged_spill_slots_.Add(false);
  }
}

void FlowGraphAllocator::ProcessInitialDefinition(
    Definition* defn,
    LiveRange* range,
    BlockEntryInstr* block,
    intptr_t initial_definition_index,
    bool second_location_for_definition) {
  // Save the range end because it may change below.
  const intptr_t range_end = range->End();

  Location loc;
  if (auto param = defn->AsParameter()) {
    loc = ComputeParameterLocation(block, param, param->base_reg(),
                                   second_location_for_definition ? 1 : 0);
  } else if (auto param = defn->AsSpecialParameter()) {
    loc = Location::RegisterLocation(RegisterForSpecialParameter(param));
  } else {
    ConstantInstr* constant = defn->AsConstant();
    ASSERT(constant != NULL);
    const intptr_t pair_index = second_location_for_definition ? 1 : 0;
    loc = Location::Constant(constant, pair_index);
  }

  AssignSafepoints(defn, range);
  if (loc.IsRegister()) {
    ProcessFixedOutput(range, block->start_pos(), loc);
    if (range->first_use() != nullptr) {
      CompleteRange(range, defn->RegisterKindForResult());
    }
    return;
  }

  // If initial definition does not arrive in a register then don't eagerly
  // allocate one for it. Instead split this range into two parts: initial
  // segment which is allocated to the given location and the sibling
  // which needs to be allocated.
  range->set_assigned_location(loc);
  range->set_spill_slot(loc);

  range->finger()->Initialize(range);
  UsePosition* use =
      range->finger()->FirstRegisterBeneficialUse(block->start_pos());
  if (use != NULL) {
    LiveRange* tail = SplitBetween(range, block->start_pos(), use->pos() + 1);
    if (tail != nullptr) {
      CompleteRange(tail, defn->RegisterKindForResult());
    }
  }
  ConvertAllUses(range);

  // If we have allocated the spill slot for this range make sure to
  // update spill slots state.
  Location spill_slot = range->spill_slot();
  if (spill_slot.IsStackSlot() && spill_slot.base_reg() == FPREG &&
      spill_slot.stack_index() <=
          compiler::target::frame_layout.first_local_from_fp &&
      !IsSuspendStateParameter(defn)) {
    // On entry to the function, range is stored on the stack above the FP in
    // the same space which is used for spill slots. Update spill slot state to
    // reflect that and prevent register allocator from reusing this space as a
    // spill slot.
    // Do not allocate spill slot for OSR parameter corresponding to
    // a synthetic :suspend_state variable as it is already allocated
    // in AllocateSpillSlotForSuspendState.
    ASSERT(defn->IsParameter());
    ASSERT(defn->AsParameter()->index() == initial_definition_index);
    const intptr_t spill_slot_index =
        -compiler::target::frame_layout.VariableIndexForFrameSlot(
            spill_slot.stack_index());
    AllocateSpillSlotForInitialDefinition(spill_slot_index, range_end);
    // Note, all incoming parameters are assumed to be tagged.
    MarkAsObjectAtSafepoints(range);
  } else if (defn->IsConstant() && block->IsCatchBlockEntry() &&
             (initial_definition_index >=
              flow_graph_.num_direct_parameters())) {
    // Constants at catch block entries consume spill slots.
    AllocateSpillSlotForInitialDefinition(
        initial_definition_index - flow_graph_.num_direct_parameters(),
        range_end);
  }
}

static Location::Kind RegisterKindFromPolicy(Location loc) {
  if (loc.policy() == Location::kRequiresFpuRegister) {
    return Location::kFpuRegister;
  } else {
    return Location::kRegister;
  }
}

//
// When describing shape of live ranges in comments below we are going to use
// the following notation:
//
//    B    block entry
//    g g' start and end of goto instruction
//    i i' start and end of any other instruction
//    j j' start and end of any other instruction

//    -  body of a use interval
//    [  start of a use interval
//    )  end of a use interval
//    *  use
//
// For example diagram
//
//           i  i'
//  value  --*--)
//
// can be read as: use interval for value starts somewhere before instruction
// and extends until currently processed instruction, there is a use of value
// at the start of the instruction.
//

Instruction* FlowGraphAllocator::ConnectOutgoingPhiMoves(
    BlockEntryInstr* block,
    BitVector* interfere_at_backedge) {
  Instruction* last = block->last_instruction();

  GotoInstr* goto_instr = last->AsGoto();
  if (goto_instr == NULL) return last;

  // If we have a parallel move here then the successor block must be a
  // join with phis.  The phi inputs contribute uses to each predecessor
  // block (and the phi outputs contribute definitions in the successor
  // block).
  JoinEntryInstr* join = goto_instr->successor();
  ASSERT(join != NULL);

  if (!goto_instr->previous()->IsParallelMove()) return goto_instr->previous();
  ParallelMoveInstr* parallel_move = goto_instr->previous()->AsParallelMove();

  // All uses are recorded at the parallel move before the Goto instruction.
  // Expected shape of live ranges:
  //
  //                 m  g
  //      value    --*
  //
  const intptr_t move_pos =
      ToPreviousMovePosition(GetLifetimePosition(goto_instr));
  RELEASE_ASSERT(IsParallelMovePosition(move_pos));

  // Search for the index of the current block in the predecessors of
  // the join.
  const intptr_t pred_index = join->IndexOfPredecessor(block);

  // Record the corresponding phi input use for each phi.
  const auto handle_value = [&](PhiInstr* phi, Definition* defn,
                                intptr_t move_index, intptr_t pair_index) {
    const auto src =
        parallel_move->MoveOperandsAt(move_index + pair_index)->src_slot();
    if (auto constant = defn->AsConstant()) {
      *src = Location::Constant(constant, pair_index);
    } else {
      const auto vreg = defn->ssa_temp_index();
      if (interfere_at_backedge != nullptr) {
        interfere_at_backedge->Add(vreg + pair_index);
      }

      auto use = AddUseAtStart(vreg + pair_index, block, move_pos, src);
      use->set_hint(GetLiveRange(phi->ssa_temp_index() + pair_index)
                        ->assigned_location_slot());
      *src = Location::PrefersRegister();
    }
  };

  TRACE_ALLOC(THR_Print("processing join B%" Pd " <- B%" Pd "\n",
                        join->block_id(), block->block_id()));

  intptr_t move_index = 0;
  for (PhiIterator it(join); !it.Done(); it.Advance()) {
    auto phi = it.Current();
    auto defn = phi->InputAt(pred_index)->definition();
    if (defn->HasPairRepresentation()) {
      for (auto pair_index : {0, 1}) {
        handle_value(phi, defn, move_index, pair_index);
      }
      move_index += 2;
    } else {
      handle_value(phi, defn, move_index,
                   /*pair_index=*/0);
      move_index += 1;
    }
  }

  // Begin backward iteration with the instruction before the parallel
  // move.
  return goto_instr->previous();
}

void FlowGraphAllocator::ConnectIncomingPhiMoves(JoinEntryInstr* join) {
  // For join blocks we need to add destinations of phi resolution moves
  // to phi's live range so that register allocator will fill them with moves.

  // All uses are recorded at the start position in the block.
  //
  // Expected shape of live ranges:
  //
  //                 B
  //                 m   i
  //      value      [-------
  //
  const intptr_t pos = join->start_pos();
  RELEASE_ASSERT(IsParallelMovePosition(pos));
  const bool is_loop_header = join->IsLoopHeader();

  const auto handle_phi = [&](PhiInstr* phi, intptr_t move_idx,
                              intptr_t pair_index) {
    const auto vreg = phi->ssa_temp_index();
    LiveRange* range = GetLiveRange(vreg + pair_index);
    range->DefineAt(pos);  // Shorten live range.
    if (is_loop_header) {
      range->mark_loop_phi();
    }

    for (intptr_t pred_idx = 0; pred_idx < phi->InputCount(); pred_idx++) {
      BlockEntryInstr* pred = join->PredecessorAt(pred_idx);
      GotoInstr* goto_instr = pred->last_instruction()->AsGoto();
      ASSERT((goto_instr != NULL) &&
             (goto_instr->previous()->IsParallelMove()));
      auto parallel_move = goto_instr->previous()->AsParallelMove();
      auto move = parallel_move->MoveOperandsAt(move_idx + pair_index);
      move->set_dest(Location::PrefersRegister());
      range->AddUse(pos, move->dest_slot());
    }

    AssignSafepoints(phi, range);
    CompleteRange(range, phi->RegisterKindForResult());
  };

  intptr_t move_idx = 0;
  for (PhiIterator it(join); !it.Done(); it.Advance()) {
    PhiInstr* phi = it.Current();
    ASSERT(phi != NULL);

    if (phi->HasPairRepresentation()) {
      for (auto pair_index : {0, 1}) {
        handle_phi(phi, move_idx, pair_index);
      }
      move_idx += 2;
    } else {
      handle_phi(phi, move_idx, /*pair_index=*/0);
      move_idx += 1;
    }
  }
}

UsePosition* FlowGraphAllocator::AddUse(intptr_t vreg,
                                        BlockEntryInstr* block,
                                        intptr_t use_pos,
                                        Location* slot) {
  return AddUse(GetLiveRange(vreg), block, use_pos, slot);
}

UsePosition* FlowGraphAllocator::AddUseAtStart(intptr_t vreg,
                                               BlockEntryInstr* block,
                                               intptr_t use_pos,
                                               Location* slot) {
  return AddUseAtStart(GetLiveRange(vreg), block, use_pos, slot);
}

UsePosition* FlowGraphAllocator::AddUse(LiveRange* range,
                                        BlockEntryInstr* block,
                                        intptr_t use_pos,
                                        Location* slot) {
  range->AddUseInterval(block->start_pos(), use_pos + 1);
  return range->AddUse(use_pos, slot);
}

UsePosition* FlowGraphAllocator::AddUseAtStart(LiveRange* range,
                                               BlockEntryInstr* block,
                                               intptr_t use_pos,
                                               Location* slot) {
  range->AddUseInterval(block->start_pos(), use_pos);
  return range->AddUse(use_pos, slot);
}

void FlowGraphAllocator::ProcessOneEnvironmentUse(BlockEntryInstr* block,
                                                  intptr_t use_pos,
                                                  Definition* def,
                                                  Location* slot) {
  if (auto constant = def->AsConstant()) {
    *slot = Location::Constant(constant);
  }
  if (def->HasPairRepresentation()) {
    *slot = Location::Pair(Location::Any(), Location::Any());
    PairLocation* location_pair = slot->AsPairLocation();
    AddUse(def->ssa_temp_index(), block, use_pos, location_pair->SlotAt(0));
    AddUse(ToSecondPairVreg(def->ssa_temp_index()), block, use_pos,
           location_pair->SlotAt(1));
  } else if (auto mat = def->AsMaterializeObject()) {
    // MaterializeObject itself produces no value. But its uses
    // are treated as part of the environment: allocated locations
    // will be used when building deoptimization data.
    *slot = Location::NoLocation();
    ProcessMaterializationUses(block, use_pos, mat);
  } else {
    *slot = Location::Any();
    AddUse(def->ssa_temp_index(), block, use_pos, slot);
  }
}

void FlowGraphAllocator::ProcessEnvironmentUses(BlockEntryInstr* block,
                                                Instruction* current) {
  ASSERT(current->env() != NULL);
  Environment* env = current->env();
  while (env != NULL) {
    // Any value mentioned in the deoptimization environment should survive
    // until the end of instruction but it does not need to be in the register.
    //
    // Expected shape of live range:
    //
    //                 i  m
    //      value    --*--)
    //

    if (env->Length() == 0) {
      env = env->outer();
      continue;
    }

    const intptr_t use_pos = GetLifetimePosition(current);  // TODO(XXX)

    Location* locations = flow_graph_.zone()->Alloc<Location>(env->Length());

    for (intptr_t i = 0; i < env->Length(); ++i) {
      Value* value = env->ValueAt(i);
      Definition* def = value->definition();

      if (env->outer() == nullptr && flow_graph_.SuspendStateVar() != nullptr &&
          i == flow_graph_.SuspendStateEnvIndex()) {
        // Make sure synthetic :suspend_state variable gets a correct
        // location on the stack frame. It is used by deoptimization.
        const intptr_t slot_index =
            compiler::target::frame_layout.FrameSlotForVariable(
                flow_graph_.parsed_function().suspend_state_var());
        locations[i] = Location::StackSlot(slot_index, FPREG);
        if (!def->IsConstant()) {
          // Update live intervals for Parameter/Phi definitions
          // corresponding to :suspend_state in OSR and try/catch cases as
          // they are still used when resolving control flow.
          ASSERT(def->IsParameter() || def->IsPhi());
          ASSERT(!def->HasPairRepresentation());
          LiveRange* range = GetLiveRange(def->ssa_temp_index());
          range->AddUseInterval(block->start_pos(), use_pos);
        }
        continue;
      }

      if (def->IsPushArgument()) {
        // Frame size is unknown until after allocation.
        locations[i] = Location::NoLocation();
        continue;
      }

      ProcessOneEnvironmentUse(block, use_pos, def, &locations[i]);
    }

    env->set_locations(locations);
    env = env->outer();
  }
}

void FlowGraphAllocator::ProcessMaterializationUses(
    BlockEntryInstr* block,
    const intptr_t use_pos,
    MaterializeObjectInstr* mat) {
  // Materialization can occur several times in the same environment.
  // Check if we already processed this one.
  if (mat->locations() != NULL) {
    return;  // Already processed.
  }

  // Initialize location for every input of the MaterializeObject instruction.
  Location* locations = flow_graph_.zone()->Alloc<Location>(mat->InputCount());
  mat->set_locations(locations);

  for (intptr_t i = 0; i < mat->InputCount(); ++i) {
    Definition* def = mat->InputAt(i)->definition();
    ProcessOneEnvironmentUse(block, use_pos, def, &locations[i]);
  }
}

void FlowGraphAllocator::ProcessOneInput(BlockEntryInstr* block,
                                         intptr_t pos,
                                         Location* in_ref,
                                         Value* input,
                                         intptr_t vreg,
                                         RegisterSet* live_registers) {
  ASSERT(IsRealInstructionPosition(pos));
  ASSERT(in_ref != NULL);
  ASSERT(!in_ref->IsPairLocation());
  ASSERT(input != NULL);
  ASSERT(block != NULL);
  LiveRange* range = GetLiveRange(vreg);
  if (in_ref->IsMachineRegister()) {
    ASSERT(!in_ref->IsRegister() ||
           ((1 << in_ref->reg()) & kDartAvailableCpuRegs) != 0);

    // Input is expected in a fixed register. Expected shape of
    // live ranges:
    //
    //                 m  i  m'
    //      value    --*
    //      register   [-----)
    //
    if (live_registers != NULL) {
      live_registers->Add(*in_ref, range->representation());
    }

    const intptr_t move_pos = ToPreviousMovePosition(pos);
    MoveOperands* move = AddMoveAt(move_pos, *in_ref, Location::Any());
    AddUseAtStart(vreg, block, move_pos, move->src_slot())->set_hint(in_ref);

    BlockLocation(*in_ref, move_pos, pos + 1);
  } else if (in_ref->IsUnallocated()) {
    if (in_ref->policy() == Location::kWritableRegister) {
      // Writable unallocated input. Expected shape of
      // live ranges:
      //
      //                 m  i  m'
      //      value    --*
      //      temp       *--*--)
      const intptr_t move_pos = ToPreviousMovePosition(pos);

      MoveOperands* move = AddMoveAt(move_pos, Location::RequiresRegister(),
                                     Location::PrefersRegister());
      AddUseAtStart(vreg, block, move_pos, move->src_slot());

      // Create live range for the temporary.
      LiveRange* temp = MakeLiveRangeForTemporary();
      temp->AddUseInterval(move_pos, pos + 1);
      temp->AddHintedUse(pos, in_ref, move->src_slot());
      temp->AddUse(pos, move->dest_slot());
      *in_ref = Location::RequiresRegister();
      CompleteRange(temp, RegisterKindFromPolicy(*in_ref));
    } else {
      // Normal unallocated input. Expected shape of
      // live ranges:
      //
      //                 i  i'
      //      value    --*---
      //
      AddUse(range, block, pos, in_ref);
    }
  } else {
    ASSERT(in_ref->IsConstant());
  }
}

static ParallelMoveInstr* CreateParallelMoveBefore(Instruction* instr) {
  if (!(!instr->IsParallelMove() && !instr->IsBlockEntry())) {
    OS::PrintErr("trying to create a parallel move before %s\n",
                 instr->ToCString());
  }
  ASSERT(!instr->IsParallelMove() && !instr->IsBlockEntry());
  Instruction* prev = instr->previous();
  ParallelMoveInstr* move = prev->AsParallelMove();
  const auto move_pos = FlowGraphAllocator::GetLifetimePosition(instr) - 1;
  if (move == NULL ||
      FlowGraphAllocator::GetLifetimePosition(move) != move_pos) {
    move = new ParallelMoveInstr();
    prev->LinkTo(move);
    move->LinkTo(instr);
    FlowGraphAllocator::SetLifetimePosition(move, move_pos);
  }
  return move;
}

static ParallelMoveInstr* CreateParallelMoveAfter(Instruction* instr) {
  ASSERT(!instr->IsParallelMove() && !instr->IsBlockEntry());
  Instruction* next = instr->next();
  ParallelMoveInstr* move = next->AsParallelMove();
  if (move == NULL) {
    move = new ParallelMoveInstr();
    instr->LinkTo(move);
    move->LinkTo(next);
    FlowGraphAllocator::SetLifetimePosition(
        move, FlowGraphAllocator::GetLifetimePosition(instr) + 1);
  }
  return move;
}

static ParallelMoveInstr* GetBlockEntryMove(BlockEntryInstr* block) {
  auto next = block->next();
  auto move = next->AsParallelMove();
  if (move == nullptr ||
      FlowGraphAllocator::GetLifetimePosition(move) != block->start_pos()) {
    move = new ParallelMoveInstr();
    block->LinkTo(move);
    move->LinkTo(next);
    FlowGraphAllocator::SetLifetimePosition(move, block->start_pos());
  }
  return move;
}

void FlowGraphAllocator::ProcessFixedOutput(LiveRange* range,
                                            intptr_t move_pos,
                                            Location out) {
  // Emit move connecting fixed register with another location that will be
  // allocated for this output's live range.

  // If there are any uses that occur at the move itself - connect
  // them to the output location.
  for (UsePosition* use = range->first_use(); use != nullptr;
       use = use->next()) {
    if (use->pos() == move_pos) {
      // Allocate and then drop this use.
      ASSERT(use->location_slot()->IsUnallocated());
      *(use->location_slot()) = out;
      range->set_first_use(use->next());
    } else {
      ASSERT(use->pos() > move_pos);  // sorted
      break;
    }
  }
  if (range->first_use() == nullptr) {
    // All uses are immediately connected to the fixed output.
    return;
  }

  // Create a move which moves the fixed location into the location
  // of the range.
  ASSERT(IsParallelMovePosition(move_pos));
  auto instr = InstructionAt(move_pos + 1);
  auto parallel_move = instr->IsBlockEntry()
                           ? GetBlockEntryMove(instr->Cast<BlockEntryInstr>())
                           : CreateParallelMoveBefore(instr);
  auto move = parallel_move->AddMove(Location::Any(), out);
  range->AddHintedUse(move_pos, move->dest_slot(), move->src_slot());

  // Shorten live range to the point of definition. If there were any
  // uses at move_pos they would have been handled by the code above.
  // So we don't expect the resulting range to be empty.
  range->DefineAt(move_pos);
  RELEASE_ASSERT(range->Start() != range->End());
}

void FlowGraphAllocator::ProcessOneOutput(BlockEntryInstr* block,
                                          intptr_t pos,
                                          Location* out,
                                          Definition* def,
                                          intptr_t vreg,
                                          bool output_same_as_first_input,
                                          Location* in_ref,
                                          Definition* input,
                                          intptr_t input_vreg,
                                          BitVector* interference_set) {
  RELEASE_ASSERT(IsRealInstructionPosition(pos));
  ASSERT(out != NULL);
  ASSERT(!out->IsPairLocation());
  ASSERT(def != NULL);
  ASSERT(block != NULL);

  LiveRange* range =
      vreg >= 0 ? GetLiveRange(vreg) : MakeLiveRangeForTemporary();

  // Process output and finalize its liverange.
  if (out->IsMachineRegister()) {
    ASSERT(!out->IsRegister() ||
           ((1 << out->reg()) & kDartAvailableCpuRegs) != 0);
    const auto move_pos = ToNextMovePosition(pos);
    BlockLocation(*out, pos, move_pos);
    if (range->vreg() == kTempVirtualRegister) {
      // If the value has no uses then we are done.
      return;
    }
    ProcessFixedOutput(range, move_pos, *out);
    if (range->first_use() == nullptr) {
      return;
    }
  } else if (output_same_as_first_input) {
    ASSERT(in_ref != NULL);
    ASSERT(input != NULL);
    // Output register will contain a value of the first input at instruction's
    // start. Expected shape of live ranges:
    //
    //                 m  i  m'
    //    input #0   --*
    //    output       [--*----
    //
    ASSERT(in_ref->Equals(Location::RequiresRegister()) ||
           in_ref->Equals(Location::RequiresFpuRegister()));
    *out = *in_ref;
    // Create move that will copy value between input and output.
    const auto move_pos = ToPreviousMovePosition(pos);
    auto move =
        AddMoveAt(move_pos, Location::RequiresRegister(), Location::Any());
    // Add uses to the live range of the input.
    AddUseAtStart(input_vreg, block, move_pos, move->src_slot());

    // Shorten output live range to the point of definition and add both input
    // and output uses slots to be filled by allocator.
    range->DefineAt(move_pos);

    range->AddHintedUse(move_pos, out, move->src_slot());
    range->AddUse(move_pos, move->dest_slot());
    range->AddUse(pos, in_ref);

    if ((interference_set != NULL) && (range->vreg() >= 0) &&
        interference_set->Contains(range->vreg())) {
      interference_set->Add(input->ssa_temp_index());
    }
  } else {
    // Normal unallocated location that requires a register. Expected shape of
    // live range:
    //
    //                    i  i'
    //    output          [-------
    //
    ASSERT(out->Equals(Location::RequiresRegister()) ||
           out->Equals(Location::RequiresFpuRegister()));

    // Shorten live range to the point of definition and add use to be filled by
    // allocator.
    range->DefineAt(pos);
    range->AddUse(pos, out);
  }

  AssignSafepoints(def, range);
  CompleteRange(range, def->RegisterKindForResult());
}

// Create and update live ranges corresponding to instruction's inputs,
// temporaries and output.
void FlowGraphAllocator::ProcessOneInstruction(BlockEntryInstr* block,
                                               Instruction* current,
                                               BitVector* interference_set) {
  LocationSummary* locs = current->locs();

  Definition* def = current->AsDefinition();
  if ((def != NULL) && (def->AsConstant() != NULL)) {
    ASSERT(!def->HasPairRepresentation());
    LiveRange* range = (def->ssa_temp_index() != -1)
                           ? GetLiveRange(def->ssa_temp_index())
                           : NULL;

    // Drop definitions of constants that have no uses.
    if ((range == NULL) || (range->first_use() == NULL)) {
      locs->set_out(0, Location::NoLocation());
      return;
    }

    // If this constant has only unconstrained uses convert them all
    // to use the constant directly and drop this definition.
    // TODO(vegorov): improve allocation when we have enough registers to keep
    // constants used in the loop in them.
    if (HasOnlyUnconstrainedUses(range)) {
      ConstantInstr* constant_instr = def->AsConstant();
      range->set_assigned_location(Location::Constant(constant_instr));
      range->set_spill_slot(Location::Constant(constant_instr));
      range->finger()->Initialize(range);
      ConvertAllUses(range);

      locs->set_out(0, Location::NoLocation());
      return;
    }
  }

  const intptr_t pos = GetLifetimePosition(current);
  ASSERT(IsRealInstructionPosition(pos));

  ASSERT(locs->input_count() == current->InputCount());

  // Normalize same-as-first-input output if input is specified as
  // fixed register.
  if (locs->out(0).IsUnallocated() &&
      (locs->out(0).policy() == Location::kSameAsFirstInput)) {
    if (locs->in(0).IsPairLocation()) {
      // Pair input, pair output.
      PairLocation* in_pair = locs->in(0).AsPairLocation();
      ASSERT(in_pair->At(0).IsMachineRegister() ==
             in_pair->At(1).IsMachineRegister());
      if (in_pair->At(0).IsMachineRegister() &&
          in_pair->At(1).IsMachineRegister()) {
        locs->set_out(0, Location::Pair(in_pair->At(0), in_pair->At(1)));
      }
    } else if (locs->in(0).IsMachineRegister()) {
      // Single input, single output.
      locs->set_out(0, locs->in(0));
    }
  }

  const bool output_same_as_first_input =
      locs->out(0).IsUnallocated() &&
      (locs->out(0).policy() == Location::kSameAsFirstInput);

  // Output is same as first input which is a pair.
  if (output_same_as_first_input && locs->in(0).IsPairLocation()) {
    // Make out into a PairLocation.
    locs->set_out(0, Location::Pair(Location::RequiresRegister(),
                                    Location::RequiresRegister()));
  }
  // Add uses from the deoptimization environment.
  if (current->env() != NULL) ProcessEnvironmentUses(block, current);

  // Process inputs.
  // Skip the first input if output is specified with kSameAsFirstInput policy,
  // they will be processed together at the very end.
  {
    for (intptr_t j = output_same_as_first_input ? 1 : 0;
         j < locs->input_count(); j++) {
      // Determine if we are dealing with a value pair, and if so, whether
      // the location is the first register or second register.
      Value* input = current->InputAt(j);
      Location* in_ref = locs->in_slot(j);
      RegisterSet* live_registers = NULL;
      if (locs->HasCallOnSlowPath()) {
        live_registers = locs->live_registers();
      }
      if (in_ref->IsPairLocation()) {
        ASSERT(input->definition()->HasPairRepresentation());
        PairLocation* pair = in_ref->AsPairLocation();
        const intptr_t vreg = input->definition()->ssa_temp_index();
        // Each element of the pair is assigned it's own virtual register number
        // and is allocated its own LiveRange.
        ProcessOneInput(block, pos, pair->SlotAt(0), input, vreg,
                        live_registers);
        ProcessOneInput(block, pos, pair->SlotAt(1), input,
                        ToSecondPairVreg(vreg), live_registers);
      } else {
        ProcessOneInput(block, pos, in_ref, input,
                        input->definition()->ssa_temp_index(), live_registers);
      }
    }
  }

  // Process temps.
  for (intptr_t j = 0; j < locs->temp_count(); j++) {
    // Expected shape of live range:
    //
    //              i  m
    //              [--)
    //

    Location temp = locs->temp(j);
    // We do not support pair locations for temporaries.
    ASSERT(!temp.IsPairLocation());
    if (temp.IsMachineRegister()) {
      ASSERT(!temp.IsRegister() ||
             ((1 << temp.reg()) & kDartAvailableCpuRegs) != 0);
      BlockLocation(temp, pos, pos + 1);
    } else if (temp.IsUnallocated()) {
      LiveRange* range = MakeLiveRangeForTemporary();
      range->AddUseInterval(pos, pos + 1);
      range->AddUse(pos, locs->temp_slot(j));
      CompleteRange(range, RegisterKindFromPolicy(temp));
    } else {
      UNREACHABLE();
    }
  }

  // Block all volatile (i.e. not native ABI callee-save) registers.
  if (locs->native_leaf_call()) {
    BlockCpuRegisters(kDartVolatileCpuRegs, pos, pos + 1);
    BlockFpuRegisters(kAbiVolatileFpuRegs, pos, pos + 1);
#if defined(TARGET_ARCH_ARM)
    // We do not yet have a way to say that we only want FPU registers that
    // overlap S registers.
    // Block all Q/D FPU registers above the 8/16 that have S registers in
    // VFPv3-D32.
    // This way we avoid ending up trying to do single-word operations on
    // registers that don't support it.
    BlockFpuRegisters(kFpuRegistersWithoutSOverlap, pos, pos + 1);
#endif
  }

  // Block all allocatable registers for calls.
  if (locs->always_calls() && !locs->callee_safe_call()) {
    // Expected shape of live range:
    //
    //              i  i'
    //              [--)
    //
    // The stack bitmap describes the position i.
    BlockCpuRegisters(kAllCpuRegistersList, pos, pos + 1);

    BlockFpuRegisters(kAllFpuRegistersList, pos, pos + 1);

#if defined(DEBUG)
    // Verify that temps, inputs and output were specified as fixed
    // locations.  Every register is blocked now so attempt to
    // allocate will go on the stack.
    for (intptr_t j = 0; j < locs->temp_count(); j++) {
      ASSERT(!locs->temp(j).IsPairLocation());
      ASSERT(!locs->temp(j).IsUnallocated());
    }

    for (intptr_t j = 0; j < locs->input_count(); j++) {
      if (locs->in(j).IsPairLocation()) {
        PairLocation* pair = locs->in_slot(j)->AsPairLocation();
        ASSERT(!pair->At(0).IsUnallocated() ||
               pair->At(0).policy() == Location::kAny);
        ASSERT(!pair->At(1).IsUnallocated() ||
               pair->At(1).policy() == Location::kAny);
      } else {
        ASSERT(!locs->in(j).IsUnallocated() ||
               locs->in(j).policy() == Location::kAny);
      }
    }

    if (locs->out(0).IsPairLocation()) {
      PairLocation* pair = locs->out_slot(0)->AsPairLocation();
      ASSERT(!pair->At(0).IsUnallocated());
      ASSERT(!pair->At(1).IsUnallocated());
    } else {
      ASSERT(!locs->out(0).IsUnallocated());
    }
#endif
  }

  if (locs->can_call() && !locs->native_leaf_call()) {
    safepoints_.Add(current);
  }

  if (def == NULL) {
    ASSERT(locs->out(0).IsInvalid());
    return;
  }

  if (locs->out(0).IsInvalid()) {
    ASSERT(def->ssa_temp_index() < 0);
    return;
  }

  ASSERT(locs->output_count() == 1);
  Location* out = locs->out_slot(0);
  if (out->IsPairLocation()) {
    ASSERT(def->HasPairRepresentation());
    PairLocation* pair = out->AsPairLocation();
    if (output_same_as_first_input) {
      ASSERT(locs->in_slot(0)->IsPairLocation());
      PairLocation* in_pair = locs->in_slot(0)->AsPairLocation();
      Definition* input = current->InputAt(0)->definition();
      ASSERT(input->HasPairRepresentation());
      // Each element of the pair is assigned it's own virtual register number
      // and is allocated its own LiveRange.
      ProcessOneOutput(block, pos,             // BlockEntry, seq.
                       pair->SlotAt(0), def,   // (output) Location, Definition.
                       def->ssa_temp_index(),  // (output) virtual register.
                       true,                   // output mapped to first input.
                       in_pair->SlotAt(0), input,  // (input) Location, Def.
                       input->ssa_temp_index(),    // (input) virtual register.
                       interference_set);
      ProcessOneOutput(
          block, pos, pair->SlotAt(1), def,
          ToSecondPairVreg(def->ssa_temp_index()), true, in_pair->SlotAt(1),
          input, ToSecondPairVreg(input->ssa_temp_index()), interference_set);
    } else {
      // Each element of the pair is assigned it's own virtual register number
      // and is allocated its own LiveRange.
      ProcessOneOutput(block, pos, pair->SlotAt(0), def, def->ssa_temp_index(),
                       false,           // output is not mapped to first input.
                       NULL, NULL, -1,  // First input not needed.
                       interference_set);
      ProcessOneOutput(block, pos, pair->SlotAt(1), def,
                       ToSecondPairVreg(def->ssa_temp_index()), false, NULL,
                       NULL, -1, interference_set);
    }
  } else {
    if (output_same_as_first_input) {
      Location* in_ref = locs->in_slot(0);
      Definition* input = current->InputAt(0)->definition();
      ASSERT(!in_ref->IsPairLocation());
      ProcessOneOutput(block, pos,             // BlockEntry, Instruction, seq.
                       out, def,               // (output) Location, Definition.
                       def->ssa_temp_index(),  // (output) virtual register.
                       true,                   // output mapped to first input.
                       in_ref, input,          // (input) Location, Def.
                       input->ssa_temp_index(),  // (input) virtual register.
                       interference_set);
    } else {
      ProcessOneOutput(block, pos, out, def, def->ssa_temp_index(),
                       false,           // output is not mapped to first input.
                       NULL, NULL, -1,  // First input not needed.
                       interference_set);
    }
  }
}

// Linearize the control flow graph. The chosen order will be used by the
// linear-scan register allocator. Number most instructions with a pair of
// numbers representing lifetime positions.  Introduce explicit parallel
// move instructions in the predecessors of join nodes. The moves are used
// for phi resolution.
void FlowGraphAllocator::NumberInstructions() {
  // The basic block order is reverse postorder.
  const intptr_t block_count = postorder_.length();

  // Handle GraphEntry specially. It has position 0. All other blocks
  // start with odd positions.
  {
    BlockEntryInstr* graph_entry = postorder_[block_count - 1];
    RELEASE_ASSERT(graph_entry->IsGraphEntry());
    instructions_.Add(graph_entry);
    block_entries_.Add(graph_entry);
    graph_entry->set_start_pos(0);
    graph_entry->set_end_pos(1);
    SetLifetimePosition(graph_entry, 0);
  }

  intptr_t pos = 1;
  for (intptr_t i = block_count - 2; i >= 0; i--) {
    BlockEntryInstr* block = postorder_[i];
    RELEASE_ASSERT(IsParallelMovePosition(pos));

    block->set_start_pos(pos);
    SetLifetimePosition(block, pos);

    // TODO(XXX) document here.
    RELEASE_ASSERT(((pos + 1) / 2) == instructions_.length());
    instructions_.Add(block);
    block_entries_.Add(block);
    pos += 1;

    for (ForwardInstructionIterator it(block); !it.Done(); it.Advance()) {
      Instruction* current = it.Current();
      // Not expecting any parallel moves inserted before we start register
      // allocation.
      pos += 2;  // 1 for instruction and 1 for a possible parallel move.
      ASSERT(!current->IsParallelMove());
      RELEASE_ASSERT(IsRealInstructionPosition(pos));
      RELEASE_ASSERT((pos / 2) == instructions_.length());
      instructions_.Add(current);
      block_entries_.Add(block);
      SetLifetimePosition(current, pos);
    }
    pos++;  // No parallel move after the last instruction.
    block->set_end_pos(pos);
    RELEASE_ASSERT(block->start_pos() < block->end_pos());
  }

  // Create parallel moves in join predecessors.  This must be done after
  // all instructions are numbered.
  for (intptr_t i = block_count - 1; i >= 0; i--) {
    BlockEntryInstr* block = postorder_[i];

    // For join entry predecessors create phi resolution moves if
    // necessary. They will be populated by the register allocator.
    JoinEntryInstr* join = block->AsJoinEntry();
    if (join != NULL) {
      intptr_t move_count = 0;
      for (PhiIterator it(join); !it.Done(); it.Advance()) {
        move_count += it.Current()->HasPairRepresentation() ? 2 : 1;
      }
      for (intptr_t i = 0; i < block->PredecessorCount(); i++) {
        // Insert the move between the last two instructions of the
        // predecessor block (all such blocks have at least two instructions:
        // the block entry and goto instructions.)
        Instruction* last = block->PredecessorAt(i)->last_instruction();
        ASSERT(last->IsGoto());
        ASSERT(!last->previous()->IsParallelMove());

        ParallelMoveInstr* move = CreateParallelMoveBefore(last->AsGoto());
        // Populate the ParallelMove with empty moves.
        for (intptr_t j = 0; j < move_count; j++) {
          move->AddMove(Location::NoLocation(), Location::NoLocation());
        }
      }
    }
  }

  // Prepare some extra information for each loop.
  Zone* zone = flow_graph_.zone();
  const LoopHierarchy& loop_hierarchy = flow_graph_.loop_hierarchy();
  const intptr_t num_loops = loop_hierarchy.num_loops();
  for (intptr_t i = 0; i < num_loops; i++) {
    extra_loop_info_.Add(
        ComputeExtraLoopInfo(zone, loop_hierarchy.headers()[i]->loop_info()));
  }
}

Instruction* FlowGraphAllocator::InstructionAt(intptr_t pos) const {
  return instructions_[pos >> 1];
}

BlockEntryInstr* FlowGraphAllocator::BlockEntryAt(intptr_t pos) const {
  return block_entries_[(pos + 1) >> 1];
}

void AllocationFinger::Initialize(LiveRange* range) {
  first_pending_use_interval_ = range->first_use_interval();
  first_register_use_ = range->first_use();
  first_register_beneficial_use_ = range->first_use();
  first_hinted_use_ = range->first_use();
}

bool AllocationFinger::Advance(const intptr_t start) {
  UseInterval* a = first_pending_use_interval_;
  while (a != NULL && a->end() <= start)
    a = a->next();
  first_pending_use_interval_ = a;
  return (first_pending_use_interval_ == NULL);
}

Location AllocationFinger::FirstHint() {
  UsePosition* use = first_hinted_use_;

  while (use != NULL) {
    if (use->HasHint()) return use->hint();
    use = use->next();
  }

  return Location::NoLocation();
}

static UsePosition* FirstUseAfter(UsePosition* use, intptr_t after) {
  while ((use != NULL) && (use->pos() < after)) {
    use = use->next();
  }
  return use;
}

UsePosition* AllocationFinger::FirstRegisterUse(intptr_t after) {
  for (UsePosition* use = FirstUseAfter(first_register_use_, after);
       use != NULL; use = use->next()) {
    Location* loc = use->location_slot();
    if (loc->IsUnallocated() &&
        ((loc->policy() == Location::kRequiresRegister) ||
         (loc->policy() == Location::kRequiresFpuRegister))) {
      first_register_use_ = use;
      return use;
    }
  }
  return NULL;
}

UsePosition* AllocationFinger::FirstRegisterBeneficialUse(intptr_t after) {
  for (UsePosition* use = FirstUseAfter(first_register_beneficial_use_, after);
       use != NULL; use = use->next()) {
    Location* loc = use->location_slot();
    if (loc->IsUnallocated() && loc->IsRegisterBeneficial()) {
      first_register_beneficial_use_ = use;
      return use;
    }
  }
  return NULL;
}

UsePosition* AllocationFinger::FirstInterferingUse(intptr_t after) {
  if (IsParallelMovePosition(after)) {  // TODO(XXX)
    after += 1;
  }
  return FirstRegisterUse(after);
}

void AllocationFinger::UpdateAfterSplit(intptr_t first_use_after_split_pos) {
  if ((first_register_use_ != NULL) &&
      (first_register_use_->pos() >= first_use_after_split_pos)) {
    first_register_use_ = NULL;
  }

  if ((first_register_beneficial_use_ != NULL) &&
      (first_register_beneficial_use_->pos() >= first_use_after_split_pos)) {
    first_register_beneficial_use_ = NULL;
  }
}

intptr_t UseInterval::Intersect(UseInterval* other) {
  if (this->start() <= other->start()) {
    if (other->start() < this->end()) return other->start();
  } else if (this->start() < other->end()) {
    return this->start();
  }
  return kIllegalPosition;
}

static intptr_t FirstIntersection(UseInterval* a, UseInterval* u) {
  while (a != NULL && u != NULL) {
    const intptr_t pos = a->Intersect(u);
    if (pos != kIllegalPosition) return pos;

    if (a->start() < u->start()) {
      a = a->next();
    } else {
      u = u->next();
    }
  }

  return kMaxPosition;
}

template <typename PositionType>
PositionType* SplitListOfPositions(PositionType** head,
                                   intptr_t split_pos,
                                   bool split_at_start) {
  PositionType* last_before_split = NULL;
  PositionType* pos = *head;
  if (split_at_start) {
    while ((pos != NULL) && (pos->pos() < split_pos)) {
      last_before_split = pos;
      pos = pos->next();
    }
  } else {
    while ((pos != NULL) && (pos->pos() <= split_pos)) {
      last_before_split = pos;
      pos = pos->next();
    }
  }

  if (last_before_split == NULL) {
    *head = NULL;
  } else {
    last_before_split->set_next(NULL);
  }

  return pos;
}

LiveRange* LiveRange::SplitAt(intptr_t split_pos) {
  // We can only connect two live ranges at a parallel move.
  RELEASE_ASSERT(IsParallelMovePosition(split_pos));
  if (Start() == split_pos) {
    UNREACHABLE();
    /*
    next_sibling_ =
        new LiveRange(vreg(), representation(), uses_, first_use_interval_,
                      last_use_interval_, first_safepoint_, next_sibling_);
    uses_ = nullptr;
    first_safepoint_ = nullptr;
    first_use_interval_ = last_use_interval_ =
        new UseInterval(split_pos, split_pos, nullptr);
    finger_.Initialize(this);
    return next_sibling_; */
    return this;
  }
  if (End() == split_pos) return nullptr;

  UseInterval* interval = finger_.first_pending_use_interval();
  if (interval == NULL) {
    finger_.Initialize(this);
    interval = finger_.first_pending_use_interval();
  }

  ASSERT(split_pos < End());

  // Corner case. Split position can be inside the a lifetime hole or at its
  // end. We need to start over to find the previous interval.
  if (split_pos <= interval->start()) interval = first_use_interval_;

  UseInterval* last_before_split = NULL;
  while (interval->end() <= split_pos) {
    last_before_split = interval;
    interval = interval->next();
  }

  const bool split_at_start = (interval->start() == split_pos);

  UseInterval* first_after_split = interval;
  if (!split_at_start && interval->Contains(split_pos)) {
    first_after_split =
        new UseInterval(split_pos, interval->end(), interval->next());
    interval->end_ = split_pos;
    interval->next_ = first_after_split;
    last_before_split = interval;
  }

  ASSERT(last_before_split != NULL);
  ASSERT(last_before_split->next() == first_after_split);
  ASSERT(last_before_split->end() <= split_pos);
  ASSERT(split_pos <= first_after_split->start());

  UsePosition* first_use_after_split =
      SplitListOfPositions(&uses_, split_pos, split_at_start);

  SafepointPosition* first_safepoint_after_split =
      SplitListOfPositions(&first_safepoint_, split_pos, split_at_start);

  UseInterval* last_use_interval = (last_before_split == last_use_interval_)
                                       ? first_after_split
                                       : last_use_interval_;
  next_sibling_ = new LiveRange(vreg(), representation(), first_use_after_split,
                                first_after_split, last_use_interval,
                                first_safepoint_after_split, next_sibling_);

  TRACE_ALLOC(THR_Print("  split sibling [%" Pd ", %" Pd ")\n",
                        next_sibling_->Start(), next_sibling_->End()));

  last_use_interval_ = last_before_split;
  last_use_interval_->next_ = NULL;

  if (first_use_after_split != NULL) {
    finger_.UpdateAfterSplit(first_use_after_split->pos());
  }

  return next_sibling_;
}

LiveRange* FlowGraphAllocator::SplitBetween(LiveRange* range,
                                            intptr_t from,
                                            intptr_t to) {
  TRACE_ALLOC(THR_Print("split v%" Pd " [%" Pd ", %" Pd ") between [%" Pd
                        ", %" Pd ")\n",
                        range->vreg(), range->Start(), range->End(), from, to));

  intptr_t split_pos = kIllegalPosition;

  BlockEntryInstr* split_block_entry = BlockEntryAt(to - 1);
  RELEASE_ASSERT(split_block_entry == InstructionAt(to)->GetBlock());

  if (from < split_block_entry->start_pos()) {
    // Interval [from, to) spans multiple blocks.

    // If the last block is inside a loop, prefer splitting at the outermost
    // loop's header that follows the definition. Note that, as illustrated
    // below, if the potential split S linearly appears inside a loop, even
    // though it technically does not belong to the natural loop, we still
    // prefer splitting at the header H. Splitting in the "middle" of the loop
    // would disconnect the prefix of the loop from any block X that follows,
    // increasing the chance of "disconnected" allocations.
    //
    //            +--------------------+
    //            v                    |
    //            |loop|          |loop|
    // . . . . . . . . . . . . . . . . . . . . .
    //     def------------use     -----------
    //            ^      ^        ^
    //            H      S        X
    LoopInfo* loop_info = split_block_entry->loop_info();
    if (loop_info == nullptr) {
      const LoopHierarchy& loop_hierarchy = flow_graph_.loop_hierarchy();
      const intptr_t num_loops = loop_hierarchy.num_loops();
      for (intptr_t i = 0; i < num_loops; i++) {
        if (extra_loop_info_[i]->start < to && to < extra_loop_info_[i]->end) {
          // Split loop found!
          loop_info = loop_hierarchy.headers()[i]->loop_info();
          break;
        }
      }
    }
    while ((loop_info != nullptr) &&
           (from < GetLifetimePosition(loop_info->header()))) {
      split_block_entry = loop_info->header();
      loop_info = loop_info->outer();
      TRACE_ALLOC(THR_Print("  move back to loop header B%" Pd " at %" Pd "\n",
                            split_block_entry->block_id(),
                            GetLifetimePosition(split_block_entry)));
    }

    // Split at block's start.
    split_pos = GetLifetimePosition(split_block_entry);
  } else {
    // Interval [from, to) is contained inside a single block.

    // Always split at the parallel move position.
    split_pos = ToPreceedingMovePosition(to - 1);
  }

  TRACE_ALLOC(THR_Print("--- selected %" Pd "\n", split_pos));

  ASSERT(split_pos != kIllegalPosition);
  ASSERT(from <= split_pos);

  return range->SplitAt(split_pos);
}

void FlowGraphAllocator::SpillBetween(LiveRange* range,
                                      intptr_t from,
                                      intptr_t to) {
  ASSERT(from < to);
  TRACE_ALLOC(THR_Print("spill v%" Pd " [%" Pd ", %" Pd ") "
                        "between [%" Pd ", %" Pd ")\n",
                        range->vreg(), range->Start(), range->End(), from, to));
  LiveRange* tail = from == range->Start() ? range : range->SplitAt(from);

  if (tail->Start() < to) {
    // There is an intersection of tail and [from, to).
    LiveRange* tail_tail = SplitBetween(tail, tail->Start(), to);
    Spill(tail);
    AddToUnallocated(tail_tail);
  } else {
    // No intersection between tail and [from, to).
    AddToUnallocated(tail);
  }
}

void FlowGraphAllocator::SpillAfter(LiveRange* range, intptr_t from) {
  TRACE_ALLOC(THR_Print("spill v%" Pd " [%" Pd ", %" Pd ") after %" Pd "\n",
                        range->vreg(), range->Start(), range->End(), from));
  // When spilling the value inside the loop check if this spill can
  // be moved outside.
  LoopInfo* loop_info = BlockEntryAt(from)->loop_info();
  if (loop_info != nullptr) {
    if ((range->Start() <= loop_info->header()->start_pos()) &&
        RangeHasOnlyUnconstrainedUsesInLoop(range, loop_info->id())) {
      ASSERT(loop_info->header()->start_pos() <= from);
      from = loop_info->header()->start_pos();
      TRACE_ALLOC(
          THR_Print("  moved spill position to loop header %" Pd "\n", from));
    }
  }

  if (range->Start() == from) {
    Spill(range);
    return;
  }

  LiveRange* tail = range->SplitAt(from);
  Spill(tail);
}

void FlowGraphAllocator::AllocateSpillSlotFor(LiveRange* range) {
  ASSERT(range->spill_slot().IsInvalid());

  // Compute range start and end.
  LiveRange* last_sibling = range;
  while (last_sibling->next_sibling() != NULL) {
    last_sibling = last_sibling->next_sibling();
  }

  const intptr_t start = range->Start();
  const intptr_t end = last_sibling->End();

  // During fpu register allocation spill slot indices are computed in terms of
  // double (64bit) stack slots. We treat quad stack slot (128bit) as a
  // consecutive pair of two double spill slots.
  // Special care is taken to never allocate the same index to both
  // double and quad spill slots as it complicates disambiguation during
  // parallel move resolution.
  const bool need_quad = (register_kind_ == Location::kFpuRegister) &&
                         ((range->representation() == kUnboxedFloat32x4) ||
                          (range->representation() == kUnboxedInt32x4) ||
                          (range->representation() == kUnboxedFloat64x2));
  const bool need_untagged = (register_kind_ == Location::kRegister) &&
                             ((range->representation() == kUntagged));

  // Search for a free spill slot among allocated: the value in it should be
  // dead and its type should match (e.g. it should not be a part of the quad if
  // we are allocating normal double slot).
  // For CPU registers we need to take reserved slots for try-catch into
  // account.
  intptr_t idx = register_kind_ == Location::kRegister
                     ? flow_graph_.graph_entry()->fixed_slot_count()
                     : 0;
  for (; idx < spill_slots_.length(); idx++) {
    if ((need_quad == quad_spill_slots_[idx]) &&
        (need_untagged == untagged_spill_slots_[idx]) &&
        (spill_slots_[idx] <= start)) {
      break;
    }
  }

  while (idx > spill_slots_.length()) {
    spill_slots_.Add(kMaxPosition);
    quad_spill_slots_.Add(false);
    untagged_spill_slots_.Add(false);
  }

  if (idx == spill_slots_.length()) {
    idx = spill_slots_.length();

    // No free spill slot found. Allocate a new one.
    spill_slots_.Add(0);
    quad_spill_slots_.Add(need_quad);
    untagged_spill_slots_.Add(need_untagged);
    if (need_quad) {  // Allocate two double stack slots if we need quad slot.
      spill_slots_.Add(0);
      quad_spill_slots_.Add(need_quad);
      untagged_spill_slots_.Add(need_untagged);
    }
  }

  // Set spill slot expiration boundary to the live range's end.
  spill_slots_[idx] = end;
  if (need_quad) {
    ASSERT(quad_spill_slots_[idx] && quad_spill_slots_[idx + 1]);
    idx++;  // Use the higher index it corresponds to the lower stack address.
    spill_slots_[idx] = end;
  } else {
    ASSERT(!quad_spill_slots_[idx]);
  }

  // Assign spill slot to the range.
  if (register_kind_ == Location::kRegister) {
    const intptr_t slot_index =
        compiler::target::frame_layout.FrameSlotForVariableIndex(-idx);
    range->set_spill_slot(Location::StackSlot(slot_index, FPREG));
  } else {
    // We use the index of the slot with the lowest address as an index for the
    // FPU register spill slot. In terms of indexes this relation is inverted:
    // so we have to take the highest index.
    const intptr_t slot_idx =
        compiler::target::frame_layout.FrameSlotForVariableIndex(
            -(cpu_spill_slot_count_ + idx * kDoubleSpillFactor +
              (kDoubleSpillFactor - 1)));

    Location location;
    if ((range->representation() == kUnboxedFloat32x4) ||
        (range->representation() == kUnboxedInt32x4) ||
        (range->representation() == kUnboxedFloat64x2)) {
      ASSERT(need_quad);
      location = Location::QuadStackSlot(slot_idx, FPREG);
    } else {
      ASSERT(range->representation() == kUnboxedFloat ||
             range->representation() == kUnboxedDouble);
      location = Location::DoubleStackSlot(slot_idx, FPREG);
    }
    range->set_spill_slot(location);
  }

  spilled_.Add(range);
}

void FlowGraphAllocator::MarkAsObjectAtSafepoints(LiveRange* range) {
  Location spill_slot = range->spill_slot();
  intptr_t stack_index = spill_slot.stack_index();
  if (spill_slot.base_reg() == FPREG) {
    stack_index = -compiler::target::frame_layout.VariableIndexForFrameSlot(
        spill_slot.stack_index());
  }
  ASSERT(stack_index >= 0);
  while (range != NULL) {
    for (SafepointPosition* safepoint = range->first_safepoint();
         safepoint != NULL; safepoint = safepoint->next()) {
      // Mark the stack slot as having an object.
      safepoint->locs()->SetStackBit(stack_index);
    }
    range = range->next_sibling();
  }
}

void FlowGraphAllocator::Spill(LiveRange* range) {
  LiveRange* parent = GetLiveRange(range->vreg());
  if (parent->spill_slot().IsInvalid()) {
    AllocateSpillSlotFor(parent);
    if (range->representation() == kTagged) {
      MarkAsObjectAtSafepoints(parent);
    }
  }
  range->set_assigned_location(parent->spill_slot());
  ConvertAllUses(range);
}

void FlowGraphAllocator::AllocateSpillSlotForSuspendState() {
  if (flow_graph_.parsed_function().suspend_state_var() == nullptr) {
    return;
  }

  spill_slots_.Add(kMaxPosition);
  quad_spill_slots_.Add(false);
  untagged_spill_slots_.Add(false);

#if defined(DEBUG)
  const intptr_t stack_index =
      -compiler::target::frame_layout.VariableIndexForFrameSlot(
          compiler::target::frame_layout.FrameSlotForVariable(
              flow_graph_.parsed_function().suspend_state_var()));
  ASSERT(stack_index == spill_slots_.length() - 1);
#endif
}

void FlowGraphAllocator::UpdateStackmapsForSuspendState() {
  if (flow_graph_.parsed_function().suspend_state_var() == nullptr) {
    return;
  }

  const intptr_t stack_index =
      -compiler::target::frame_layout.VariableIndexForFrameSlot(
          compiler::target::frame_layout.FrameSlotForVariable(
              flow_graph_.parsed_function().suspend_state_var()));
  ASSERT(stack_index >= 0);

  for (intptr_t i = 0, n = safepoints_.length(); i < n; ++i) {
    Instruction* safepoint_instr = safepoints_[i];
    safepoint_instr->locs()->SetStackBit(stack_index);
  }
}

intptr_t FlowGraphAllocator::FirstIntersectionWithAllocated(
    intptr_t reg,
    LiveRange* unallocated) {
  intptr_t intersection = kMaxPosition;
  for (intptr_t i = 0; i < registers_[reg]->length(); i++) {
    LiveRange* allocated = (*registers_[reg])[i];
    if (allocated == NULL) continue;

    UseInterval* allocated_head =
        allocated->finger()->first_pending_use_interval();
    if (allocated_head->start() >= intersection) continue;

    const intptr_t pos = FirstIntersection(
        unallocated->finger()->first_pending_use_interval(), allocated_head);
    if (pos < intersection) intersection = pos;
  }
  if (intersection == kMaxPosition) {
    return intersection;
  } else {
    return ToPreceedingMovePosition(intersection);
  }
}

void ReachingDefs::AddPhi(PhiInstr* phi) {
  if (phi->reaching_defs() == NULL) {
    Zone* zone = flow_graph_.zone();
    phi->set_reaching_defs(
        new (zone) BitVector(zone, flow_graph_.max_virtual_register_number()));

    // Compute initial set reaching defs set.
    bool depends_on_phi = false;
    for (intptr_t i = 0; i < phi->InputCount(); i++) {
      Definition* input = phi->InputAt(i)->definition();
      if (input->IsPhi()) {
        depends_on_phi = true;
      }
      phi->reaching_defs()->Add(input->ssa_temp_index());
      if (phi->HasPairRepresentation()) {
        phi->reaching_defs()->Add(ToSecondPairVreg(input->ssa_temp_index()));
      }
    }

    // If this phi depends on another phi then we need fix point iteration.
    if (depends_on_phi) phis_.Add(phi);
  }
}

void ReachingDefs::Compute() {
  // Transitively collect all phis that are used by the given phi.
  for (intptr_t i = 0; i < phis_.length(); i++) {
    PhiInstr* phi = phis_[i];

    // Add all phis that affect this phi to the list.
    for (intptr_t i = 0; i < phi->InputCount(); i++) {
      PhiInstr* input_phi = phi->InputAt(i)->definition()->AsPhi();
      if (input_phi != NULL) {
        AddPhi(input_phi);
      }
    }
  }

  // Propagate values until fix point is reached.
  bool changed;
  do {
    changed = false;
    for (intptr_t i = 0; i < phis_.length(); i++) {
      PhiInstr* phi = phis_[i];
      for (intptr_t i = 0; i < phi->InputCount(); i++) {
        PhiInstr* input_phi = phi->InputAt(i)->definition()->AsPhi();
        if (input_phi != NULL) {
          if (phi->reaching_defs()->AddAll(input_phi->reaching_defs())) {
            changed = true;
          }
        }
      }
    }
  } while (changed);

  phis_.Clear();
}

BitVector* ReachingDefs::Get(PhiInstr* phi) {
  if (phi->reaching_defs() == NULL) {
    ASSERT(phis_.is_empty());
    AddPhi(phi);
    Compute();
  }
  return phi->reaching_defs();
}

bool FlowGraphAllocator::AllocateFreeRegister(LiveRange* unallocated) {
  intptr_t candidate = kNoRegister;
  intptr_t free_until = 0;

  // If hint is available try hint first.
  // TODO(vegorov): ensure that phis are hinted on the back edge.
  Location hint = unallocated->finger()->FirstHint();
  if (hint.IsMachineRegister()) {
    if (!blocked_registers_[hint.register_code()]) {
      free_until =
          FirstIntersectionWithAllocated(hint.register_code(), unallocated);
      candidate = hint.register_code();
    }

    TRACE_ALLOC(THR_Print("found hint %s for v%" Pd ": free until %" Pd "\n",
                          hint.Name(), unallocated->vreg(), free_until));
  } else {
    for (intptr_t i = 0; i < NumberOfRegisters(); ++i) {
      intptr_t reg = (i + kRegisterAllocationBias) % NumberOfRegisters();
      if (!blocked_registers_[reg] && (registers_[reg]->length() == 0)) {
        candidate = reg;
        free_until = kMaxPosition;
        break;
      }
    }
  }

  ASSERT(0 <= kMaxPosition);
  if (free_until != kMaxPosition) {
    for (intptr_t i = 0; i < NumberOfRegisters(); ++i) {
      intptr_t reg = (i + kRegisterAllocationBias) % NumberOfRegisters();
      if (blocked_registers_[reg] || (reg == candidate)) continue;
      const intptr_t intersection =
          FirstIntersectionWithAllocated(reg, unallocated);
      if (intersection > free_until) {
        candidate = reg;
        free_until = intersection;
        if (free_until == kMaxPosition) break;
      }
    }
  }

  // All registers are blocked by active ranges.
  if (free_until <= unallocated->Start()) return false;

  // We have a very good candidate (either hinted to us or completely free).
  // If we are in a loop try to reduce number of moves on the back edge by
  // searching for a candidate that does not interfere with phis on the back
  // edge.
  LoopInfo* loop_info = BlockEntryAt(unallocated->Start())->loop_info();
  if ((unallocated->vreg() >= 0) && (loop_info != nullptr) &&
      (free_until >= extra_loop_info_[loop_info->id()]->end) &&
      extra_loop_info_[loop_info->id()]->backedge_interference->Contains(
          unallocated->vreg())) {
    GrowableArray<bool> used_on_backedge(number_of_registers_);
    for (intptr_t i = 0; i < number_of_registers_; i++) {
      used_on_backedge.Add(false);
    }

    for (PhiIterator it(loop_info->header()->AsJoinEntry()); !it.Done();
         it.Advance()) {
      PhiInstr* phi = it.Current();
      ASSERT(phi->is_alive());
      const intptr_t phi_vreg = phi->ssa_temp_index();
      LiveRange* range = GetLiveRange(phi_vreg);
      if (range->assigned_location().kind() == register_kind_) {
        const intptr_t reg = range->assigned_location().register_code();
        if (!reaching_defs_.Get(phi)->Contains(unallocated->vreg())) {
          used_on_backedge[reg] = true;
        }
      }
      if (phi->HasPairRepresentation()) {
        const intptr_t second_phi_vreg = ToSecondPairVreg(phi_vreg);
        LiveRange* second_range = GetLiveRange(second_phi_vreg);
        if (second_range->assigned_location().kind() == register_kind_) {
          const intptr_t reg =
              second_range->assigned_location().register_code();
          if (!reaching_defs_.Get(phi)->Contains(unallocated->vreg())) {
            used_on_backedge[reg] = true;
          }
        }
      }
    }

    if (used_on_backedge[candidate]) {
      TRACE_ALLOC(THR_Print("considering %s for v%" Pd
                            ": has interference on the back edge"
                            " {loop [%" Pd ", %" Pd ")}\n",
                            MakeRegisterLocation(candidate).Name(),
                            unallocated->vreg(),
                            extra_loop_info_[loop_info->id()]->start,
                            extra_loop_info_[loop_info->id()]->end));
      for (intptr_t i = 0; i < NumberOfRegisters(); ++i) {
        intptr_t reg = (i + kRegisterAllocationBias) % NumberOfRegisters();
        if (blocked_registers_[reg] || (reg == candidate) ||
            used_on_backedge[reg]) {
          continue;
        }

        const intptr_t intersection =
            FirstIntersectionWithAllocated(reg, unallocated);
        if (intersection >= free_until) {
          candidate = reg;
          free_until = intersection;
          TRACE_ALLOC(THR_Print(
              "found %s for v%" Pd " with no interference on the back edge\n",
              MakeRegisterLocation(candidate).Name(), candidate));
          break;
        }
      }
    }
  }

  if (free_until != kMaxPosition) {
    // There was an intersection. Split unallocated.
    TRACE_ALLOC(THR_Print("  splitting at %" Pd "\n", free_until));
    LiveRange* tail = unallocated->SplitAt(free_until);
    AddToUnallocated(tail);

    // If unallocated represents a constant value and does not have
    // any uses then avoid using a register for it.
    if (unallocated->first_use() == NULL) {
      if (unallocated->vreg() >= 0) {
        LiveRange* parent = GetLiveRange(unallocated->vreg());
        if (parent->spill_slot().IsConstant()) {
          Spill(unallocated);
          return true;
        }
      }
    }
  }

  TRACE_ALLOC(THR_Print("  assigning free register "));
  TRACE_ALLOC(MakeRegisterLocation(candidate).Print());
  TRACE_ALLOC(THR_Print(" to v%" Pd "\n", unallocated->vreg()));

  registers_[candidate]->Add(unallocated);
  unallocated->set_assigned_location(MakeRegisterLocation(candidate));
  return true;
}

bool FlowGraphAllocator::RangeHasOnlyUnconstrainedUsesInLoop(LiveRange* range,
                                                             intptr_t loop_id) {
  if (range->vreg() >= 0) {
    LiveRange* parent = GetLiveRange(range->vreg());
    return parent->HasOnlyUnconstrainedUsesInLoop(loop_id);
  }
  return false;
}

bool FlowGraphAllocator::IsCheapToEvictRegisterInLoop(LoopInfo* loop_info,
                                                      intptr_t reg) {
  const intptr_t loop_start = extra_loop_info_[loop_info->id()]->start;
  const intptr_t loop_end = extra_loop_info_[loop_info->id()]->end;

  for (intptr_t i = 0; i < registers_[reg]->length(); i++) {
    LiveRange* allocated = (*registers_[reg])[i];
    UseInterval* interval = allocated->finger()->first_pending_use_interval();
    if (interval->Contains(loop_start)) {
      if (!RangeHasOnlyUnconstrainedUsesInLoop(allocated, loop_info->id())) {
        return false;
      }
    } else if (interval->start() < loop_end) {
      return false;
    }
  }

  return true;
}

bool FlowGraphAllocator::HasCheapEvictionCandidate(LiveRange* phi_range) {
  ASSERT(phi_range->is_loop_phi());

  BlockEntryInstr* header = BlockEntryAt(phi_range->Start());

  ASSERT(header->IsLoopHeader());
  ASSERT(phi_range->Start() == header->start_pos());

  for (intptr_t reg = 0; reg < NumberOfRegisters(); ++reg) {
    if (blocked_registers_[reg]) continue;
    if (IsCheapToEvictRegisterInLoop(header->loop_info(), reg)) {
      return true;
    }
  }

  return false;
}

void FlowGraphAllocator::AllocateAnyRegister(LiveRange* unallocated) {
  // If a loop phi has no register uses we might still want to allocate it
  // to the register to reduce amount of memory moves on the back edge.
  // This is possible if there is a register blocked by a range that can be
  // cheaply evicted i.e. it has no register beneficial uses inside the
  // loop.
  UsePosition* register_use =
      unallocated->finger()->FirstRegisterUse(unallocated->Start());
  if ((register_use == NULL) &&
      !(unallocated->is_loop_phi() && HasCheapEvictionCandidate(unallocated))) {
    Spill(unallocated);
    return;
  }

  intptr_t candidate = kNoRegister;
  intptr_t free_until = 0;
  intptr_t blocked_at = kMaxPosition;

  for (int i = 0; i < NumberOfRegisters(); ++i) {
    int reg = (i + kRegisterAllocationBias) % NumberOfRegisters();
    if (blocked_registers_[reg]) continue;
    if (UpdateFreeUntil(reg, unallocated, &free_until, &blocked_at)) {
      candidate = reg;
    }
  }

  const intptr_t register_use_pos =
      (register_use != NULL) ? register_use->pos() : unallocated->Start();
  if (free_until < register_use_pos) {
    // Can't acquire free register. Spill until we really need one.
    RELEASE_ASSERT(unallocated->Start() <
                   ToPreceedingMovePosition(register_use_pos));
    SpillBetween(unallocated, unallocated->Start(), register_use->pos());
    return;
  }

  ASSERT(candidate != kNoRegister);

  TRACE_ALLOC(THR_Print("assigning blocked register "));
  TRACE_ALLOC(MakeRegisterLocation(candidate).Print());
  TRACE_ALLOC(THR_Print(" to live range v%" Pd " until %" Pd "\n",
                        unallocated->vreg(), blocked_at));

  if (blocked_at < unallocated->End()) {
    // Register is blocked before the end of the live range.  Split the range
    // at latest at blocked_at position.
    LiveRange* tail =
        SplitBetween(unallocated, unallocated->Start(), blocked_at + 1);
    AddToUnallocated(tail);
  }

  AssignNonFreeRegister(unallocated, candidate);
}

bool FlowGraphAllocator::UpdateFreeUntil(intptr_t reg,
                                         LiveRange* unallocated,
                                         intptr_t* cur_free_until,
                                         intptr_t* cur_blocked_at) {
  intptr_t free_until = kMaxPosition;
  intptr_t blocked_at = kMaxPosition;
  const intptr_t start = unallocated->Start();

  for (intptr_t i = 0; i < registers_[reg]->length(); i++) {
    LiveRange* allocated = (*registers_[reg])[i];

    UseInterval* first_pending_use_interval =
        allocated->finger()->first_pending_use_interval();
    if (first_pending_use_interval->Contains(start)) {
      // This is an active interval.
      if (allocated->vreg() < 0) {
        // This register blocked by an interval that
        // can't be spilled.
        return false;
      }

      UsePosition* use = allocated->finger()->FirstInterferingUse(start);
      if ((use != NULL) && ((use->pos() - start) <= 1)) {
        // This register is blocked by interval that is used
        // as register in the current instruction and can't
        // be spilled.
        return false;
      }

      const intptr_t use_pos = (use != NULL) ? use->pos() : allocated->End();

      if (use_pos < free_until) free_until = use_pos;
    } else {
      // This is inactive interval.
      const intptr_t intersection = FirstIntersection(
          first_pending_use_interval, unallocated->first_use_interval());
      if (intersection != kMaxPosition) {
        if (intersection < free_until) free_until = intersection;
        if (allocated->vreg() == kNoVirtualRegister) blocked_at = intersection;
      }
    }

    if (free_until <= *cur_free_until) {
      return false;
    }
  }

  ASSERT(free_until > *cur_free_until);
  *cur_free_until = free_until;
  *cur_blocked_at = blocked_at;
  return true;
}

void FlowGraphAllocator::RemoveEvicted(intptr_t reg, intptr_t first_evicted) {
  intptr_t to = first_evicted;
  intptr_t from = first_evicted + 1;
  while (from < registers_[reg]->length()) {
    LiveRange* allocated = (*registers_[reg])[from++];
    if (allocated != NULL) (*registers_[reg])[to++] = allocated;
  }
  registers_[reg]->TruncateTo(to);
}

void FlowGraphAllocator::AssignNonFreeRegister(LiveRange* unallocated,
                                               intptr_t reg) {
  intptr_t first_evicted = -1;
  for (intptr_t i = registers_[reg]->length() - 1; i >= 0; i--) {
    LiveRange* allocated = (*registers_[reg])[i];
    if (allocated->vreg() < 0) continue;  // Can't be evicted.
    if (EvictIntersection(allocated, unallocated)) {
      // If allocated was not spilled convert all pending uses.
      if (allocated->assigned_location().IsMachineRegister()) {
        ASSERT(allocated->End() <= unallocated->Start());
        ConvertAllUses(allocated);
      }
      (*registers_[reg])[i] = NULL;
      first_evicted = i;
    }
  }

  // Remove evicted ranges from the array.
  if (first_evicted != -1) RemoveEvicted(reg, first_evicted);

  registers_[reg]->Add(unallocated);
  unallocated->set_assigned_location(MakeRegisterLocation(reg));
}

bool FlowGraphAllocator::EvictIntersection(LiveRange* allocated,
                                           LiveRange* unallocated) {
  UseInterval* first_unallocated =
      unallocated->finger()->first_pending_use_interval();
  const intptr_t intersection = FirstIntersection(
      allocated->finger()->first_pending_use_interval(), first_unallocated);
  if (intersection == kMaxPosition) return false;

  const intptr_t spill_position =
      ToPreceedingMovePosition(first_unallocated->start());
  UsePosition* use = allocated->finger()->FirstInterferingUse(spill_position);
  TRACE_ALLOCF("| evict intersection of v%" Pd " and v%" Pd
               " (first use at %" Pd ", first intersection at %" Pd ")\n",
               unallocated->vreg(), allocated->vreg(),
               use != nullptr ? use->pos() : -1, intersection);
  if (use == NULL) {
    // No register uses after this point.
    SpillAfter(allocated, spill_position);
  } else {
    const intptr_t restore_position = use->pos();

    SpillBetween(allocated, spill_position, restore_position);
  }

  return true;
}

MoveOperands* FlowGraphAllocator::AddMoveAt(intptr_t pos,
                                            Location to,
                                            Location from) {
  // No parallel moves at the graph entry.
  RELEASE_ASSERT(pos > 0);
  RELEASE_ASSERT(IsParallelMovePosition(pos));

  Instruction* instr = InstructionAt(pos + 1);
  return CreateParallelMoveBefore(instr)->AddMove(to, from);
}

void FlowGraphAllocator::ConvertUseTo(UsePosition* use, Location loc) {
  ASSERT(!loc.IsPairLocation());
  ASSERT(use->location_slot() != NULL);
  Location* slot = use->location_slot();
  TRACE_ALLOC(THR_Print("  use at %" Pd " %s (was %s)\n", use->pos(),
                        loc.ToCString(), slot->ToCString()));
  ASSERT(slot->IsUnallocated());
  *slot = loc;
}

void FlowGraphAllocator::ConvertAllUses(LiveRange* range) {
  if (range->vreg() == kNoVirtualRegister) return;

  const Location loc = range->assigned_location();
  ASSERT(!loc.IsInvalid());

  TRACE_ALLOC(THR_Print("range [%" Pd ", %" Pd ") "
                        "for v%" Pd " has been allocated to ",
                        range->Start(), range->End(), range->vreg()));
  TRACE_ALLOC(loc.Print());
  TRACE_ALLOC(THR_Print(":\n"));

  for (UsePosition* use = range->first_use(); use != NULL; use = use->next()) {
    ConvertUseTo(use, loc);
  }

  // Add live registers at all safepoints for instructions with slow-path
  // code.
  if (loc.IsMachineRegister()) {
    for (SafepointPosition* safepoint = range->first_safepoint();
         safepoint != NULL; safepoint = safepoint->next()) {
      if (!safepoint->locs()->always_calls()) {
        ASSERT(safepoint->locs()->can_call());
        safepoint->locs()->live_registers()->Add(loc, range->representation());
      }
    }
  }
}

void FlowGraphAllocator::AdvanceActiveIntervals(const intptr_t start) {
  for (intptr_t i = 0; i < NumberOfRegisters(); ++i) {
    intptr_t reg = (i + kRegisterAllocationBias) % NumberOfRegisters();
    if (registers_[reg]->is_empty()) continue;

    intptr_t first_evicted = -1;
    for (intptr_t i = registers_[reg]->length() - 1; i >= 0; i--) {
      LiveRange* range = (*registers_[reg])[i];
      if (range->finger()->Advance(start)) {
        ConvertAllUses(range);
        (*registers_[reg])[i] = NULL;
        first_evicted = i;
      }
    }

    if (first_evicted != -1) RemoveEvicted(reg, first_evicted);
  }
}

bool LiveRange::Contains(intptr_t pos) const {
  if (!CanCover(pos)) return false;

  for (UseInterval* interval = first_use_interval_; interval != NULL;
       interval = interval->next()) {
    if (interval->Contains(pos)) {
      return true;
    }
  }

  return false;
}

void FlowGraphAllocator::AssignSafepoints(Definition* defn, LiveRange* range) {
  for (intptr_t i = safepoints_.length() - 1; i >= 0; i--) {
    Instruction* safepoint_instr = safepoints_[i];
    if (safepoint_instr == defn) {
      // The value is not live until after the definition is fully executed,
      // don't assign the safepoint inside the definition itself to
      // definition's liverange.
      continue;
    }

    const intptr_t pos = GetLifetimePosition(safepoint_instr);
    if (range->End() <= pos) break;

    if (range->Contains(pos)) {
      range->AddSafepoint(pos, safepoint_instr->locs());
    }
  }
}

static inline bool ShouldBeAllocatedBefore(LiveRange* a, LiveRange* b) {
  // TODO(vegorov): consider first hint position when ordering live ranges.
  return a->Start() <= b->Start();
}

static void AddToSortedListOfRanges(GrowableArray<LiveRange*>* list,
                                    LiveRange* range) {
  range->finger()->Initialize(range);

  if (list->is_empty()) {
    list->Add(range);
    return;
  }

  for (intptr_t i = list->length() - 1; i >= 0; i--) {
    if (ShouldBeAllocatedBefore(range, (*list)[i])) {
      list->InsertAt(i + 1, range);
      return;
    }
  }
  list->InsertAt(0, range);
}

void FlowGraphAllocator::AddToUnallocated(LiveRange* range) {
  AddToSortedListOfRanges(&unallocated_, range);
}

void FlowGraphAllocator::CompleteRange(LiveRange* range, Location::Kind kind) {
  switch (kind) {
    case Location::kRegister:
      AddToSortedListOfRanges(&unallocated_cpu_, range);
      break;

    case Location::kFpuRegister:
      AddToSortedListOfRanges(&unallocated_xmm_, range);
      break;

    default:
      UNREACHABLE();
  }
}

#if defined(DEBUG)
bool FlowGraphAllocator::UnallocatedIsSorted() {
  for (intptr_t i = unallocated_.length() - 1; i >= 1; i--) {
    LiveRange* a = unallocated_[i];
    LiveRange* b = unallocated_[i - 1];
    if (!ShouldBeAllocatedBefore(a, b)) {
      UNREACHABLE();
      return false;
    }
  }
  return true;
}
#endif

void FlowGraphAllocator::PrepareForAllocation(
    Location::Kind register_kind,
    intptr_t number_of_registers,
    const GrowableArray<LiveRange*>& unallocated,
    LiveRange** blocking_ranges,
    bool* blocked_registers) {
  register_kind_ = register_kind;
  number_of_registers_ = number_of_registers;

  blocked_registers_.Clear();
  registers_.Clear();
  for (intptr_t i = 0; i < number_of_registers_; i++) {
    blocked_registers_.Add(false);
    registers_.Add(new ZoneGrowableArray<LiveRange*>);
  }
  ASSERT(unallocated_.is_empty());
  unallocated_.AddArray(unallocated);

  for (intptr_t i = 0; i < NumberOfRegisters(); ++i) {
    intptr_t reg = (i + kRegisterAllocationBias) % NumberOfRegisters();
    blocked_registers_[reg] = blocked_registers[reg];
    ASSERT(registers_[reg]->is_empty());

    LiveRange* range = blocking_ranges[reg];
    if (range != NULL) {
      range->finger()->Initialize(range);
      registers_[reg]->Add(range);
    }
  }
}

#if defined(INCLUDE_LINEAR_SCAN_TRACING_CODE)
static intptr_t SafeStart(UseInterval* i) {
  return i != nullptr ? i->start() : -1;
}

static intptr_t SafeEnd(UseInterval* i) {
  return i != nullptr ? i->end() : -1;
}
#endif

void FlowGraphAllocator::AllocateUnallocatedRanges() {
#if defined(DEBUG)
  ASSERT(UnallocatedIsSorted());
#endif

  while (!unallocated_.is_empty()) {
    LiveRange* range = unallocated_.RemoveLast();
    const intptr_t start = range->Start();
    TRACE_ALLOC(THR_Print("Processing live range for v%" Pd " "
                          "starting at %" Pd "\n",
                          range->vreg(), start));

    TRACE_ALLOC({
      for (intptr_t reg = 0; reg < number_of_registers_; reg++) {
        bool header_printed = false;
        for (LiveRange* allocated : *registers_[reg]) {
          if (allocated == nullptr) continue;
          if (!header_printed) {
            THR_Print("Register %s\n", MakeRegisterLocation(reg).ToCString());
            header_printed = true;
          }
          THR_Print(
              "    v%" Pd " [%" Pd ", %" Pd ") pending [%" Pd ", %" Pd ")\n",
              allocated->vreg(), allocated->Start(), allocated->End(),
              SafeStart(allocated->finger()->first_pending_use_interval()),
              SafeEnd(allocated->finger()->first_pending_use_interval()));
        }
      }
    });

    // TODO(vegorov): eagerly spill liveranges without register uses.
    AdvanceActiveIntervals(start);

    if (!AllocateFreeRegister(range)) {
      if (intrinsic_mode_) {
        // No spilling when compiling intrinsics.
        // TODO(fschneider): Handle spilling in intrinsics. For now, the
        // IR has to be built so that there are enough free registers.
        UNREACHABLE();
      }
      AllocateAnyRegister(range);
    }
  }

  // All allocation decisions were done.
  ASSERT(unallocated_.is_empty());

  // Finish allocation.
  AdvanceActiveIntervals(kMaxPosition);
  TRACE_ALLOC(THR_Print("Allocation completed\n"));
}

bool FlowGraphAllocator::TargetLocationIsSpillSlot(LiveRange* range,
                                                   Location target) {
  return GetLiveRange(range->vreg())->spill_slot().Equals(target);
}

static LiveRange* FindCover(LiveRange* parent, intptr_t pos) {
  for (LiveRange* range = parent; range != nullptr;
       range = range->next_sibling()) {
    if (range->CanCover(pos)) {
      return range;
    }
  }
  OS::PrintErr("Failed to find cover for %" Pd " in the range for v%" Pd "\n",
               pos, parent->vreg());
  for (LiveRange* range = parent; range != nullptr;
       range = range->next_sibling()) {
    OS::PrintErr("  [%" Pd ", %" Pd ")\n", range->Start(), range->End());
  }
  UNREACHABLE();
  return nullptr;
}

static bool AreLocationsAllTheSame(const GrowableArray<Location>& locs) {
  for (intptr_t j = 1; j < locs.length(); j++) {
    if (!locs[j].Equals(locs[0])) {
      return false;
    }
  }
  return true;
}

// Emit move on the edge from |pred| to |succ|.
static void EmitMoveOnEdge(BlockEntryInstr* succ,
                           BlockEntryInstr* pred,
                           const MoveOperands& pmove) {
  Instruction* last = pred->last_instruction();
  if ((last->SuccessorCount() == 1) && !pred->IsGraphEntry()) {
    ASSERT(last->IsGoto());
    CreateParallelMoveBefore(last)->AddMove(pmove.dest(), pmove.src());
  } else {
    auto parallel_move = GetBlockEntryMove(succ);
    parallel_move->AddMove(pmove.dest(), pmove.src());
  }
}

void FlowGraphAllocator::ResolveControlFlow() {
  // Resolve linear control flow between touching split siblings
  // inside basic blocks.
  const auto can_eagery_resolve_at = [&](intptr_t pos,
                                         BlockEntryInstr** on_block) {
    if (pos == 0) {
      return true;
    }

    if (!IsParallelMovePosition(pos)) {
      return true;
    }

    auto block = BlockEntryAt(pos);
    if (block == BlockEntryAt(pos - 1)) {
      return true;
    }

    *on_block = block;
    return block->IsBlockEntryWithInitialDefs();
  };

  for (intptr_t vreg = 0; vreg < live_ranges_.length(); vreg++) {
    LiveRange* range = live_ranges_[vreg];
    if (range == NULL) continue;

    while (range->next_sibling() != nullptr) {
      LiveRange* sibling = range->next_sibling();
      TRACE_ALLOC(THR_Print("connecting [%" Pd ", %" Pd ") [", range->Start(),
                            range->End()));
      TRACE_ALLOC(range->assigned_location().Print());
      TRACE_ALLOC(THR_Print("] to [%" Pd ", %" Pd ") [", sibling->Start(),
                            sibling->End()));
      TRACE_ALLOC(sibling->assigned_location().Print());
      TRACE_ALLOC(THR_Print("]\n"));
      BlockEntryInstr* block = nullptr;
      if ((range->End() == sibling->Start()) &&
          !TargetLocationIsSpillSlot(range, sibling->assigned_location()) &&
          !range->assigned_location().Equals(sibling->assigned_location()) &&
          can_eagery_resolve_at(range->End(), &block)) {
        if (block != nullptr) {
          ASSERT(block->start_pos() == sibling->Start());
          GetBlockEntryMove(block)->AddMove(sibling->assigned_location(),
                                            range->assigned_location());
        } else {
          AddMoveAt(sibling->Start(), sibling->assigned_location(),
                    range->assigned_location());
        }
      }
      range = sibling;
    }
  }

  // Resolve non-linear control flow across branches.
  // At joins we attempt to sink duplicated moves from predecessors into join
  // itself as long as their source is not blocked by other moves.
  // Moves which are candidates to sinking are collected in the |pending|
  // array and we later compute which one of them we can emit (|can_emit|)
  // at the join itself.
  GrowableArray<Location> src_locs(2);
  GrowableArray<MoveOperands> pending(10);
  BitVector* can_emit = new BitVector(flow_graph_.zone(), 10);
  for (intptr_t i = 1; i < block_order_.length(); i++) {
    BlockEntryInstr* block = block_order_[i];

    // Process move to destination.
    const auto process_move = [&](LiveRange* range, Location dst) {
      src_locs.Clear();
      for (intptr_t j = 0; j < block->PredecessorCount(); j++) {
        BlockEntryInstr* pred = block->PredecessorAt(j);
        LiveRange* src_cover = FindCover(range, pred->end_pos() - 1);
        Location src = src_cover->assigned_location();
        src_locs.Add(src);

        TRACE_ALLOC(THR_Print("| incoming value in %s on exit from B%" Pd
                              " covered by [%" Pd ", %" Pd ")\n",
                              src.ToCString(), pred->block_id(),
                              src_cover->Start(), src_cover->End()));
      }

      // Check if all source locations are the same for the range. Then
      // we can try to emit a single move at the destination if we can
      // guarantee that source location is available on all incoming edges.
      // (i.e. it is not destroyed by some other move).
      if ((src_locs.length() > 1) && AreLocationsAllTheSame(src_locs)) {
        if (!dst.Equals(src_locs[0])) {
          // We have a non-redundant move which potentially can be performed
          // at the start of block, however we will only be able to check
          // whether or not source location is alive on all incoming edges
          // only when we finish processing all live-in values.
          TRACE_ALLOC(THR_Print(
              "| create pending move %s <- %s (at B%" Pd ")\n", dst.ToCString(),
              src_locs[0].ToCString(), block->block_id()));
          pending.Add(MoveOperands(dst, src_locs[0]));
        }

        // Next incoming value.
        return;
      }

      for (intptr_t j = 0; j < block->PredecessorCount(); j++) {
        if (dst.Equals(src_locs[j])) {
          // Redundant move.
          continue;
        }

        EmitMoveOnEdge(block, block->PredecessorAt(j), {dst, src_locs[j]});
      }
    };

    BitVector* live = liveness_.GetLiveInSet(block);
    for (BitVector::Iterator it(live); !it.Done(); it.Advance()) {
      LiveRange* range = GetLiveRange(it.Current());
      if (range->next_sibling() == nullptr) {
        // Nothing to connect. The whole range was allocated to the same
        // location.
        TRACE_ALLOC(
            THR_Print("range v%" Pd " has no siblings\n", range->vreg()));
        continue;
      }

      LiveRange* dst_cover = FindCover(range, block->start_pos());
      Location dst = dst_cover->assigned_location();

      TRACE_ALLOC(THR_Print("range v%" Pd
                            " is allocated to %s on entry to B%" Pd
                            " covered by [%" Pd ", %" Pd ")\n",
                            range->vreg(), dst.ToCString(), block->block_id(),
                            dst_cover->Start(), dst_cover->End()));

      if (TargetLocationIsSpillSlot(range, dst)) {
        // Values are eagerly spilled. Spill slot already contains appropriate
        // value.
        TRACE_ALLOC(
            THR_Print("  [no resolution necessary - range is spilled]\n"));
        continue;
      }

      process_move(range, dst);
    }

    // For each pending move we need to check if it can be emitted into the
    // destination block (prerequisite for that is that predecessors should
    // not destroy the value in the Goto move).
    if (pending.length() > 0) {
      if (can_emit->length() < pending.length()) {
        can_emit = new BitVector(flow_graph_.zone(), pending.length());
      }
      can_emit->SetAll();

      // Set to |true| when we discover more blocked pending moves and
      // need to run another run through pending moves to propagate that.
      bool changed = false;

      // Process all pending moves and check if any move in the predecessor
      // blocks then by overwriting their source.
      for (intptr_t j = 0; j < pending.length(); j++) {
        Location src = pending[j].src();
        for (intptr_t p = 0; p < block->PredecessorCount(); p++) {
          BlockEntryInstr* pred = block->PredecessorAt(p);
          if (auto parallel_move = pred->last_instruction()
                                       ->AsGoto()
                                       ->previous()
                                       ->AsParallelMove()) {
            for (auto move : parallel_move->moves()) {
              if (!move->IsRedundant() && move->dest().Equals(src)) {
                can_emit->Remove(j);
                changed = true;
                break;
              }
            }
          }
        }
      }

      // Check if newly discovered blocked moves block any other pending moves.
      while (changed) {
        changed = false;
        for (intptr_t j = 0; j < pending.length(); j++) {
          if (can_emit->Contains(j)) {
            for (intptr_t k = 0; k < pending.length(); k++) {
              if (!can_emit->Contains(k) &&
                  pending[k].dest().Equals(pending[j].src())) {
                can_emit->Remove(j);
                changed = true;
                break;
              }
            }
          }
        }
      }

      // Emit pending moves either in the successor block or in predecessors
      // (if they are blocked).
      for (intptr_t j = 0; j < pending.length(); j++) {
        const auto& pending_move = pending[j];
        if (can_emit->Contains(j)) {
#if 0
          // When sinking a move from a predecessor to the successor we need to
          // rewrite all moves in the successor that expect destination to be
          // populated. In other words:
          //
          //   ParallelMove { Dst <- Src } ParallelMove { ... <- Dst }
          //
          // has to become
          //
          //   ParallelMove { ... <- Src, Dst <- Src }
          //
          /*
          for (auto move : parallel_move->moves()) {
            if (!move->IsRedundant() &&
                move->src().Equals(pending_move.dest())) {
              TRACE_ALLOC(THR_Print("| need to fix already emitted move %s <- %s (<- %s) (at B%" Pd ")\n",
                                    move->dest().ToCString(), move->src().ToCString(), pending_move.src().ToCString(), block->block_id()));
              move->set_src(pending_move.src());
            }
          }*/
#endif
          auto parallel_move = GetBlockEntryMove(block);
          parallel_move->AddMove(pending_move.dest(), pending_move.src());
          TRACE_ALLOC(THR_Print("| emit pending move %s <- %s (at B%" Pd ")\n",
                                pending_move.dest().ToCString(),
                                pending_move.src().ToCString(),
                                block->block_id()));
        } else {
          for (intptr_t p = 0; p < block->PredecessorCount(); p++) {
            EmitMoveOnEdge(block, block->PredecessorAt(p), pending_move);
          }
        }
      }

      pending.Clear();
    }
  }

  // Eagerly spill values.
  // TODO(vegorov): if value is spilled on the cold path (e.g. by the call)
  // this will cause spilling to occur on the fast path (at the definition).
  for (intptr_t i = 0; i < spilled_.length(); i++) {
    LiveRange* range = spilled_[i];
    TRACE_ALLOC(
        THR_Print("trying to emit spill for v%" Pd "\n", range->vreg()));

    if (!range->assigned_location().Equals(range->spill_slot())) {
      // We need to create a move to a spill slot.
      RELEASE_ASSERT(range->Start() > 0);
      if (IsRealInstructionPosition(range->Start())) {
        // The value is produced by the instruction.
        CreateParallelMoveAfter(InstructionAt(range->Start()))
            ->AddMove(range->spill_slot(), range->assigned_location());
      } else {
        // The value is produced by the parallel move (e.g. a phi or a value
        // produced by the fixed output), then we need to scan the parallel
        // move to find the original location.

        auto instr = InstructionAt(range->Start() + 1);
        if (auto defn = instr->AsDefinition()) {
          if (defn->ssa_temp_index() == range->vreg()) {
            // Output being same as first input results in the following
            // sequence:
            //
            //    ParallelMove R <- A
            //    R <- Op(R, ...)
            //
            // With live-range for the output value starting at the move.
            // Here we need to insert spill after the Op.
            // The value is produced by the instruction.
            CreateParallelMoveAfter(instr)->AddMove(range->spill_slot(),
                                                    range->assigned_location());
            continue;
          }
        }

        auto block = BlockEntryAt(range->Start());
        TRACE_ALLOC(THR_Print("-|> start of range %" Pd ", start of block %" Pd
                              "\n",
                              block->start_pos(), range->Start()));

        auto parallel_move = block->start_pos() == range->Start()
                                 ? GetBlockEntryMove(block)
                                 : CreateParallelMoveBefore(instr);

        ASSERT(block->start_pos() == range->Start() ||
               (parallel_move->previous()->IsDefinition() &&
                parallel_move->previous()->AsDefinition()->ssa_temp_index() ==
                    range->vreg()));

        Location original_loc = range->assigned_location();
        for (auto& move : parallel_move->moves()) {
          if (move->dest().Equals(original_loc)) {
            original_loc = move->src();
            break;
          }
        }
        parallel_move->AddMove(range->spill_slot(), original_loc);
      }
    }
  }
}

static Representation RepresentationForRange(Representation definition_rep) {
  if (definition_rep == kUnboxedInt64) {
    // kUnboxedInt64 is split into two ranges, each of which are kUntagged.
    return kUntagged;
  } else if (definition_rep == kUnboxedUint32) {
    // kUnboxedUint32 is untagged.
    return kUntagged;
  }
  return definition_rep;
}

void FlowGraphAllocator::CollectRepresentations() {
  // Constants.
  GraphEntryInstr* graph_entry = flow_graph_.graph_entry();
  auto initial_definitions = graph_entry->initial_definitions();
  for (intptr_t i = 0; i < initial_definitions->length(); ++i) {
    Definition* def = (*initial_definitions)[i];
    value_representations_[def->ssa_temp_index()] =
        RepresentationForRange(def->representation());
    if (def->HasPairRepresentation()) {
      value_representations_[ToSecondPairVreg(def->ssa_temp_index())] =
          RepresentationForRange(def->representation());
    }
  }

  for (BlockIterator it = flow_graph_.reverse_postorder_iterator(); !it.Done();
       it.Advance()) {
    BlockEntryInstr* block = it.Current();

    if (auto entry = block->AsBlockEntryWithInitialDefs()) {
      initial_definitions = entry->initial_definitions();
      for (intptr_t i = 0; i < initial_definitions->length(); ++i) {
        Definition* def = (*initial_definitions)[i];
        value_representations_[def->ssa_temp_index()] =
            RepresentationForRange(def->representation());
        if (def->HasPairRepresentation()) {
          value_representations_[ToSecondPairVreg(def->ssa_temp_index())] =
              RepresentationForRange(def->representation());
        }
      }
    } else if (auto join = block->AsJoinEntry()) {
      for (PhiIterator it(join); !it.Done(); it.Advance()) {
        PhiInstr* phi = it.Current();
        ASSERT(phi != NULL && phi->ssa_temp_index() >= 0);
        value_representations_[phi->ssa_temp_index()] =
            RepresentationForRange(phi->representation());
        if (phi->HasPairRepresentation()) {
          value_representations_[ToSecondPairVreg(phi->ssa_temp_index())] =
              RepresentationForRange(phi->representation());
        }
      }
    }

    // Normal instructions.
    for (ForwardInstructionIterator instr_it(block); !instr_it.Done();
         instr_it.Advance()) {
      Definition* def = instr_it.Current()->AsDefinition();
      if ((def != NULL) && (def->ssa_temp_index() >= 0)) {
        const intptr_t vreg = def->ssa_temp_index();
        value_representations_[vreg] =
            RepresentationForRange(def->representation());
        if (def->HasPairRepresentation()) {
          value_representations_[ToSecondPairVreg(vreg)] =
              RepresentationForRange(def->representation());
        }
      }
    }
  }
}

Location FlowGraphAllocator::ComputeParameterLocation(BlockEntryInstr* block,
                                                      ParameterInstr* param,
                                                      Register base_reg,
                                                      intptr_t pair_index) {
  ASSERT(pair_index == 0 || param->HasPairRepresentation());

  // Only function entries may have unboxed parameters, possibly making the
  // parameters size different from the number of parameters on 32-bit
  // architectures.
  const intptr_t parameters_size = block->IsFunctionEntry()
                                       ? flow_graph_.direct_parameters_size()
                                       : flow_graph_.num_direct_parameters();
  intptr_t slot_index = param->param_offset() - pair_index;
  ASSERT(slot_index >= 0);
  if (base_reg == FPREG) {
    // Slot index for the rightmost fixed parameter is -1.
    slot_index -= parameters_size;
  } else {
    // Slot index for a "frameless" parameter is reversed.
    ASSERT(base_reg == SPREG);
    ASSERT(slot_index < parameters_size);
    slot_index = parameters_size - 1 - slot_index;
  }

  if (base_reg == FPREG) {
    slot_index =
        compiler::target::frame_layout.FrameSlotForVariableIndex(-slot_index);
  } else {
    ASSERT(base_reg == SPREG);
    slot_index += compiler::target::frame_layout.last_param_from_entry_sp;
  }

  if (param->representation() == kUnboxedInt64 ||
      param->representation() == kTagged) {
    return Location::StackSlot(slot_index, base_reg);
  } else {
    ASSERT(param->representation() == kUnboxedDouble);
    return Location::DoubleStackSlot(slot_index, base_reg);
  }
}

void FlowGraphAllocator::RemoveFrameIfNotNeeded() {
  // Intrinsic functions are naturally frameless.
  if (intrinsic_mode_) {
    flow_graph_.graph_entry()->MarkFrameless();
    return;
  }

  // For now only AOT compiled code in bare instructions mode supports
  // frameless functions. Outside of bare instructions mode we need to preserve
  // caller PP - so all functions need a frame if they have their own pool which
  // is hard to determine at this stage.
  if (!FLAG_precompiled_mode) {
    return;
  }

  // Copying of parameters needs special changes to become frameless.
  // Specifically we need to rebase IL instructions which directly access frame
  // ({Load,Store}IndexedUnsafeInstr) to use SP rather than FP.
  // For now just always give such functions a frame.
  if (flow_graph_.parsed_function().function().MakesCopyOfParameters()) {
    return;
  }

  // If we have spills we need to create a frame.
  if (flow_graph_.graph_entry()->spill_slot_count() > 0) {
    return;
  }

#if defined(TARGET_ARCH_ARM64) || defined(TARGET_ARCH_ARM)
  bool has_write_barrier_call = false;
#endif
  for (auto block : flow_graph_.reverse_postorder()) {
    for (auto instruction : block->instructions()) {
      if (instruction->HasLocs() && instruction->locs()->can_call()) {
        // Function contains a call and thus needs a frame.
        return;
      }

      // Some instructions contain write barriers inside, which can call
      // a helper stub. This however is a leaf call and we can entirely
      // ignore it on ia32 and x64, because return address is always on the
      // stack. On ARM/ARM64 however return address is in LR and needs to
      // be explicitly preserved in the frame. Write barrier instruction
      // sequence has explicit support for emitting LR spill/restore
      // if necessary, however for code size purposes it does not make
      // sense to make function frameless if it contains more than 1
      // write barrier invocation.
#if defined(TARGET_ARCH_ARM64) || defined(TARGET_ARCH_ARM)
      if (auto store_field = instruction->AsStoreInstanceField()) {
        if (store_field->ShouldEmitStoreBarrier()) {
          if (has_write_barrier_call) {
            // We already have at least one write barrier call.
            // For code size purposes it is better if we have copy of
            // LR spill/restore.
            return;
          }
          has_write_barrier_call = true;
        }
      }

      if (auto store_indexed = instruction->AsStoreIndexed()) {
        if (store_indexed->ShouldEmitStoreBarrier()) {
          if (has_write_barrier_call) {
            // We already have at least one write barrier call.
            // For code size purposes it is better if we have copy of
            // LR spill/restore.
            return;
          }
          has_write_barrier_call = true;
        }
      }
#endif
    }
  }

  // Good to go. No need to setup a frame due to the call.
  auto entry = flow_graph_.graph_entry();

  entry->MarkFrameless();

  // Fix location of parameters to use SP as their base register instead of FP.
  auto fix_location_for = [&](BlockEntryInstr* block, ParameterInstr* param,
                              intptr_t vreg, intptr_t pair_index) {
    auto fp_relative =
        ComputeParameterLocation(block, param, FPREG, pair_index);
    auto sp_relative =
        ComputeParameterLocation(block, param, SPREG, pair_index);
    for (LiveRange* range = GetLiveRange(vreg); range != nullptr;
         range = range->next_sibling()) {
      if (range->assigned_location().Equals(fp_relative)) {
        range->set_assigned_location(sp_relative);
        range->set_spill_slot(sp_relative);
        for (UsePosition* use = range->first_use(); use != nullptr;
             use = use->next()) {
          ASSERT(use->location_slot()->Equals(fp_relative));
          *use->location_slot() = sp_relative;
        }
      }
    }
  };

  for (auto block : entry->successors()) {
    if (FunctionEntryInstr* entry = block->AsFunctionEntry()) {
      for (auto defn : *entry->initial_definitions()) {
        if (auto param = defn->AsParameter()) {
          const auto vreg = param->ssa_temp_index();
          fix_location_for(block, param, vreg, 0);
          if (param->HasPairRepresentation()) {
            fix_location_for(block, param, ToSecondPairVreg(vreg),
                             /*pair_index=*/1);
          }
        }
      }
    }
  }
}

void FlowGraphAllocator::AllocateRegisters() {
  CollectRepresentations();

  liveness_.Analyze();

  NumberInstructions();

  // Reserve spill slot for :suspend_state synthetic variable before
  // reserving spill slots for parameter variables.
  AllocateSpillSlotForSuspendState();

  BuildLiveRanges();

  // Update stackmaps after all safepoints are collected.
  UpdateStackmapsForSuspendState();

  if (FLAG_print_ssa_liveranges && CompilerState::ShouldTrace()) {
    const Function& function = flow_graph_.function();
    THR_Print("-- [before ssa allocator] ranges [%s] ---------\n",
              function.ToFullyQualifiedCString());
    PrintLiveRanges();
    THR_Print("----------------------------------------------\n");

    THR_Print("-- [before ssa allocator] ir [%s] -------------\n",
              function.ToFullyQualifiedCString());
    if (FLAG_support_il_printer) {
#ifndef PRODUCT
      FlowGraphPrinter printer(flow_graph_, true);
      printer.PrintBlocks();
#endif
    }
    THR_Print("----------------------------------------------\n");
  }

  PrepareForAllocation(Location::kRegister, kNumberOfCpuRegisters,
                       unallocated_cpu_, cpu_regs_, blocked_cpu_registers_);
  AllocateUnallocatedRanges();
  // GraphEntryInstr::fixed_slot_count() stack slots are reserved for catch
  // entries. When allocating a spill slot, AllocateSpillSlotFor() accounts for
  // these reserved slots and allocates spill slots on top of them.
  // However, if there are no spill slots allocated, we still need to reserve
  // slots for catch entries in the spill area.
  cpu_spill_slot_count_ = Utils::Maximum(
      spill_slots_.length(), flow_graph_.graph_entry()->fixed_slot_count());
  spill_slots_.Clear();
  quad_spill_slots_.Clear();
  untagged_spill_slots_.Clear();

  PrepareForAllocation(Location::kFpuRegister, kNumberOfFpuRegisters,
                       unallocated_xmm_, fpu_regs_, blocked_fpu_registers_);
  AllocateUnallocatedRanges();

  GraphEntryInstr* entry = block_order_[0]->AsGraphEntry();
  ASSERT(entry != NULL);
  intptr_t double_spill_slot_count = spill_slots_.length() * kDoubleSpillFactor;
  entry->set_spill_slot_count(cpu_spill_slot_count_ + double_spill_slot_count);

  RemoveFrameIfNotNeeded();

  ResolveControlFlow();

  if (FLAG_print_ssa_liveranges && CompilerState::ShouldTrace()) {
    const Function& function = flow_graph_.function();

    THR_Print("-- [after ssa allocator] ranges [%s] ---------\n",
              function.ToFullyQualifiedCString());
    PrintLiveRanges();
    THR_Print("----------------------------------------------\n");

    THR_Print("-- [after ssa allocator] ir [%s] -------------\n",
              function.ToFullyQualifiedCString());
    if (FLAG_support_il_printer) {
#ifndef PRODUCT
      FlowGraphPrinter printer(flow_graph_, true);
      printer.PrintBlocks();
#endif
    }
    THR_Print("----------------------------------------------\n");
  }
}

}  // namespace dart
