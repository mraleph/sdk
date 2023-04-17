// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
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
#include "vm/compiler/backend/parallel_move_schedule.h"
#include "vm/constants.h"

namespace dart {

class MoveOperands;

namespace compiler {

class ParallelMoveResolver : public ValueObject {
 public:
  ParallelMoveResolver(bool is_intrinsic, intptr_t spill_slot_count);

  // Schedule moves specified by the given parallel move and store the
  // schedule on the parallel move itself.
  void Resolve(ParallelMoveInstr* parallel_move);

  intptr_t additional_spill_slots_required() const {
    return additional_spill_slots_required_;
  }

 private:
  // Build the initial list of moves.
  void BuildInitialMoveList(ParallelMoveInstr* parallel_move);

  // Perform the move at the moves_ index in question (possibly requiring
  // other moves to satisfy dependencies).
  void PerformMove(const InstructionSource& source, int index);

  // Schedule a move and remove it from the move graph.
  void AddMoveToSchedule(int index);

  // Lower high-level moves into moves supported by the underlying
  // platform.
  void LegalizeMoves();

  Location CreateTemporary(Location::Kind kind);

  void AllocateTemporaries();

  const bool is_intrinsic_;

  const intptr_t spill_slot_count_;
  intptr_t additional_spill_slots_required_ = 0;

  ParallelMoveInstr* parallel_move_ = nullptr;

  // List of moves not yet resolved.
  GrowableArray<MoveOperands> moves_;
  GrowableArray<bool> is_pending_;

  GrowableArray<MoveOp> scheduled_ops_;
  GrowableArray<Location> temporaries_;

  friend class MoveSchedule;
  friend class ParallelMoveEmitter;
  friend class FlowGraphDeserializer;
};

}  // namespace compiler
}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_RESOLVER_H_
