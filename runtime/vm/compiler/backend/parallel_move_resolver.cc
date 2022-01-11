// Copyright (c) 2022, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/backend/parallel_move_resolver.h"

namespace dart {

template <typename Element>
class alignas(alignof(Element)) LengthPrefixedArray {
 public:
  static LengthPrefixedArray* Allocate(intptr_t length) {
    auto result = reinterpret_cast<LengthPrefixedArray*>(
        Thread::Current()->zone()->AllocUnsafe(sizeof(LengthPrefixedArray) +
                                               length * sizeof(Element)));
    result->length_ = length;
    return result;
  }

  intptr_t length() const { return length_; }
  Element* data() { OPEN_ARRAY_START(Element, Element); }
  const Element* data() const { OPEN_ARRAY_START(Element, Element); }

  Element* begin() { return data(); }

  const Element* begin() const { return data(); }

  Element* end() { return data() + length_; }

  const Element* end() const { return data() + length_; }

 private:
  LengthPrefixedArray() {}

  intptr_t length_;
};

class MoveSchedule : public LengthPrefixedArray<ParallelMoveResolver::Op> {
 public:
  static const MoveSchedule* From(
      const GrowableArray<ParallelMoveResolver::Op>& ops) {
    intptr_t count = 0;
    for (const auto& op : ops) {
      if (op.kind != ParallelMoveResolver::OpKind::kNop) count++;
    }

    auto result = static_cast<MoveSchedule*>(
        LengthPrefixedArray<ParallelMoveResolver::Op>::Allocate(count));
    intptr_t i = 0;
    for (const auto& op : ops) {
      if (op.kind != ParallelMoveResolver::OpKind::kNop)
        result->data()[i++] = op;
    }
    return result;
  }
};

static uword RegMaskBit(Register reg) {
  return ((reg) != kNoRegister) ? (1 << (reg)) : 0;
}

ParallelMoveResolver::ParallelMoveResolver() : moves_(32) {}

void ParallelMoveResolver::Resolve(ParallelMoveInstr* parallel_move) {
  ASSERT(moves_.is_empty());

  // Build up a worklist of moves.
  BuildInitialMoveList(parallel_move);

  const InstructionSource& move_source = InstructionSource(
      TokenPosition::kParallelMove, parallel_move->inlining_id());
  for (intptr_t i = 0; i < moves_.length(); ++i) {
    const MoveOperands& move = moves_[i];
    // Skip constants to perform them last.  They don't block other moves
    // and skipping such moves with register destinations keeps those
    // registers free for the whole algorithm.
    if (!move.IsEliminated() && !move.src().IsConstant()) {
      PerformMove(move_source, i);
    }
  }

  // Perform the moves with constant sources.
  for (const auto& move : moves_) {
    if (!move.IsEliminated()) {
      ASSERT(move.src().IsConstant());
      scheduled_ops_.Add({OpKind::kMove, move});
    }
  }
  moves_.Clear();

  LegalizeMoves();

  AllocateTemporaries(parallel_move);

  // Schedule is ready. Update parallel move itself.
  parallel_move->set_move_schedule(MoveSchedule::From(scheduled_ops_));
  scheduled_ops_.Clear();
}

void ParallelMoveResolver::BuildInitialMoveList(
    ParallelMoveInstr* parallel_move) {
  // Perform a linear sweep of the moves to add them to the initial list of
  // moves to perform, ignoring any move that is redundant (the source is
  // the same as the destination, the destination is ignored and
  // unallocated, or the move was already eliminated).
  for (int i = 0; i < parallel_move->NumMoves(); i++) {
    MoveOperands* move = parallel_move->MoveOperandsAt(i);
    if (!move->IsRedundant()) moves_.Add(*move);
  }
}

void ParallelMoveResolver::PerformMove(const InstructionSource& source,
                                       int index) {
  // Each call to this function performs a move and deletes it from the move
  // graph.  We first recursively perform any move blocking this one.  We
  // mark a move as "pending" on entry to PerformMove in order to detect
  // cycles in the move graph.  We use operand swaps to resolve cycles,
  // which means that a call to PerformMove could change any source operand
  // in the move graph.

  ASSERT(!moves_[index].IsPending());
  ASSERT(!moves_[index].IsRedundant());

  // Clear this move's destination to indicate a pending move.  The actual
  // destination is saved in a stack-allocated local.  Recursion may allow
  // multiple moves to be pending.
  ASSERT(!moves_[index].src().IsInvalid());
  Location destination = moves_[index].MarkPending();

  // Perform a depth-first traversal of the move graph to resolve
  // dependencies.  Any unperformed, unpending move with a source the same
  // as this one's destination blocks this one so recursively perform all
  // such moves.
  for (int i = 0; i < moves_.length(); ++i) {
    const MoveOperands& other_move = moves_[i];
    if (other_move.Blocks(destination) && !other_move.IsPending()) {
      // Though PerformMove can change any source operand in the move graph,
      // this call cannot create a blocking move via a swap (this loop does
      // not miss any).  Assume there is a non-blocking move with source A
      // and this move is blocked on source B and there is a swap of A and
      // B.  Then A and B must be involved in the same cycle (or they would
      // not be swapped).  Since this move's destination is B and there is
      // only a single incoming edge to an operand, this move must also be
      // involved in the same cycle.  In that case, the blocking move will
      // be created but will be "pending" when we return from PerformMove.
      PerformMove(source, i);
    }
  }

  // We are about to resolve this move and don't need it marked as
  // pending, so restore its destination.
  moves_[index].ClearPending(destination);

  // This move's source may have changed due to swaps to resolve cycles and
  // so it may now be the last move in the cycle.  If so remove it.
  if (moves_[index].src().Equals(destination)) {
    moves_[index].Eliminate();
    return;
  }

  // The move may be blocked on a (at most one) pending move, in which case
  // we have a cycle.  Search for such a blocking move and perform a swap to
  // resolve it.
  for (auto& other_move : moves_) {
    if (other_move.Blocks(destination)) {
      ASSERT(other_move.IsPending());
      AddSwapToSchedule(index);
      return;
    }
  }

  // This move is not blocked.
  AddMoveToSchedule(index);
}

void ParallelMoveResolver::AddMoveToSchedule(int index) {
  auto& move = moves_[index];
  scheduled_ops_.Add({OpKind::kMove, move});
  move.Eliminate();
}

void ParallelMoveResolver::AddSwapToSchedule(int index) {
  auto& move = moves_[index];
  const auto source = move.src();
  const auto destination = move.dest();

  scheduled_ops_.Add({OpKind::kSwap, move});

  // The swap of source and destination has executed a move from source to
  // destination.
  move.Eliminate();

  // Any unperformed (including pending) move with a source of either
  // this move's source or destination needs to have their source
  // changed to reflect the state of affairs after the swap.
  for (auto& other_move : moves_) {
    if (other_move.Blocks(source)) {
      other_move.set_src(destination);
    } else if (other_move.Blocks(destination)) {
      other_move.set_src(source);
    }
  }
}

void ParallelMoveResolver::LegalizeMoves() {
  auto move_requires_temporary = [](Location src, Location dst) {
    return (src.IsStackSlot() && dst.IsStackSlot()) ||
           (src.IsConstant() && dst.IsStackSlot());
  };

  for (intptr_t i = 0; i < scheduled_ops_.length(); ++i) {
    const auto& op = scheduled_ops_[i];
    if (op.kind == OpKind::kMove &&
        move_requires_temporary(op.operands.src(), op.operands.dest())) {
      auto src = op.operands.src();
      auto dst = op.operands.dest();

      const Location temp = Location(
          Location::kRegister, kNumberOfCpuRegisters + temporaries_.length());
      temporaries_.Add(Location());

      scheduled_ops_[i] = {OpKind::kMove, {temp, src}};
      scheduled_ops_.InsertAt(i + 1, {OpKind::kMove, {dst, temp}});
      i++;
    }
  }
}

void ParallelMoveResolver::AllocateTemporaries(ParallelMoveInstr* parallel_move) {
  if (temporaries_.is_empty()) {
    return;
  }

  if (parallel_move->next() != nullptr &&
      parallel_move->next()->locs()->always_calls()) {
    // We have an instruction that always calls, which means that we can use any
    // register that is not an input register for the call as a scratch. The
    // rest will be spilled.
    auto locs = parallel_move->next()->locs();
    intptr_t live_registers = 0;
    for (intptr_t i = 0; i < locs->input_count(); i++) {
      const auto loc = locs->in(i);
      if (loc.IsRegister()) {
        live_registers |= 1 << loc.reg();
      }
    }
    live_registers_ = live_registers;
  } else {
    live_registers_ = -1;
  }

  // We need to assign registers to temporaries. For that we are going to
  // use essentially a simple linear scan.

  // Compute mask of registers which can't be used as temporaries.
  intptr_t blocked_mask = kReservedCpuRegisters;
  if (compiler_->intrinsic_mode()) {
    // Block additional registers that must be preserved for intrinsics.
    blocked_mask |= RegMaskBit(ARGS_DESC_REG);
#if !defined(TARGET_ARCH_IA32)
    // Need to preserve CODE_REG to be able to store the PC marker
    // and load the pool pointer.
    blocked_mask |= RegMaskBit(CODE_REG);
#endif
  }

  intptr_t last_use_pos[kNumberOfCpuRegisters];
  for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
    last_use_pos[i] = -1;
  }

  GrowableArray<intptr_t> def_pos(temporaries_.length());
  def_pos.EnsureLength(temporaries_.length(), -1);

  auto record_use = [&](const Location& loc, intptr_t pos) {
    if (loc.IsRegister()) {
      if (loc.register_code() < kNumberOfCpuRegisters) {
        last_use_pos[loc.register_code()] = pos;
      }
    }
  };

  auto record_def = [&](const Location& loc, intptr_t pos) {
    if (loc.IsRegister()) {
      if (loc.register_code() >= kNumberOfCpuRegisters) {
        def_pos[loc.register_code() - kNumberOfCpuRegisters] = pos;
      }
    }
  };

  for (intptr_t i = 0; i < scheduled_ops_.length(); i++) {
    const auto& op = scheduled_ops_[i];
    switch (op.kind) {
      case OpKind::kNop:
        break;
      case OpKind::kSwap:
        record_def(op.operands.src(), i);
        record_def(op.operands.dest(), i);
        record_use(op.operands.src(), i);
        record_use(op.operands.dest(), i);
        break;
      case OpKind::kMove:
        record_def(op.operands.dest(), i);
        record_use(op.operands.src(), i);
        break;
    }
  }

  SmallSet<Register> available(~live_registers_);

  auto allocate_temporary = [&](intptr_t def_pos) -> Register {
    auto available_regs =
        static_cast<uint64_t>(
            (available.data() & ~blocked_mask & kAllCpuRegistersList)) |
        (static_cast<uint64_t>(1) << kNumberOfCpuRegisters);
    for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
      if (last_use_pos[i] > def_pos) {
        available_regs &= ~(static_cast<uint64_t>(1) << i);
      }
    }
    auto reg = Utils::CountTrailingZeros64(available_regs);
    if (reg == kNumberOfCpuRegisters) {
      // No free CPU register - everything is blocked.
      UNREACHABLE();
    }
    const auto result = static_cast<Register>(reg);
    available.Remove(result);
    return result;
  };

  auto def = [&](Location& loc) {
    if (loc.IsRegister()) {
      if (loc.register_code() >= kNumberOfCpuRegisters) {
        const auto temp_index = loc.register_code() - kNumberOfCpuRegisters;
        ASSERT(temporaries_[temp_index].IsRegister());
        loc = temporaries_[temp_index];
      } else {
        available.Add(loc.reg());
      }
    }
  };

  auto alloc = [&](Location& loc) {
    if (loc.IsRegister()) {
      if (loc.register_code() >= kNumberOfCpuRegisters) {
        const auto temp_index = loc.register_code() - kNumberOfCpuRegisters;
        if (temporaries_[temp_index].IsInvalid()) {
          temporaries_[temp_index] = Location::RegisterLocation(
              allocate_temporary(def_pos[temp_index]));
        }
        loc = temporaries_[temp_index];
      }
    }
  };

  auto use = [&](Location& loc) {
    if (loc.IsRegister()) {
      if (loc.register_code() < kNumberOfCpuRegisters) {
        available.Remove(loc.reg());
      }
    }
  };

  for (intptr_t i = scheduled_ops_.length() - 1; i >= 0; i--) {
    auto& op = scheduled_ops_[i];
    switch (op.kind) {
      case OpKind::kNop:
        break;
      case OpKind::kSwap:
        def(*op.operands.src_slot());
        def(*op.operands.dest_slot());
        use(*op.operands.src_slot());
        use(*op.operands.dest_slot());
        alloc(*op.operands.src_slot());
        alloc(*op.operands.dest_slot());
        break;
      case OpKind::kMove:
        def(*op.operands.dest_slot());
        use(*op.operands.src_slot());
        alloc(*op.operands.src_slot());
        break;
    }
  }
}

void ParallelMoveEmitter::EmitNativeCode() {
  for (auto op : *parallel_move_->move_schedule()) {
    switch (op.kind) {
      case ParallelMoveResolver::OpKind::kNop:
        break;
      case ParallelMoveResolver::OpKind::kMove:
        EmitMove(op.operands);
        break;
      case ParallelMoveResolver::OpKind::kSwap:
        EmitSwap(op.operands);
        break;
    }
  }
}

void ParallelMoveEmitter::EmitMove(const MoveOperands& move) {
  const Location src = move.src();
  const Location dst = move.dest();
  compiler_->EmitMove(dst, src);
}

bool ParallelMoveEmitter::IsScratchLocation(Location loc) {
  for (const auto& op : *parallel_move_->move_schedule()) {
    if (op.operands.src().Equals(loc) ||
        (op.kind == ParallelMoveResolver::OpKind::kSwap &&
         op.operands.dest().Equals(loc))) {
      return false;
    }
  }

  for (const auto& op : *parallel_move_->move_schedule()) {
    if (op.kind == ParallelMoveResolver::OpKind::kMove &&
        op.operands.dest().Equals(loc)) {
      return true;
    }
  }

  return false;
}

intptr_t ParallelMoveEmitter::AllocateScratchRegister(
    Location::Kind kind,
    uword blocked_mask,
    intptr_t first_free_register,
    intptr_t last_free_register,
    bool* spilled) {
  COMPILE_ASSERT(static_cast<intptr_t>(sizeof(blocked_mask)) * kBitsPerByte >=
                 kNumberOfFpuRegisters);
  COMPILE_ASSERT(static_cast<intptr_t>(sizeof(blocked_mask)) * kBitsPerByte >=
                 kNumberOfCpuRegisters);
  intptr_t scratch = -1;
  for (intptr_t reg = first_free_register; reg <= last_free_register; reg++) {
    if ((((1 << reg) & blocked_mask) == 0) &&
        IsScratchLocation(Location::MachineRegisterLocation(kind, reg))) {
      scratch = reg;
      break;
    }
  }

  if (scratch == -1) {
    *spilled = true;
    for (intptr_t reg = first_free_register; reg <= last_free_register; reg++) {
      if (((1 << reg) & blocked_mask) == 0) {
        scratch = reg;
        break;
      }
    }
  } else {
    *spilled = false;
  }

  return scratch;
}

ParallelMoveEmitter::ScratchFpuRegisterScope::ScratchFpuRegisterScope(
    ParallelMoveEmitter* emitter,
    FpuRegister blocked)
    : emitter_(emitter), reg_(kNoFpuRegister), spilled_(false) {
  COMPILE_ASSERT(FpuTMP != kNoFpuRegister);
  uword blocked_mask =
      ((blocked != kNoFpuRegister) ? 1 << blocked : 0) | 1 << FpuTMP;
  reg_ = static_cast<FpuRegister>(
      emitter_->AllocateScratchRegister(Location::kFpuRegister, blocked_mask, 0,
                                        kNumberOfFpuRegisters - 1, &spilled_));

  if (spilled_) {
    emitter->SpillFpuScratch(reg_);
  }
}

ParallelMoveEmitter::ScratchFpuRegisterScope::~ScratchFpuRegisterScope() {
  if (spilled_) {
    emitter_->RestoreFpuScratch(reg_);
  }
}

ParallelMoveEmitter::TemporaryAllocator::TemporaryAllocator(
    ParallelMoveEmitter* emitter,
    Register blocked)
    : emitter_(emitter),
      blocked_(blocked),
      reg_(kNoRegister),
      spilled_(false) {}

Register ParallelMoveEmitter::TemporaryAllocator::AllocateTemporary() {
  ASSERT(reg_ == kNoRegister);

  uword blocked_mask = RegMaskBit(blocked_) | kReservedCpuRegisters;
  if (emitter_->compiler_->intrinsic_mode()) {
    // Block additional registers that must be preserved for intrinsics.
    blocked_mask |= RegMaskBit(ARGS_DESC_REG);
#if !defined(TARGET_ARCH_IA32)
    // Need to preserve CODE_REG to be able to store the PC marker
    // and load the pool pointer.
    blocked_mask |= RegMaskBit(CODE_REG);
#endif
  }
  reg_ = static_cast<Register>(
      emitter_->AllocateScratchRegister(Location::kRegister, blocked_mask, 0,
                                        kNumberOfCpuRegisters - 1, &spilled_));

  if (spilled_) {
    emitter_->SpillScratch(reg_);
  }

  DEBUG_ONLY(allocated_ = true;)
  return reg_;
}

void ParallelMoveEmitter::TemporaryAllocator::ReleaseTemporary() {
  if (spilled_) {
    emitter_->RestoreScratch(reg_);
  }
  reg_ = kNoRegister;
}

ParallelMoveEmitter::ScratchRegisterScope::ScratchRegisterScope(
    ParallelMoveEmitter* emitter,
    Register blocked)
    : allocator_(emitter, blocked) {
  reg_ = allocator_.AllocateTemporary();
}

ParallelMoveEmitter::ScratchRegisterScope::~ScratchRegisterScope() {
  allocator_.ReleaseTemporary();
}

}  // namespace dart
