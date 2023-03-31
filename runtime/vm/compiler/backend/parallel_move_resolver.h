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
  ParallelMoveResolver();

  // Schedule moves specified by the given parallel move and store the
  // schedule on the parallel move itself.
  void Resolve(ParallelMoveInstr* parallel_move);

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

  FlowGraphCompiler* compiler_;

  // List of moves not yet resolved.
  GrowableArray<MoveOperands> moves_;

  GrowableArray<MoveOp> scheduled_ops_;

  friend class MoveSchedule;
  friend class ParallelMoveEmitter;
  friend class FlowGraphDeserializer;
};

}  // namespace compiler
}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_RESOLVER_H_
