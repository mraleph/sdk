// Copyright (c) 2022, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_RESOLVER_H_
#define RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_RESOLVER_H_

#if defined(DART_PRECOMPILED_RUNTIME)
#error "AOT runtime should not use compiler sources (including header files)"
#endif  // defined(DART_PRECOMPILED_RUNTIME)

#include "vm/allocation.h"
#include "vm/compiler/backend/flow_graph_compiler.h"
#include "vm/compiler/backend/locations.h"
#include "vm/constants.h"

namespace dart {

class BaseTextBuffer;
class MoveOperands;

class ParallelMoveResolver : public ValueObject {
 public:
  ParallelMoveResolver(bool is_intrinsic, intptr_t spill_slot_count);

  // Schedule moves specified by the given parallel move and store the
  // schedule on the parallel move itself.
  void Resolve(ParallelMoveInstr* parallel_move);

  intptr_t additional_spill_slots_required() const {
    return additional_spill_slots_required_;
  }

  static void PrintScheduleTo(const MoveSchedule& schedule, BaseTextBuffer* f);
  static void CopyScheduleTo(const MoveSchedule& schedule, GrowableArray<MoveOperands>* moves);

 private:
  // Build the initial list of moves.
  void BuildInitialMoveList(ParallelMoveInstr* parallel_move);

  // Perform the move at the moves_ index in question (possibly requiring
  // other moves to satisfy dependencies).
  void PerformMove(const InstructionSource& source, int index);

  // Schedule a move and remove it from the move graph.
  void AddMoveToSchedule(int index);

  // Schedule a swap of two operands. The move from
  // source to destination is removed from the move graph.
  void AddSwapToSchedule(int index);

  void LegalizeMoves();

  void AllocateTemporaries(ParallelMoveInstr* parallel_move);

  Location CreateTemporary(Location::Kind kind) {
    temporaries_.Add(Location());
    return Location(kind, kNumberOfCpuRegisters + temporaries_.length() - 1);
  }

  const bool is_intrinsic_;
  const intptr_t spill_slot_count_;
  intptr_t additional_spill_slots_required_ = 0;

  // List of moves not yet resolved.
  GrowableArray<MoveOperands> moves_;

  enum class OpKind {
    kNop,
    kMove,
  };

  struct Op {
    OpKind kind;
    MoveOperands operands;
    Location temp = Location();
  };

  GrowableArray<Location> temporaries_;

  GrowableArray<Op> scheduled_ops_;

  friend class MoveSchedule;
  friend class ParallelMoveEmitter;
};

class ParallelMoveEmitter : public ValueObject {
 public:
  ParallelMoveEmitter(FlowGraphCompiler* compiler,
                      ParallelMoveInstr* parallel_move)
      : compiler_(compiler), parallel_move_(parallel_move) {}

  void EmitNativeCode();

 private:
  // Generate the code for a move from source to destination.
  void EmitMove(const ParallelMoveResolver::Op& move);

  // Verify the move list before performing moves.
  void Verify();

  FlowGraphCompiler* compiler_;
  ParallelMoveInstr* parallel_move_;
};

}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_RESOLVER_H_
