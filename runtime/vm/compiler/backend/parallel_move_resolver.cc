// Copyright (c) 2022, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/backend/parallel_move_resolver.h"

#include <algorithm>
#include <array>

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

ParallelMoveResolver::ParallelMoveResolver(bool is_intrinsic) : is_intrinsic_(is_intrinsic), moves_(32) {}

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
  // we have a cycle.  Search for such a blocking move and use a temporary
  // location to break the cycle.
  for (intptr_t i = 0; i < moves_.length(); i++) {
    auto& other_move = moves_[i];
    if (other_move.Blocks(destination)) {
      ASSERT(other_move.IsPending());
      const auto tmp = CreateTemporary(destination.IsDoubleStackSlot() || destination.IsQuadStackSlot() || destination.IsFpuRegister() ? Location::kFpuRegister : Location::kRegister);
      scheduled_ops_.Add({OpKind::kMove, {tmp, destination}});
      // OS::PrintErr("BreakingLoop(%" Pd ", %s <- %s)\n", i, tmp.ToCString(), destination.ToCString());
      for (intptr_t j = i; j < moves_.length(); j++) {
        if (moves_[j].src().Equals(destination)) {
          moves_[j].set_src(tmp);
        }
      }
      break;
    }
  }

  // This move is not blocked.
  AddMoveToSchedule(index);
}

void ParallelMoveResolver::AddMoveToSchedule(int index) {
  auto& move = moves_[index];
  // OS::PrintErr("AddMoveToSchedule(%d, %s <- %s)\n", index, move.dest().ToCString(), move.src().ToCString());
  scheduled_ops_.Add({OpKind::kMove, move});
  move.Eliminate();
}

void ParallelMoveResolver::LegalizeMoves() {
  auto move_requires_temporary = [](Location src, Location dst) -> bool {
#if defined(TARGET_ARCH_X64)
    const bool is_memory_to_memory = (src.IsStackSlot() || src.IsDoubleStackSlot() || src.IsQuadStackSlot()) && src.kind() == dst.kind();
    return is_memory_to_memory || (src.IsConstant() && dst.IsStackSlot());
#elif defined(TARGET_ARCH_IA32)
    return (src.IsStackSlot() && dst.IsStackSlot());
#elif defined(TARGET_ARCH_ARM64)
    return dst.IsStackSlot() && (src.IsStackSlot() || src.IsConstant());
#elif defined(TARGET_ARCH_ARM)
    return src.IsConstant() && dst.IsStackSlot();
#else
#error Unknown target architecture
#endif
  };

  for (intptr_t i = 0; i < scheduled_ops_.length(); ++i) {
    const auto& op = scheduled_ops_[i];
    if (op.kind == OpKind::kMove) {
      if (move_requires_temporary(op.operands.src(), op.operands.dest())) {
        auto src = op.operands.src();
        auto dst = op.operands.dest();

        const auto temp = CreateTemporary(Location::kRegister);
        scheduled_ops_[i] = {OpKind::kMove, {temp, src}};
        scheduled_ops_.InsertAt(i + 1, {OpKind::kMove, {dst, temp}});
        i++;
      } else {
#if defined(TARGET_ARCH_ARM)
        if (op.operands.src().IsConstant() &&
            (op.operands.dest().IsFpuRegister() ||
             op.operands.dest().IsDoubleStackSlot())) {
          scheduled_ops_[i].temp = CreateTemporary(Location::kRegister);
        }
#endif
      }
    }
  }
}

void ParallelMoveResolver::AllocateTemporaries(ParallelMoveInstr* parallel_move) {
  if (temporaries_.is_empty()) {
    return;
  }

  const intptr_t kAllFpuRegistersList = (static_cast<intptr_t>(1) << kNumberOfFpuRegisters) - 1;
  const intptr_t max_registers[2] = {kNumberOfCpuRegisters, kNumberOfFpuRegisters};

  const auto to_index = [](const Location& loc) -> intptr_t {
    if (loc.IsConstant()) {
      return -1;
    }
    return (loc.kind() - Location::kRegister) >> 2;
  };

  const auto is_temporary = [&](const Location& loc) -> bool {
    const auto index = to_index(loc);
    return index >= 0 && loc.register_code() >= max_registers[index];
  };

#if defined(TARGET_ARCH_X64)
  const auto kReservedCpuTemp = TMP;
  const auto kReservedFpuTemp = FpuTMP;
#else
  const auto kReservedCpuTemp = kNoRegister;
  const auto kReservedFpuTemp = FpuTMP;
#endif

#if 0
  for (auto& op : scheduled_ops_) {
    if (op.kind == OpKind::kMove) {
      OS::PrintErr(" %s <- %s", op.operands.dest().ToCString(), op.operands.src().ToCString());
    }
  }
  OS::PrintErr("\n");
#endif

  // Check if we can reschedule any moves which store into a register
  // to create a temporary register.
  for (intptr_t i = 0, length = scheduled_ops_.length(); i < length - 1; i++) {
    const auto& candidate = scheduled_ops_[i];
    const auto dst = candidate.operands.dest();
    const auto src = candidate.operands.src();
    if (candidate.kind == OpKind::kMove &&
        dst.IsMachineRegister() && !is_temporary(dst) &&
        !is_temporary(src)) {
      // Try to sink it down to the end of the list.
      intptr_t j;
      for (j = i + 1; j < scheduled_ops_.length(); j++) {
        const auto& other_move = scheduled_ops_[j];
        if (other_move.kind == OpKind::kMove &&
            (other_move.operands.src().Equals(dst) ||
             other_move.operands.dest().Equals(src))) {
          break;
        }
      }
      if (j == scheduled_ops_.length()) {
        // OS::PrintErr("sinking %s <- %s from %" Pd " to the end\n", dst.ToCString(), src.ToCString(), i);
        scheduled_ops_.Add(candidate);
        scheduled_ops_[i].kind = OpKind::kNop;
      }
    }
  }

#if 0
  for (auto& op : scheduled_ops_) {
    if (op.kind == OpKind::kMove) {
      OS::PrintErr(" %s <- %s", op.operands.dest().ToCString(), op.operands.src().ToCString());
    }
  }
  OS::PrintErr("\n");
#endif

  // Caveat: parallel_move->next() might be a parallel move itself and thus
  // will have locs() == nullptr.
  // TODO(vegorov) delete this once we normalize live range splitting policy.
  SmallSet<intptr_t> available[2];
  if (parallel_move->next() != nullptr &&
      parallel_move->next()->locs() != nullptr &&
      parallel_move->next()->locs()->always_calls()) {
    // We have an instruction that always calls, which means that we can use any
    // register that is not an input register for the call as a scratch. The
    // rest will be spilled.
    auto locs = parallel_move->next()->locs();
    available[0] = SmallSet<intptr_t>(kAllCpuRegistersList);
    available[1] = SmallSet<intptr_t>(kAllFpuRegistersList);
    for (intptr_t i = 0; i < locs->input_count(); i++) {
      const auto loc = locs->in(i);
      const intptr_t index = to_index(loc);
      if (index >= 0) {
        available[index].Remove(loc.register_code());
      }
    }
  } else {
    available[0].Clear();
    if (kReservedCpuTemp != kNoRegister) {
      available[0].Add(kReservedCpuTemp);
    }
    available[1].Clear();
    if (kReservedFpuTemp != kNoFpuRegister) {
      available[1].Add(kReservedFpuTemp);
    }
  }

  // We need to assign registers to temporaries. For that we are going to
  // use essentially a simple linear scan.

  // Compute mask of registers which can't be used as temporaries.
  intptr_t not_blocked[2] = {~kReservedCpuRegisters, ~0};
  if (is_intrinsic_) {
    // Block additional registers that must be preserved for intrinsics.
    not_blocked[0] &= ~RegMaskBit(ARGS_DESC_REG);
#if !defined(TARGET_ARCH_IA32)
    // Need to preserve CODE_REG to be able to store the PC marker
    // and load the pool pointer.
    not_blocked[0] &= ~RegMaskBit(CODE_REG);
#endif
  }
  if (kReservedCpuTemp != kNoRegister) {
    not_blocked[0] |= RegMaskBit(kReservedCpuTemp);
  }
  not_blocked[0] &= kAllCpuRegistersList;

  std::array<intptr_t, std::max<int>(kNumberOfCpuRegisters, kNumberOfFpuRegisters)> last_use_pos[2];
  last_use_pos[0].fill(-1);
  last_use_pos[1].fill(-1);

  GrowableArray<intptr_t> def_pos(temporaries_.length());
  def_pos.EnsureLength(temporaries_.length(), -1);

  const auto record_use = [&](const Location& loc, intptr_t pos) {
    const auto index = to_index(loc);
    if (index >= 0) {
      if (loc.register_code() < max_registers[index]) {
        last_use_pos[index][loc.register_code()] = pos;
      }
    }
  };

  const auto record_def = [&](const Location& loc, intptr_t pos) {
    const auto index = to_index(loc);
    if (index >= 0) {
      if (loc.register_code() >= max_registers[index]) {
        def_pos[loc.register_code() - max_registers[index]] = pos;
      }
    }
  };

  for (intptr_t i = 0; i < scheduled_ops_.length(); i++) {
    const auto& op = scheduled_ops_[i];
    switch (op.kind) {
      case OpKind::kNop:
        break;
      case OpKind::kMove:
        record_def(op.operands.dest(), i);
        record_use(op.operands.src(), i);
        record_def(op.temp, i - 1);
        break;
    }
  }

  static_assert((Location::kFpuRegister - Location::kRegister) >> 2 == 1);

  const auto allocate_temporary = [&](Location::Kind kind, intptr_t def_pos) -> Location {
    const intptr_t kLastBit = 63;
    const intptr_t index = (kind - Location::kRegister) >> 2;
    auto available_regs =
        static_cast<uint64_t>(
            (available[index].data() & not_blocked[index])) |
        (static_cast<uint64_t>(1) << kLastBit);
    for (intptr_t i = 0; i < max_registers[index]; i++) {
      if (last_use_pos[index][i] > def_pos) {
        available_regs &= ~(static_cast<uint64_t>(1) << i);
      }
    }
    auto reg = Utils::CountTrailingZeros64(available_regs);
    if (reg == kLastBit) {
      // No free CPU register - everything is blocked.
      OS::PrintErr("allocating %s requires spilling\n", parallel_move->ToCString());
      UNREACHABLE();
    }
    available[index].Remove(reg);
    return Location::MachineRegisterLocation(kind, reg);
  };

 const auto def = [&](Location& loc) {
    const auto index = to_index(loc);
    if (index >= 0) {
      if (loc.register_code() >= max_registers[index]) {
        const auto temp_index = loc.register_code() - max_registers[index];
        RELEASE_ASSERT(temporaries_[temp_index].kind() == loc.kind());
        loc = temporaries_[temp_index];
      }
      available[index].Add(loc.reg());
    }
  };

  const auto alloc = [&](Location& loc) {
    const auto index = to_index(loc);
    if (index >= 0) {
      if (loc.register_code() >= max_registers[index]) {
        const auto temp_index = loc.register_code() - max_registers[index];
        if (temporaries_[temp_index].IsInvalid()) {
          temporaries_[temp_index] = allocate_temporary(loc.kind(), def_pos[temp_index]);
        }
        loc = temporaries_[temp_index];
      }
    }
  };

  const auto use = [&](Location& loc) {
    const auto index = to_index(loc);
    if (index >= 0) {
      if (loc.register_code() < max_registers[index]) {
        available[index].Remove(loc.register_code());
      }
    }
  };

  // auto print_set = [&](intptr_t regs) {
  //  bool comma = false;
  //  for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
  //    if (regs & (1 << i)) {
  //      OS::PrintErr("%s%s", comma ? ", " : "", RegisterNames::RegisterName(static_cast<Register>(i)));
  //      comma = true;
  //    }
  //  }
  //  OS::PrintErr("\n");
  // };

  for (intptr_t i = scheduled_ops_.length() - 1; i >= 0; i--) {
    auto& op = scheduled_ops_[i];
    // if (op.kind == OpKind::kMove) {
      // OS::PrintErr("%" Pd ": %s -> %s\n", i, op.operands.src().ToCString(), op.operands.dest().ToCString());
      // OS::PrintErr("  avail: ");
      // print_set(available[0].data());
      // OS::PrintErr("  not_blocked: ");
    //  print_set(not_blocked[0]);
    //}
    alloc(op.temp);
    switch (op.kind) {
      case OpKind::kNop:
        break;
      case OpKind::kMove:
        def(*op.operands.dest_slot());
        use(*op.operands.src_slot());
        alloc(*op.operands.src_slot());
        break;
    }
    def(op.temp);
  }
}

void ParallelMoveResolver::PrintScheduleTo(const MoveSchedule& schedule, BaseTextBuffer* f) {
  bool comma = false;
  for (const auto& move : schedule) {
    switch (move.kind) {
      case OpKind::kNop:
        break;
      case OpKind::kMove:
        if (comma) f->AddString(", ");
        move.operands.dest().PrintTo(f);
        f->AddString(" <- ");
        move.operands.src().PrintTo(f);
        comma = true;
        break;
    }
  }
}

void ParallelMoveEmitter::EmitNativeCode() {
  for (const auto& op : *parallel_move_->move_schedule()) {
    switch (op.kind) {
      case ParallelMoveResolver::OpKind::kNop:
        break;
      case ParallelMoveResolver::OpKind::kMove:
        EmitMove(op);
        break;
    }
  }
}

void ParallelMoveEmitter::EmitMove(const ParallelMoveResolver::Op& op) {
  const Location src = op.operands.src();
  const Location dst = op.operands.dest();

#if defined(TARGET_ARCH_ARM)
  if (src.IsConstant() &&
      (dst.IsFpuRegister() ||
        dst.IsDoubleStackSlot())) {
    ASSERT(op.temp.IsRegister());
    src.constant_instruction()->EmitMoveToLocation(compiler_, dst, op.temp.reg());
    return;
  }
#endif

  compiler_->EmitMove(dst, src);
}

}  // namespace dart
