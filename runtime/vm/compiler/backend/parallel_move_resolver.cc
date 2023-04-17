// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/backend/parallel_move_resolver.h"

#include <array>

#include "vm/compiler/backend/parallel_move_emitter.h"
#include "vm/zone_text_buffer.h"

#define TRACE_ALLOC(...)

namespace dart {
namespace compiler {

namespace {
struct AbstractMachineState {
  const char* cpu[kNumberOfCpuRegisters];
  const char* fpu[kNumberOfFpuRegisters];

  const intptr_t stack_size;
  const intptr_t fp_base;
  const char** stack;

  static constexpr intptr_t kMaxParams = 150;

  explicit AbstractMachineState(Zone* zone, intptr_t sp_to_fp_delta) :
    stack_size(sp_to_fp_delta + 2 * kMaxParams),
    fp_base(kMaxParams + sp_to_fp_delta),
    stack(zone->Alloc<const char*>(stack_size)) {
    for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
      cpu[i] = RegisterNames::RegisterName(static_cast<Register>(i));
    }
    for (intptr_t i = 0; i < kNumberOfFpuRegisters; i++) {
      fpu[i] = RegisterNames::FpuRegisterName(static_cast<FpuRegister>(i));
    }
    for (intptr_t i = 0; i < stack_size; i++) {
      stack[i] = OS::SCreate(zone, "stack(%" Pd ")", i);
    }
  }

  void Print() {
    OS::PrintErr("{\n");
    for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
      OS::PrintErr("  %s: %s\n",
                   RegisterNames::RegisterName(static_cast<Register>(i)),
                   cpu[i]);
    }
    for (intptr_t i = 0; i < kNumberOfFpuRegisters; i++) {
      OS::PrintErr("  %s: %s\n",
                   RegisterNames::FpuRegisterName(static_cast<FpuRegister>(i)),
                   fpu[i]);
    }
    for (intptr_t i = 0; i < stack_size; i++) {
      OS::PrintErr("  S(%" Pd "): %s\n", i, stack[i]);
    }
    OS::PrintErr("}\n");
  }

  AbstractMachineState(const AbstractMachineState& other) = default;

  const char* ValueOf(const Location& loc) {
    if (loc.IsConstant()) {
      return loc.constant().ToCString();
    } else {
      return *SlotFor(loc);
    }
  }

  const char** SlotFor(const Location& loc) {
    if (loc.IsRegister()) {
      return &cpu[loc.register_code()];
    } else if (loc.IsFpuRegister()) {
      return &fpu[loc.register_code()];
    } else if (loc.IsStackSlot() || loc.IsDoubleStackSlot() ||
               loc.IsQuadStackSlot()) {
      return &stack[(loc.base_reg() == FPREG ? fp_base : 0) + loc.stack_index()];
    } else {
      OS::PrintErr("Can't find slot for %s\n", loc.ToCString());
      UNREACHABLE();
    }
  }

  void ExecuteInSequence(const MoveSchedule& moves) {
    for (auto& op : moves) {
      switch (op.kind) {
        case MoveOp::Kind::kMove:
          *SlotFor(op.operands.dest()) = ValueOf(op.operands.src());
          break;
        case MoveOp::Kind::kNop:
          UNREACHABLE();
          break;
      }
    }
  }

  void ExecuteInParallel(const GrowableArray<MoveOperands*>& moves) {
    GrowableArray<const char*> values(moves.length());
    for (intptr_t i = 0; i < moves.length(); i++) {
      values.Add(ValueOf(moves[i]->src()));
    }
    for (intptr_t i = 0; i < moves.length(); i++) {
      *SlotFor(moves[i]->dest()) = values[i];
    }
  }

  static bool Equal(const char* a, const char* b) { return strcmp(a, b) == 0; }

  bool CheckEqual(const AbstractMachineState& other,
                  const RegisterSet& temporaries) {
    bool ok = true;
    for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
      if (!temporaries.ContainsRegister(static_cast<Register>(i)) &&
          !Equal(cpu[i], other.cpu[i])) {
        OS::PrintErr("reg %s mismatch: %s expected, got %s\n",
                     RegisterNames::RegisterName(static_cast<Register>(i)),
                     cpu[i], other.cpu[i]);
        ok = false;
      }
    }
    for (intptr_t i = 0; i < kNumberOfFpuRegisters; i++) {
      if (!temporaries.ContainsFpuRegister(static_cast<FpuRegister>(i)) &&
          !Equal(fpu[i], other.fpu[i])) {
        OS::PrintErr(
            "reg %s mismatch: %s expected, got %s\n",
            RegisterNames::FpuRegisterName(static_cast<FpuRegister>(i)), fpu[i],
            other.fpu[i]);
        ok = false;
      }
    }
    for (intptr_t i = 0; i < stack_size; i++) {
      if (!Equal(stack[i], other.stack[i])) {
        OS::PrintErr("stack slot %" Pd " mismatch: %s expected, got %s\n", i,
                     stack[i], other.stack[i]);
        ok = false;
      }
    }
    return ok;
  }
};

void VerifyMoveSchedule(ParallelMoveInstr* instr,
                        intptr_t spill_slot_count,
                        intptr_t additional_spill_slots_required) {
  RegisterSet temporaries;

#if defined(TARGET_ARCH_X64)
  const auto kReservedCpuTemp = TMP;
  const auto kReservedFpuTemp = FpuTMP;
#elif defined(TARGET_ARCH_IA32)
  const auto kReservedCpuTemp = kNoRegister;
  const auto kReservedFpuTemp = FpuTMP;
#elif defined(TARGET_ARCH_ARM64)
  const auto kReservedCpuTemp = TMP;
  const auto kReservedFpuTemp = FpuTMP;
#elif defined(TARGET_ARCH_ARM)
  const auto kReservedCpuTemp = kNoRegister;
  const auto kReservedFpuTemp = kNoFpuRegister;
#else
  const auto kReservedCpuTemp = kNoRegister;
  const auto kReservedFpuTemp = FpuTMP;
#endif

  if (kReservedCpuTemp != kNoRegister) {
    temporaries.Add(Location::RegisterLocation(kReservedCpuTemp));
  }
  if (kReservedFpuTemp != kNoFpuRegister) {
    temporaries.Add(Location::FpuRegisterLocation(kReservedFpuTemp));
  }

  Thread* thread = Thread::Current();

  AbstractMachineState initial(thread->zone(), spill_slot_count + additional_spill_slots_required);
  AbstractMachineState copy(initial);

  copy.ExecuteInParallel(instr->moves());
  initial.ExecuteInSequence(instr->move_schedule());

  for (intptr_t i = 0; i < additional_spill_slots_required; i++) {
    const auto slot_index = spill_slot_count + i;
    const auto stack_index =
        compiler::target::frame_layout.FrameSlotForVariableIndex(-slot_index);
    const auto spill_loc = Location::StackSlot(stack_index, FPREG);
    *initial.SlotFor(spill_loc) = *copy.SlotFor(spill_loc);
  }

  if (!copy.CheckEqual(initial, temporaries)) {
    copy.Print();
    initial.Print();

    OS::PrintErr("Incorrect schedule is generated for %s\n", instr->ToCString());
    UNREACHABLE();
  }
}
}  // namespace

ParallelMoveResolver::ParallelMoveResolver(bool is_intrinsic,
                                           intptr_t spill_slot_count)
    : is_intrinsic_(is_intrinsic),
      spill_slot_count_(spill_slot_count),
      moves_(32) {}

void ParallelMoveResolver::Resolve(ParallelMoveInstr* parallel_move) {
  ASSERT(moves_.is_empty());

  parallel_move_ = parallel_move;
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
      scheduled_ops_.Add({MoveOp::Kind::kMove, move});
    }
  }
  moves_.Clear();

  LegalizeMoves();

  AllocateTemporaries();

  // Schedule is ready. Update parallel move itself.
  parallel_move->set_move_schedule(MoveSchedule::From(scheduled_ops_));
  scheduled_ops_.Clear();
  parallel_move_ = nullptr;

  VerifyMoveSchedule(parallel_move, spill_slot_count_,
                     additional_spill_slots_required_);
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
  // we have a cycle.  Search for such a blocking move and break the cycle
  // by emitting a move to temporary.
  for (intptr_t i = 0; i < moves_.length(); i++) {
    auto& other_move = moves_[i];
    if (other_move.Blocks(destination)) {
      const auto tmp = CreateTemporary(destination.IsDoubleStackSlot() ||
                                               destination.IsQuadStackSlot() ||
                                               destination.IsFpuRegister()
                                           ? Location::kFpuRegister
                                           : Location::kRegister);
      scheduled_ops_.Add({MoveOp::Kind::kMove, {tmp, destination}});
      other_move.set_src(tmp);
      break;
    }
  }

  // This move is not blocked.
  AddMoveToSchedule(index);
}

void ParallelMoveResolver::AddMoveToSchedule(int index) {
  auto& move = moves_[index];
  if (!move.IsRedundant()) {
    scheduled_ops_.Add({MoveOp::Kind::kMove, move});
  }
  move.Eliminate();
}

Location ParallelMoveResolver::CreateTemporary(Location::Kind kind) {
  static_assert(kNumberOfFpuRegisters <= kNumberOfCpuRegisters);
  temporaries_.Add(Location());
  return Location(kind, kNumberOfCpuRegisters + temporaries_.length() - 1);
}

void ParallelMoveResolver::LegalizeMoves() {
  for (intptr_t i = 0; i < scheduled_ops_.length(); ++i) {
    const auto& op = scheduled_ops_[i];
    if (op.kind == MoveOp::Kind::kMove) {
      if (ParallelMoveEmitter::RequiresTemporary(op.operands.dest(),
                                                 op.operands.src())) {
        auto src = op.operands.src();
        auto dst = op.operands.dest();
        const auto need_fpu_temp =
            dst.IsDoubleStackSlot() || dst.IsQuadStackSlot();

        const auto temp = CreateTemporary(need_fpu_temp ? Location::kFpuRegister
                                                        : Location::kRegister);
        scheduled_ops_[i] = {MoveOp::Kind::kMove, {temp, src}};
        scheduled_ops_.InsertAt(i + 1, {MoveOp::Kind::kMove, {dst, temp}});
        i++;
      }
    }
  }
}

namespace {

#if 0
const char* MoveToCString(const MoveOp& op) {
  switch (op.kind) {
    case MoveOp::Kind::kMove:
      return OS::SCreate(Thread::Current()->zone(), "move(%s <- %s)",
                         op.operands.dest().ToCString(),
                         op.operands.src().ToCString());
    case MoveOp::Kind::kNop:
      return "nop";
  }
}
#endif

// TODO(XXX) when allocating a register look backwards and search for a possible
// use of form mem <- R. This is essentially a spilling move.

class RegisterAllocator {
  static constexpr intptr_t kMaxNumberOfRegisters =
      Utils::Maximum<intptr_t>(kNumberOfCpuRegisters, kNumberOfFpuRegisters);
  static_assert(kMaxNumberOfRegisters == kNumberOfCpuRegisters);

 public:
  RegisterAllocator(
      Location::Kind kind,
      GrowableArray<intptr_t>* def_pos,
      intptr_t* spill_slot_count,
      GrowableArray<std::pair<intptr_t, MoveOperands>>* spill_moves)
      : kind(kind),
        max_registers(kind == Location::kRegister ? kNumberOfCpuRegisters
                                                  : kNumberOfFpuRegisters),
        spill_slot_size(kind == Location::kRegister
                            ? 1
                            : (kFpuRegisterSize / target::kWordSize)),
        spill_slot_count(spill_slot_count),
        spill_moves(*spill_moves),
        def_pos(*def_pos),
        contents(def_pos->length() + kMaxNumberOfRegisters) {
    RELEASE_ASSERT(kind == Location::kRegister ||
                   kind == Location::kFpuRegister);
    last_use.fill(-1);
    contents.EnsureLength(def_pos->length() + kMaxNumberOfRegisters,
                          Location());
    spill_slot.EnsureLength(def_pos->length() + kMaxNumberOfRegisters,
                            Location());
  }

  void Init() {
    for (intptr_t r = 0; r < max_registers; r++) {
      if (!clobbered.Contains(r)) {
        contents[r] = Location::MachineRegisterLocation(kind, r);
      }
    }
  }

  inline bool IsRegister(const Location& loc) {
    return loc.kind() == kind && loc.register_code() < kMaxNumberOfRegisters;
  }

  inline bool IsTemporary(const Location& loc) {
    return loc.kind() == kind && loc.register_code() >= kMaxNumberOfRegisters;
  }

  static inline intptr_t TemporaryIndex(const Location& loc) {
    return loc.register_code() - kMaxNumberOfRegisters;
  }

  void ComputeLiveRanges(const GrowableArray<MoveOp>& scheduled_ops) {
    const auto use = [&](const Location& loc, intptr_t pos) {
      if (IsRegister(loc)) {
        last_use[loc.register_code()] = pos;
      }
    };

    const auto def = [&](const Location& loc, intptr_t pos) {
      if (IsTemporary(loc)) {
        def_pos[TemporaryIndex(loc)] = pos;
      }
    };

    for (intptr_t pos = 0; pos < scheduled_ops.length(); pos++) {
      const auto& op = scheduled_ops[pos];
      switch (op.kind) {
        case MoveOp::Kind::kMove:
          use(op.operands.src(), pos);
          def(op.operands.dest(), pos);
          if (op.operands.dest().HasStackIndex() &&
              op.operands.src().kind() == kind &&
              !IsTemporary(
                  op.operands
                      .src())) {  // TODO(XXX) review. consider renaming the getter
            AssociateSpillSlot(op.operands.src().register_code(),
                               op.operands.dest());
          }
          break;
        case MoveOp::Kind::kNop:
          break;
      }
    }
  }

  void AssociateSpillSlot(intptr_t reg, Location loc) {
    if (spill_slot[reg].IsInvalid()) {
      spill_slot[reg] = loc;
    }
  }

  void Allocate(GrowableArray<MoveOp>& scheduled_ops) {
    const auto def = [&](intptr_t pos, Location& loc, const Location& src) {
      if (loc.kind() == kind) {
        ProcessDef(pos, loc, /*can_fuse=*/src.IsRegister());
      }
    };

    const auto use = [&](intptr_t pos, Location& loc) {
      if (loc.kind() == kind) {
        ProcessUse(pos, loc);
      }
    };

    for (intptr_t pos = scheduled_ops.length() - 1; pos >= 0; pos--) {
      auto& op = scheduled_ops[pos];
      switch (op.kind) {
        case MoveOp::Kind::kMove:
          TRACE_ALLOC("@%" Pd " %s\n", pos, MoveToCString(op));
          def(pos, *op.operands.dest_slot(), op.operands.src());
          use(pos, *op.operands.src_slot());
          if (op.operands.src().kind() == kind &&
              spill_slot.length() > op.operands.src().register_code() &&
              spill_slot[op.operands.src().register_code()].Equals(
                  op.operands.dest())) {
            spill_slot[op.operands.src().register_code()] = Location();
          }
          break;
        case MoveOp::Kind::kNop:
          break;
      }
    }

    // Flush spilled registers which are not defined by the move itself.
    for (intptr_t r = 0; r < Utils::Minimum(max_registers, spill_slot.length());
         r++) {
      if (!spill_slot[r].IsInvalid()) {
        // Emit a move that stores this register into the spill slot.
        AddMoveAfter(-1, spill_slot[r],
                     Location::MachineRegisterLocation(kind, r));
      }
    }
  }

  static inline uint64_t RegMask(intptr_t index) {
    return static_cast<uint64_t>(1) << index;
  }

  const char* RegisterName(intptr_t reg) {
    return kind == Location::kRegister
               ? RegisterNames::RegisterName(static_cast<Register>(reg))
               : RegisterNames::FpuRegisterName(static_cast<FpuRegister>(reg));
  }

  const char* RegisterSetToCString(uint64_t bits) {
    ZoneTextBuffer printer(Thread::Current()->zone());
    SmallSet<intptr_t> set(bits);
    bool comma = false;
    printer.AddChar('{');
    for (intptr_t i = 0; i < kMaxNumberOfRegisters; i++) {
      if (set.Contains(i)) {
        if (comma) printer.AddString(", ");
        printer.AddString(RegisterName(i));
        comma = true;
      }
    }
    printer.AddChar('}');
    return printer.buffer();
  }

  Location AllocateRegister(intptr_t def_pos, intptr_t pos) {
    SmallSet<intptr_t> candidates(clobbered.data() & available.data());

    TRACE_ALLOC("|  @%" Pd " allocate register for def @%" Pd "\n", pos,
                def_pos);

    // Ideally we would like to select a register which is free between
    // the current position and the definition of the temporary because
    // this would minimize amount of spill/restore code we need to emit.
    for (intptr_t r = 0; r < max_registers; r++) {
      if (last_use[r] >= def_pos) {
        if (candidates.Contains(r)) {
          TRACE_ALLOC("|  |  possible candidate %s not suitable: last_use @%" Pd
                      "\n",
                      RegisterName(r), last_use[r]);
        }
        candidates.Remove(r);
      }
    }
    TRACE_ALLOC("|  |  candidates=%s (clobbered=%s)\n",
                RegisterSetToCString(candidates.data()),
                RegisterSetToCString(clobbered.data()));

    // Add at least one bit into |candidates| to prevent it from being 0.
    candidates.Add(kMaxNumberOfRegisters);

    auto reg = Utils::CountTrailingZeros64(candidates.data());
    if (reg == kMaxNumberOfRegisters) {
      // No free CPU register - everything is blocked. Check if there is a
      // suitable spill candidate which can be used here to become a temporary.

      // First check among registers which are already spilled and don't have
      // uses that interfer with liveness of this temporary. For such registers
      // we only need to emit a single move to restore them.
      for (intptr_t r = 0; r < max_registers; r++) {
        if (!available.Contains(r)) {
          continue;
        }
        // Do not evict currently active temporaries.
        if (contents[r].kind() == kind && IsTemporary(contents[r])) {
          continue;
        }

        if (!spill_slot[r].IsInvalid() && last_use[r] < def_pos) {
          reg = r;
        }
      }

      if (reg == kMaxNumberOfRegisters) {
        // Could not find register which is already spilled, which could be
        // used. In this case just choose and evict some register.
        intptr_t reg_last_use = -1;
        bool reg_spilled = false;
        for (intptr_t r = 0; r < max_registers; r++) {
          if (!available.Contains(r)) {
            continue;
          }
          // Do not evict currently active temporaries.
          if (contents[r].kind() == kind && IsTemporary(contents[r])) {
            continue;
          }
          const bool r_spilled = !spill_slot[r].IsInvalid();
          if (reg == kMaxNumberOfRegisters || (last_use[r] < reg_last_use) ||
              (r_spilled && !reg_spilled)) {
            reg = r;
            reg_last_use = last_use[r];
            reg_spilled = r_spilled;
          }
        }
      }

      RELEASE_ASSERT(reg != kMaxNumberOfRegisters);
      RestoreAfter(pos, reg);
    }
    clobbered.Remove(reg);
    return Location::MachineRegisterLocation(kind, reg);
  }

  void AllocateTemporary(const Location& loc, intptr_t pos) {
    RELEASE_ASSERT(IsTemporary(loc));
    const auto reg = AllocateRegister(def_pos[TemporaryIndex(loc)], pos);

    contents[loc.register_code()] = reg;
    contents[reg.register_code()] = loc;
  }

  void ProcessUse(intptr_t pos, Location& loc) {
    const intptr_t reg = loc.register_code();

    if (IsTemporary(loc)) {
      // Allocate this temporary if it is not allocated yet.
      if (contents[reg].IsInvalid()) {
        AllocateTemporary(loc, pos);
      } else {
        RELEASE_ASSERT(contents[contents[reg].register_code()].Equals(loc));
      }

      // Update the use to point to the location containing the temporary.
      loc = contents[reg];
    } else {
      clobbered.Remove(reg);
      if (!contents[reg].Equals(loc)) {
        RestoreAfter(pos, reg);
      }
      contents[reg] = loc;
    }
  }

  void ProcessDef(intptr_t pos, Location& loc, bool can_fuse) {
    const auto reg = loc.register_code();

    bool is_spilled = false;
    if (IsTemporary(loc)) {
      if (!contents[reg].IsInvalid()) {
        loc = contents[reg];
      } else {
        is_spilled = true;
      }
    } else {
      if (!contents[reg].Equals(loc)) {
        is_spilled = true;
      }
    }

    if (is_spilled) {
      if (can_fuse) {
        loc = spill_slot[reg];
      } else {
        loc = AllocateRegister(pos, pos);
        AddMoveAfter(pos, spill_slot[reg], loc);
      }
      spill_slot[reg] = Location();
    }

    if (loc.kind() == kind) {
      contents[loc.register_code()] = Location();
      clobbered.Add(loc.register_code());
    }
  }

  void AddMoveAfter(intptr_t pos, Location dst, Location src) {
    spill_moves.Add({pos, {dst, src}});
  }

  void RestoreAfter(intptr_t pos, intptr_t machine_reg) {
    if (contents[machine_reg].IsInvalid()) {
      // Nothing to do. The location is not currently used.
      return;
    }

    // We expect that the current value of the register is either
    // its original value or a temporary.
    const intptr_t current_value = contents[machine_reg].register_code();
    if (current_value >= kMaxNumberOfRegisters) {
      // Evict the temporary allocated into this register.
      contents[current_value] = Location();
    } else {
      RELEASE_ASSERT(current_value == machine_reg);
    }

    // Allocate spill slot for the current value if necessary.
    if (spill_slot[current_value].IsInvalid()) {
      // We need to allocate a spill slot.
      // TODO(XXX) handle Double and Quads if they are possible, consider
      // having register allocator telling us whether Q is a possibility.
      const intptr_t spill_slot_index = *spill_slot_count + spill_slot_size - 1;
      *spill_slot_count += spill_slot_size;
      const intptr_t slot_index =
          compiler::target::frame_layout.FrameSlotForVariableIndex(
              -spill_slot_index);
      spill_slot[current_value] =
          kind == Location::kRegister
              ? Location::StackSlot(slot_index, FPREG)
              : Location::DoubleStackSlot(slot_index, FPREG);
    }

    // Add restore move from the spill slot.
    AddMoveAfter(pos, Location::MachineRegisterLocation(kind, machine_reg),
                 spill_slot[current_value]);
  }

  // Register kind handled by this allocator (kRegister or kFpuRegister).
  const Location::Kind kind;

  // Maximum number of registers of the given |kind|.
  const intptr_t max_registers;

  // Spill slot size for the given type of the register.
  const intptr_t spill_slot_size;

  // Spill slot count.
  intptr_t* const spill_slot_count;

  // Additional moves emitted by the allocator to spill/restore registers.
  GrowableArray<std::pair<intptr_t, MoveOperands>>& spill_moves;

  // Locations assinged to different temporaries (indexed by temp_index).
  GrowableArray<intptr_t>& def_pos;

  // Registers available for allocation.
  SmallSet<intptr_t> available;

  // Set of registers which are clobbered by subsequent moves. Such registers
  // can be used as temporaries if needed.
  SmallSet<intptr_t> clobbered;

  // Current state of the assigned locations for each register (both machine
  // register and virtual temporary registers).
  GrowableArray<Location> contents;

  // Current spill slot containing corresponding register value (both machine
  // registers and virtual temporary registers).
  GrowableArray<Location> spill_slot;

  std::array<intptr_t, kMaxNumberOfRegisters> last_use;
};

static bool IsTemporary(intptr_t register_code) {
  return register_code >= kNumberOfCpuRegisters;
}

static bool IsTemporary(const Location& loc) {
  return loc.IsMachineRegister() && IsTemporary(loc.register_code());
}

static void RescheduleMoves(GrowableArray<MoveOp>& ops) {
  // Check if we can reschedule any moves which store into a register
  // to create a temporary register.
  for (intptr_t i = 0, length = ops.length(); i < length; i++) {
    const auto& candidate = ops[i];
    const auto dst = candidate.operands.dest();
    const auto src = candidate.operands.src();
    if (candidate.kind == MoveOp::Kind::kMove) {
      if (dst.IsMachineRegister() && !IsTemporary(dst) && !IsTemporary(src)) {
        // If this is R <- ? move where neither destination, nor source are
        // temporaries. Try moving it to the very end of the move sequence
        // (given that there are no interfering moves) - if this moves
        // the move past any move that defines a temporary then it will create
        // a valid register to be used as temporary.
        intptr_t j;
        bool encountered_any_temporaries = false;
        for (j = i + 1; j < ops.length(); j++) {
          const auto& other_move = ops[j];
          if (other_move.kind == MoveOp::Kind::kMove) {
            // Check if this moves destroys the candidate's source.
            if (other_move.operands.dest().Equals(src)) {
              break;
            }
            if (IsTemporary(other_move.operands.dest()) &&
                other_move.operands.dest().kind() == dst.kind()) {
              encountered_any_temporaries = true;
            }
          }
        }
        if (encountered_any_temporaries) {
          RELEASE_ASSERT(j > (i + 1));
          RELEASE_ASSERT(j <= ops.length());
          for (intptr_t k = i; k < (j - 1); k++) {
            ops[k] = ops[k + 1];
          }
          ops[j - 1] = {MoveOp::Kind::kMove, {dst, src}};
        }
      } else if (i > 0 && src.IsMachineRegister() && !IsTemporary(src) &&
                 (dst.IsStackSlot() || dst.IsDoubleStackSlot())) {
        // Check if this is a S <- R move. We can attempt to shift this
        // move to the start of the move sequence to create opportunities
        // for using such register as a temporary.

        intptr_t blocked_at;
        for (blocked_at = i - 1; blocked_at >= 0; blocked_at--) {
          const auto& other_move = ops[blocked_at];
          if (other_move.kind == MoveOp::Kind::kMove) {
            // Can't schedule past this move.
            if (other_move.operands.src().Equals(dst)) {
              break;
            }
          }
        }

        if (blocked_at < (i - 1)) {  //
          for (intptr_t k = i; k > (blocked_at + 1); k--) {
            ops[k] = ops[k - 1];
          }
          ops[blocked_at + 1] = {MoveOp::Kind::kMove, {dst, src}};
        }
      }
    }
  }
}

}  // namespace

void ParallelMoveResolver::AllocateTemporaries() {
  if (temporaries_.is_empty()) {
    return;
  }

  RescheduleMoves(scheduled_ops_);

  const intptr_t kAllFpuRegistersList =
      (static_cast<intptr_t>(1) << kNumberOfFpuRegisters) - 1;

  intptr_t spill_slot_count = spill_slot_count_;
  GrowableArray<std::pair<intptr_t, MoveOperands>> spill_moves;

  GrowableArray<intptr_t> def_pos(temporaries_.length());
  def_pos.EnsureLength(temporaries_.length(), -1);

  std::array<RegisterAllocator, 2> allocators = {
      RegisterAllocator(Location::kRegister, &def_pos, &spill_slot_count,
                        &spill_moves),
      RegisterAllocator(Location::kFpuRegister, &def_pos, &spill_slot_count,
                        &spill_moves),
  };

  auto& cpu_reg_allocator = allocators[0];
  auto& fpu_reg_allocator = allocators[1];

#if defined(TARGET_ARCH_X64)
  const auto kReservedCpuTemp = TMP;
  const auto kReservedFpuTemp = FpuTMP;
#elif defined(TARGET_ARCH_IA32)
  const auto kReservedCpuTemp = kNoRegister;
  const auto kReservedFpuTemp = FpuTMP;
#elif defined(TARGET_ARCH_ARM64)
  const auto kReservedCpuTemp = TMP;
  const auto kReservedFpuTemp = FpuTMP;
#elif defined(TARGET_ARCH_ARM)
  const auto kReservedCpuTemp = kNoRegister;
  const auto kReservedFpuTemp = kNoFpuRegister;
#else
  const auto kReservedCpuTemp = kNoRegister;
  const auto kReservedFpuTemp = FpuTMP;
#endif

  // Compute registers which can be used for allocation.
  cpu_reg_allocator.available =
      SmallSet<intptr_t>(~kReservedCpuRegisters & kAllCpuRegistersList);
  if (is_intrinsic_) {
    // Block additional registers that must be preserved for intrinsics.
    cpu_reg_allocator.available.Remove(ARGS_DESC_REG);
#if !defined(TARGET_ARCH_IA32)
    // Need to preserve CODE_REG to be able to store the PC marker
    // and load the pool pointer.
    cpu_reg_allocator.available.Remove(CODE_REG);
#endif
  }
  if (kReservedCpuTemp != kNoRegister) {
    cpu_reg_allocator.available.Add(kReservedCpuTemp);
    cpu_reg_allocator.clobbered.Add(kReservedCpuTemp);
  }

  fpu_reg_allocator.available = SmallSet<intptr_t>(kAllFpuRegistersList);
  if (kReservedFpuTemp != kNoFpuRegister) {
    fpu_reg_allocator.available.Add(kReservedFpuTemp);
    fpu_reg_allocator.clobbered.Add(kReservedFpuTemp);
  }

  for (auto& allocator : allocators) {
    TRACE_ALLOC("allocating %s registers\n",
                allocator.kind == Location::kRegister ? "CPU" : "FPU");
    allocator.Init();
    allocator.ComputeLiveRanges(scheduled_ops_);
    allocator.Allocate(scheduled_ops_);
  }

  GrowableArray<MoveOp> ops;

  const auto emit_spill_moves_before = [&](intptr_t pos) {
    while (!spill_moves.is_empty() && spill_moves.Last().first < pos) {
      ops.Add({MoveOp::Kind::kMove, spill_moves.RemoveLast().second});
    }
  };

  for (intptr_t pos = 0; pos < scheduled_ops_.length(); pos++) {
    emit_spill_moves_before(pos);
    const auto& op = scheduled_ops_[pos];
    switch (op.kind) {
      case MoveOp::Kind::kMove:
        if (!op.operands.IsRedundant()) {
          ops.Add(op);
        }
        break;
      case MoveOp::Kind::kNop:
        break;
    }
  }
  emit_spill_moves_before(scheduled_ops_.length());

  scheduled_ops_.Clear();
  scheduled_ops_.AddArray(ops);

  additional_spill_slots_required_ = Utils::Maximum(
      additional_spill_slots_required_, spill_slot_count - spill_slot_count_);
}

}  // namespace compiler
}  // namespace dart
