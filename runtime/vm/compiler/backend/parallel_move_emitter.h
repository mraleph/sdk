// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_EMITTER_H_
#define RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_EMITTER_H_

#if defined(DART_PRECOMPILED_RUNTIME)
#error "AOT runtime should not use compiler sources (including header files)"
#endif  // defined(DART_PRECOMPILED_RUNTIME)

#include "vm/allocation.h"
#include "vm/compiler/backend/flow_graph_compiler.h"
#include "vm/compiler/backend/locations.h"
#include "vm/constants.h"

namespace dart {
namespace compiler {

class ParallelMoveEmitter : public ValueObject {
 public:
  ParallelMoveEmitter(FlowGraphCompiler* compiler,
                      ParallelMoveInstr* parallel_move)
      : compiler_(compiler), parallel_move_(parallel_move) {}

  void EmitNativeCode();

  static bool RequiresTemporary(Location dst, Location src);

 private:
  class ScratchFpuRegisterScope : public ValueObject {
   public:
    ScratchFpuRegisterScope(ParallelMoveEmitter* emitter, FpuRegister blocked);
    ~ScratchFpuRegisterScope();

    FpuRegister reg() const { return reg_; }

   private:
    ParallelMoveEmitter* const emitter_;
    FpuRegister reg_;
    bool spilled_;
  };

  class TemporaryAllocator : public TemporaryRegisterAllocator {
   public:
    TemporaryAllocator(ParallelMoveEmitter* emitter, Register blocked);

    Register AllocateTemporary() override;
    void ReleaseTemporary() override;
    DEBUG_ONLY(bool DidAllocateTemporary() { return allocated_; })

    virtual ~TemporaryAllocator() { ASSERT(reg_ == kNoRegister); }

   private:
    ParallelMoveEmitter* const emitter_;
    const Register blocked_;
    Register reg_;
    bool spilled_;
    DEBUG_ONLY(bool allocated_ = false);
  };

  class ScratchRegisterScope : public ValueObject {
   public:
    ScratchRegisterScope(ParallelMoveEmitter* emitter, Register blocked);
    ~ScratchRegisterScope();

    Register reg() const { return reg_; }

   private:
    TemporaryAllocator allocator_;
    Register reg_;
  };

  bool IsScratchLocation(Location loc);
  intptr_t AllocateScratchRegister(Location::Kind kind,
                                   uword blocked_mask,
                                   intptr_t first_free_register,
                                   intptr_t last_free_register,
                                   bool* spilled);

  void SpillScratch(Register reg);
  void RestoreScratch(Register reg);
  void SpillFpuScratch(FpuRegister reg);
  void RestoreFpuScratch(FpuRegister reg);

  // Generate the code for a move from source to destination.
  void EmitMove(const MoveOperands& move);

  void EmitSwap(const MoveOperands& swap);

  // Verify the move list before performing moves.
  void Verify();

  // Helpers for non-trivial source-destination combinations that cannot
  // be handled by a single instruction.
  void EmitMove(Location destination, Location source);

  void MoveMemoryToMemory(const compiler::Address& dst,
                          const compiler::Address& src);
  void Exchange(Register reg, const compiler::Address& mem);
  void Exchange(const compiler::Address& mem1, const compiler::Address& mem2);
  void Exchange(Register reg, Register base_reg, intptr_t stack_offset);
  void Exchange(Register base_reg1,
                intptr_t stack_offset1,
                Register base_reg2,
                intptr_t stack_offset2);

  FlowGraphCompiler* const compiler_;
  ParallelMoveInstr* parallel_move_;
  intptr_t current_move_;
};

}  // namespace compiler
}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_EMITTER_H_
