// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/backend/parallel_move_resolver.h"

namespace dart {
namespace compiler {

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
      scheduled_ops_.Add({MoveOp::Kind::kMove, move});
    }
  }
  moves_.Clear();

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
  scheduled_ops_.Add({MoveOp::Kind::kMove, move});
  move.Eliminate();
}

void ParallelMoveResolver::AddSwapToSchedule(int index) {
  auto& move = moves_[index];
  const auto source = move.src();
  const auto destination = move.dest();

  scheduled_ops_.Add({MoveOp::Kind::kSwap, move});

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

}  // namespace compiler
}  // namespace dart
