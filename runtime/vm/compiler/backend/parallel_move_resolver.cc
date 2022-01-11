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

ParallelMoveResolver::ParallelMoveResolver(bool is_intrinsic,
                                           bool has_frame,
                                           intptr_t spill_slot_count)
    : is_intrinsic_(is_intrinsic),
      has_frame_(has_frame),
      spill_slot_count_(spill_slot_count),
      moves_(32) {}

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

  // Rebase SPREG based stack slots to use FPREG is this is possible and
  // it improves the code quality.
  for (auto op : scheduled_ops_) {
    if (op.kind != OpKind::kNop) {
      op.operands.set_src(
          RebaseStackSlotIfBeneficial(op.operands.src(), /*move_pair=*/false));
      op.operands.set_dest(
          RebaseStackSlotIfBeneficial(op.operands.dest(), /*move_pair=*/false));
    }
  }

  // Schedule is ready. Update parallel move itself.
  parallel_move->set_move_schedule(MoveSchedule::From(scheduled_ops_));
  scheduled_ops_.Clear();
}

void ParallelMoveResolver::BuildInitialMoveList(
    ParallelMoveInstr* parallel_move) {
  // Rebase FP relative moves to have uniform encoding of operands.
  auto rebase_stack_location = [&](const Location& loc) -> Location {
    if (loc.IsStackSlot() || loc.IsDoubleStackSlot()) {
      if (loc.base_reg() == FPREG) {
        return loc.ToSPRelative(spill_slot_count_);
      }
    }
    return loc;
  };

  // Perform a linear sweep of the moves to add them to the initial list of
  // moves to perform, ignoring any move that is redundant (the source is
  // the same as the destination, the destination is ignored and
  // unallocated, or the move was already eliminated).
  for (int i = 0; i < parallel_move->NumMoves(); i++) {
    auto move = *parallel_move->MoveOperandsAt(i);
    move.set_dest(rebase_stack_location(move.dest()));
    move.set_src(rebase_stack_location(move.src()));
    if (!move.IsRedundant()) moves_.Add(move);
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
      const auto tmp = CreateTemporary(destination.IsDoubleStackSlot() ||
                                               destination.IsQuadStackSlot() ||
                                               destination.IsFpuRegister()
                                           ? Location::kFpuRegister
                                           : Location::kRegister);
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
    const bool is_memory_to_memory =
        (src.IsStackSlot() || src.IsDoubleStackSlot() ||
         src.IsQuadStackSlot()) &&
        src.kind() == dst.kind();
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

namespace {
struct AllocationState {
  static constexpr intptr_t kMaxNumberOfRegisters =
      std::max<intptr_t>(kNumberOfCpuRegisters, kNumberOfFpuRegisters);

  AllocationState(Location::Kind kind,
                  GrowableArray<Location>* temporaries,
                  GrowableArray<intptr_t>* def_pos,
                  intptr_t* spill_slot_count,
                  GrowableArray<std::pair<intptr_t, MoveOperands>>* spill_moves)
      : kind(kind),
        spill_slot_size(
            kind == Location::kRegister ? 1 : (kFpuRegisterSize / kWordSize)),
        max_registers(kind == Location::kRegister ? kNumberOfCpuRegisters
                                                  : kNumberOfFpuRegisters),
        spill_slot_count(spill_slot_count),
        temporaries(*temporaries),
        def_pos(*def_pos),
        spill_moves(*spill_moves) {
    last_use_pos.fill(-1);
  }

  void Init() {
    for (intptr_t r = 0; r < max_registers; r++) {
      if (!available.Contains(r)) {
        alive[r] = Location::MachineRegisterLocation(kind, r);
      }
    }
  }

  void Finalize() {
    for (intptr_t r = 0;
         r < Utils::Minimum(max_registers, restore_from.length()); r++) {
      if (!spill_candidates.Contains(r) && !restore_from[r].IsInvalid()) {
        // We have not reached the definition of this register. We need to
        // actually emit spilling code.
        spill_moves.Add(
            {-1,
             {restore_from[r], Location::MachineRegisterLocation(kind, r)}});
      }
    }
  }

  Location AllocateTemporary(intptr_t def_pos, intptr_t cur_pos) {
    const intptr_t kLastBit = 63;
    auto available_regs =
        static_cast<uint64_t>((available.data() & not_blocked.data())) |
        (static_cast<uint64_t>(1) << kLastBit);
    for (intptr_t i = 0; i < max_registers; i++) {
      if (last_use_pos[i] > def_pos) {
        available_regs &= ~(static_cast<uint64_t>(1) << i);
      }
    }
    auto reg = Utils::CountTrailingZeros64(available_regs);
    if (reg == kLastBit) {
      // No free CPU register - everything is blocked. Check if there is a
      // suitable spill candidate which can be used here to become a temporary.
      //      const auto available_spill_candidates =
      //          static_cast<uint64_t>(spill_candidates.data() &
      //                                ~spilled.data()) |
      //          (static_cast<uint64_t>(1) << kLastBit);
      //      reg = Utils::CountTrailingZeros64(available_spill_candidates);
      //      if (reg == kLastBit) {
      // Now lets try to find a register which can be used as a temporary,
      // select the candidate that has minimum last_use_pos
      // TODO(vegorov) finally do the eviction cost computation here.
      // e.g. prefer the candidate that is cheap to evict.
      intptr_t candidate = -1;
      intptr_t last_candidate_use_pos = -1;
      for (intptr_t r = 0; r < max_registers; r++) {
        if (!not_blocked.Contains(r)) {
          continue;
        }
        if (candidate == -1 || spill_candidates.Contains(r) ||
            last_candidate_use_pos > last_use_pos[r]) {
          candidate = r;
          last_candidate_use_pos =
              spill_candidates.Contains(r) ? -1 : last_use_pos[r];
        }
      }
      RELEASE_ASSERT(candidate != -1);

      OS::PrintErr(
          "found spilling candidate %s free until %" Pd "\n",
          Location::MachineRegisterLocation(kind, candidate).ToCString(),
          last_use_pos[candidate]);

      reg = candidate;
      //      }
      //      spilled.Add(reg);
      Spill(cur_pos, reg);
    }
    available.Remove(reg);
    return Location::MachineRegisterLocation(kind, reg);
  }

  void ProcessUse(intptr_t cur_pos, Location& loc) {
    OS::PrintErr("USE(%s) @ %" Pd "\n", loc.ToCString(), cur_pos);

    if (loc.register_code() < max_registers) {
      available.Remove(loc.register_code());
      if (!alive[loc.register_code()].Equals(loc)) {
        Spill(cur_pos, loc.register_code());
      }
      alive[loc.register_code()] = loc;
    }
  }

  void AllocateUse(intptr_t cur_pos, Location& loc) {
    OS::PrintErr("ALLOC(%s) @ %" Pd "\n", loc.ToCString(), cur_pos);

    if (loc.register_code() >= max_registers) {
      const auto temp_index = loc.register_code() - max_registers;
      if (temporaries[temp_index].IsInvalid()) {
        temporaries[temp_index] =
            AllocateTemporary(def_pos[temp_index], cur_pos);
      }
      const auto result = temporaries[temp_index];
      alive[result.register_code()] = loc;
      loc = result;
    }
  }

  void ProcessDef(intptr_t cur_pos, Location& loc) {
    OS::PrintErr("DEF(%s) @ %" Pd "\n", loc.ToCString(), cur_pos);
    const auto register_code = loc.register_code();
    if (register_code >= max_registers) {
      const auto temp_index = register_code - max_registers;
      if (temporaries[temp_index].IsInvalid()) {
        // We have arrived here spilled. We have two options: either spilling
        // can be fused with the current move, or we need to allocate a
        // temporary.
        AllocateUse(cur_pos, loc);  // We allocate a temporary.

        // Emit spilling move.
        spill_moves.Add({cur_pos, {restore_from[register_code], loc}});
        restore_from[register_code] = Location();
      } else {
        loc = temporaries[temp_index];
      }
    } else {
      OS::PrintErr("alive -> %s vs %s\n", alive[register_code].ToCString(), loc.ToCString());
      if (!alive[register_code].Equals(loc)) {
        // We have arrive here spilled. We have two options: either spilling
        // can be fused with the current move, or we need to allocate a
        // temporary.
        temporaries.Add(Location());
        def_pos.Add(cur_pos);
        loc = Location(kind, kNumberOfCpuRegisters + temporaries.length() - 1);
        AllocateUse(cur_pos, loc);
        // Emit spilling move.
        spill_moves.Add({cur_pos, {restore_from[register_code], loc}});
        restore_from[register_code] = Location();
      }
    }
    alive[loc.register_code()] = Location();
    available.Add(loc.register_code());
  }

  void Spill(intptr_t cur_pos, intptr_t reg) {
    if (alive[reg].IsInvalid()) {
      // Nothing to do. The location is not currently used.
      return;
    }

    const intptr_t current_value = alive[reg].register_code();

    if (current_value > max_registers) {
      // Evict temporary.
      temporaries[current_value - max_registers] = Location();
    }

    restore_from.EnsureLength(current_value + 1, Location());
    if (restore_from[current_value]
            .IsInvalid()) {  // We need to allocate a spill slot.
      const intptr_t spill_slot_index = *spill_slot_count;
      *spill_slot_count += spill_slot_size;
      restore_from[current_value] =
          Location::StackSlot(spill_slot_index, FPREG);  // TODO(vegorov)
    }
    spill_moves.Add({cur_pos,
                     {Location::MachineRegisterLocation(kind, reg),
                      restore_from[current_value]}});
  }

  const Location::Kind kind;
  const intptr_t spill_slot_size;
  const intptr_t max_registers;
  intptr_t* const spill_slot_count;
  GrowableArray<Location>& temporaries;
  GrowableArray<intptr_t>& def_pos;
  GrowableArray<std::pair<intptr_t, MoveOperands>>& spill_moves;
  SmallSet<intptr_t> spill_candidates;
  SmallSet<intptr_t> spilled;
  SmallSet<intptr_t> available;
  SmallSet<intptr_t> not_blocked;
  std::array<intptr_t, kMaxNumberOfRegisters> last_use_pos;
  GrowableArray<Location> restore_from;
  std::array<Location, kMaxNumberOfRegisters> alive;
};
}  // namespace

void ParallelMoveResolver::AllocateTemporaries(
    ParallelMoveInstr* parallel_move) {
  if (temporaries_.is_empty()) {
    return;
  }

  const intptr_t kAllFpuRegistersList =
      (static_cast<intptr_t>(1) << kNumberOfFpuRegisters) - 1;

  intptr_t spill_slot_count = 0;
  GrowableArray<std::pair<intptr_t, MoveOperands>> spill_moves;

  GrowableArray<intptr_t> def_pos(temporaries_.length());
  def_pos.EnsureLength(temporaries_.length(), -1);

  AllocationState state[2] = {
      AllocationState(Location::kRegister, &temporaries_, &def_pos,
                      &spill_slot_count, &spill_moves),
      AllocationState(Location::kFpuRegister, &temporaries_, &def_pos,
                      &spill_slot_count, &spill_moves),
  };

  const auto to_index = [](const Location& loc) -> intptr_t {
    if (loc.IsConstant()) {
      return -1;
    }
    return (loc.kind() - Location::kRegister) >> 2;
  };

  const auto is_temporary = [&](const Location& loc) -> bool {
    const auto index = to_index(loc);
    return index >= 0 && loc.register_code() >= state[index].max_registers;
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
      OS::PrintErr(" %s <- %s", op.operands.dest().ToCString(),
                   op.operands.src().ToCString());
    }
  }
  OS::PrintErr("\n");
#endif

  intptr_t spill_candidate_count = 0;

  // Check if we can reschedule any moves which store into a register
  // to create a temporary register.
  for (intptr_t i = 0, length = scheduled_ops_.length(); i < length; i++) {
    const auto& candidate = scheduled_ops_[i];
    const auto dst = candidate.operands.dest();
    const auto src = candidate.operands.src();
    if (candidate.kind == OpKind::kMove) {
      if (dst.IsMachineRegister() && !is_temporary(dst) && !is_temporary(src)) {
        // If this is R <- ? move where neither destination, nor source are
        // temporaries. Try moving it to the very end of the move sequence
        // (given that there are no interfering moves) - if this moves
        // the move past any move that defines a temporary then it will create
        // a valid register to be used as temporary.
        intptr_t j;
        bool encountered_any_temporaries = false;
        for (j = i + 1; j < scheduled_ops_.length(); j++) {
          const auto& other_move = scheduled_ops_[j];
          if (other_move.kind == OpKind::kMove) {
            // Check if this moves destroys the candidate's source.
            if (other_move.operands.dest().Equals(src)) {
              break;
            }
            if (is_temporary(other_move.operands.dest()) &&
                other_move.operands.dest().kind() == dst.kind()) {
              encountered_any_temporaries = true;
            }
          }
        }
        if (encountered_any_temporaries && j == scheduled_ops_.length()) {
          scheduled_ops_.Add(candidate);
          scheduled_ops_[i].kind = OpKind::kNop;
          continue;
        }
      }

      if (i > 0 && src.IsMachineRegister() && !is_temporary(dst) &&
          !is_temporary(src)) {
        //OS::PrintErr("checking %s <- %s\n", dst.ToCString(), src.ToCString());
        intptr_t j;
        for (j = 0; j < scheduled_ops_.length(); j++) {
          if (i == j) continue;

          const auto& other_move = scheduled_ops_[j];
          if (other_move.kind == OpKind::kMove) {
            if (other_move.operands.Blocks(dst) ||
                other_move.operands.dest().Equals(src)) {
              // OS::PrintErr("interfers %s <- %s\n",
              //             other_move.operands.dest().ToCString(),
              //             other_move.operands.src().ToCString());
              break;
            }
          }
        }
        if (j == scheduled_ops_.length()) {
          // We can schedule the move at the very start, thus allowing
          // |src| to be used as a temporary register.
          scheduled_ops_.InsertAt(spill_candidate_count, Op(candidate));
          scheduled_ops_[i + 1].kind = OpKind::kNop;
          length++;
          i++;
          spill_candidate_count++;
        }
      }
    }
  }

  for (intptr_t i = 0; i < spill_candidate_count; i++) {
    const auto& candidate = scheduled_ops_[i];
    const auto src = candidate.operands.src();
    if (candidate.kind == OpKind::kMove) {
      auto& s = state[to_index(src)];
      s.spill_candidates.Add(src.register_code());
      s.restore_from.EnsureLength(src.register_code() + 1, Location());
      s.restore_from[src.register_code()] = candidate.operands.dest();
    }
  }

  // Caveat: parallel_move->next() might be a parallel move itself and thus
  // will have locs() == nullptr.
  // TODO(vegorov) delete this once we normalize live range splitting policy.
  if (parallel_move->next() != nullptr &&
      parallel_move->next()->locs() != nullptr &&
      parallel_move->next()->locs()->always_calls()) {
    // We have an instruction that always calls, which means that we can use any
    // register that is not an input register for the call (or this parallel
    // move) as a scratch. The rest will be spilled.
    auto locs = parallel_move->next()->locs();
    state[0].available = SmallSet<intptr_t>(kAllCpuRegistersList);
    state[1].available = SmallSet<intptr_t>(kAllFpuRegistersList);
    for (intptr_t i = 0; i < locs->input_count(); i++) {
      const auto loc = locs->in(i);
      const intptr_t index = to_index(loc);
      if (index >= 0) {
        state[index].available.Remove(loc.register_code());
      }
    }
  } else {
    state[0].available.Clear();
    if (kReservedCpuTemp != kNoRegister) {
      state[0].available.Add(kReservedCpuTemp);
    }
    state[1].available.Clear();
    if (kReservedFpuTemp != kNoFpuRegister) {
      state[1].available.Add(kReservedFpuTemp);
    }
  }

  state[0].Init();
  state[1].Init();

  // We need to assign registers to temporaries. For that we are going to
  // use essentially a simple linear scan.

  // Compute mask of registers which can't be used as temporaries.
  state[0].not_blocked =
      SmallSet<intptr_t>(~kReservedCpuRegisters & kAllCpuRegistersList);
  state[1].not_blocked = SmallSet<intptr_t>(kAllFpuRegistersList);
  if (is_intrinsic_) {
    // Block additional registers that must be preserved for intrinsics.
    state[0].not_blocked.Remove(ARGS_DESC_REG);
#if !defined(TARGET_ARCH_IA32)
    // Need to preserve CODE_REG to be able to store the PC marker
    // and load the pool pointer.
    state[0].not_blocked.Remove(CODE_REG);
#endif
  }
  if (kReservedCpuTemp != kNoRegister) {
    state[0].not_blocked.Add(kReservedCpuTemp);
  }

  const auto record_use = [&](const Location& loc, intptr_t pos) {
    const auto index = to_index(loc);
    if (index >= 0) {
      auto& s = state[index];
      if (loc.register_code() < s.max_registers) {
        s.last_use_pos[loc.register_code()] = pos;
      }
    }
  };

  const auto record_def = [&](const Location& loc, intptr_t pos) {
    const auto index = to_index(loc);
    if (index >= 0) {
      const auto& s = state[index];
      if (loc.register_code() >= s.max_registers) {
        def_pos[loc.register_code() - s.max_registers] = pos;
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

  const auto def = [&](intptr_t cur_pos, Location& loc) {
    const auto index = to_index(loc);
    if (index >= 0) {
      state[index].ProcessDef(cur_pos, loc);
    }
  };

  const auto alloc = [&](intptr_t cur_pos, Location& loc) {
    const auto index = to_index(loc);
    if (index >= 0) {
      auto& s = state[index];
      s.AllocateUse(cur_pos, loc);
    }
  };

  const auto use = [&](intptr_t cur_pos, Location& loc) {
    const auto index = to_index(loc);
    if (index >= 0) {
      state[index].ProcessUse(cur_pos, loc);
    }
  };

  for (intptr_t i = scheduled_ops_.length() - 1; i >= 0; i--) {
    auto& op = scheduled_ops_[i];
    alloc(i, op.temp);
    switch (op.kind) {
      case OpKind::kNop:
        break;
      case OpKind::kMove:
        OS::PrintErr("[%" Pd "] %s <- %s\n", i, op.operands.dest().ToCString(),
                    op.operands.src().ToCString());
        OS::PrintErr("~def~ %s <- %s\n", op.operands.dest().ToCString(),
                    op.operands.src().ToCString());
        def(i, *op.operands.dest_slot());
        OS::PrintErr("~use~ %s <- %s\n", op.operands.dest().ToCString(),
                    op.operands.src().ToCString());
        use(i, *op.operands.src_slot());
        OS::PrintErr("~alloc~ %s <- %s\n", op.operands.dest().ToCString(),
                    op.operands.src().ToCString());
        alloc(i, *op.operands.src_slot());
        break;
    }
    def(i - 1, op.temp);
  }

  state[0].Finalize();
  state[1].Finalize();

  GrowableArray<Op> ops;

  auto emit_spill_moves = [&](intptr_t pos) {
    while (!spill_moves.is_empty() && spill_moves.Last().first < pos) {
      ops.Add({OpKind::kMove, spill_moves.RemoveLast().second});
    }
  };

  for (intptr_t i = 0; i < scheduled_ops_.length(); i++) {
    emit_spill_moves(i);
    if (scheduled_ops_[i].kind != OpKind::kNop) {
      ops.Add(scheduled_ops_[i]);
    }
  }
  emit_spill_moves(scheduled_ops_.length());

  scheduled_ops_.Clear();
  scheduled_ops_.AddArray(ops);

  /*
  for (intptr_t index = 0; index <= 1; index++) {
    for (intptr_t reg = 0; reg < state[index].max_registers; reg++) {
      if (state[index].spilled.Contains(reg)) {
        scheduled_ops_.Add(
            {OpKind::kMove,
             {Location::MachineRegisterLocation(
                  index == 0 ? Location::kRegister : Location::kFpuRegister,
                  reg),
              state[index].restore_from[reg]}});
      }
    }
  }
*/
}

Location ParallelMoveResolver::RebaseStackSlotIfBeneficial(const Location& loc,
                                                           bool move_pair) {
#if defined(TARGET_ARCH_ARM) || defined(TARGET_ARCH_ARM64)
  if (has_frame_) {
#if defined(TARGET_ARCH_ARM64)
    const auto can_hold = [&](compiler::OperandSize operand_size,
                              int32_t offset) -> bool {
      return compiler::Address::CanHoldOffset(
          offset,
          move_pair ? compiler::Address::PairOffset : compiler::Address::Offset,
          operand_size);
    };
#else
    const auto can_hold = [&](compiler::OperandSize operand_size,
                              int32_t offset) -> bool {
      int32_t offset_mask;
      return compiler::Address::CanHoldLoadOffset(operand_size, offset,
                                                  &offset_mask);
    };
#endif

    compiler::OperandSize operand_size;

    switch (loc.kind()) {
      case Location::kStackSlot: {
        operand_size = compiler::kWordBytes;
        break;
      }
      case Location::kDoubleStackSlot:
        ASSERT(!move_pair);
        operand_size = compiler::kDWord;
        break;
      case Location::kQuadStackSlot:
        ASSERT(!move_pair);
        operand_size = compiler::kQWord;
        break;

      default:
        return loc;
    }

    ASSERT(loc.base_reg() == SPREG);
    int32_t offset = loc.ToStackSlotOffset();
    if (!can_hold(operand_size, offset)) {
      // Offset is too big. Check if we can use FP relative offset.
      offset -= spill_slot_count_ * compiler::target::kWordSize;
      if (can_hold(operand_size, offset)) {
        return Location::StackSlot(loc.stack_index() - spill_slot_count_,
                                   FPREG);
      }
    }
  }
#endif

  return loc;
}

void ParallelMoveResolver::PrintScheduleTo(const MoveSchedule& schedule,
                                           BaseTextBuffer* f) {
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
  if (src.IsConstant() && (dst.IsFpuRegister() || dst.IsDoubleStackSlot())) {
    ASSERT(op.temp.IsRegister());
    src.constant_instruction()->EmitMoveToLocation(compiler_, dst,
                                                   op.temp.reg());
    return;
  }
#endif

  compiler_->EmitMove(dst, src);
}

}  // namespace dart
