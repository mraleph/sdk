// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/backend/parallel_move_emitter.h"
#include "vm/compiler/backend/parallel_move_schedule.h"

namespace dart {
namespace compiler {

namespace {
uword RegMaskBit(Register reg) {
  return ((reg) != kNoRegister) ? (1 << (reg)) : 0;
}
}  // namespace

void ParallelMoveEmitter::EmitNativeCode() {
  const auto& move_schedule = parallel_move_->move_schedule();
  for (intptr_t i = 0; i < move_schedule.length(); i++) {
    current_move_ = i;
    const auto& op = move_schedule[i];
    switch (op.kind) {
      case MoveOp::Kind::kNop:
        // |MoveSchedule::From| is expected to filter nops.
        UNREACHABLE();
        break;
      case MoveOp::Kind::kMove:
        EmitMove(op.operands);
        break;
    }
  }
}

void ParallelMoveEmitter::EmitMove(const MoveOperands& move) {
  const Location src = move.src();
  const Location dst = move.dest();
  EmitMove(dst, src);
}

bool ParallelMoveEmitter::IsScratchLocation(Location loc) {
  const auto& move_schedule = parallel_move_->move_schedule();
  for (intptr_t i = current_move_; i < move_schedule.length(); i++) {
    const auto& op = move_schedule[i];
    if (op.operands.src().Equals(loc)) {
      return false;
    }
  }

  for (intptr_t i = current_move_ + 1; i < move_schedule.length(); i++) {
    const auto& op = move_schedule[i];
    if (op.kind == MoveOp::Kind::kMove && op.operands.dest().Equals(loc)) {
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

}  // namespace compiler

}  // namespace dart
