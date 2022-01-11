// Copyright (c) 2019, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// Unit tests specific to BCE (bounds check eliminaton),
// which runs as part of range analysis optimizations.

#include "vm/compiler/backend/parallel_move_resolver.h"

#include "vm/unit_test.h"

namespace dart {

ISOLATE_UNIT_TEST_CASE(ParallelMove_TwoCycles) {
  auto r = [](intptr_t reg) {
    return Location::MachineRegisterLocation(Location::kRegister, reg);
  };

  auto s = [](intptr_t index) { return Location::StackSlot(index, FPREG); };

  auto run_test =
      [&](std::initializer_list<std::pair<Location, Location>> moves) {
        ParallelMoveInstr* instr = new ParallelMoveInstr();

        for (const auto& move : moves) {
          instr->AddMove(move.first, move.second);
        }

        OS::PrintErr("before scheduling: %s\n", instr->ToCString());
        ParallelMoveResolver(/*is_intrinsic=*/false, /*has_frame=*/true,
                             /*spill_slot_count=*/0)
            .Resolve(instr);
        OS::PrintErr("after scheduling: %s\n", instr->ToCString());
      };

  run_test({
      {r(1), r(0)},
      {r(3), r(2)},
      {r(2), r(3)},
      {r(0), r(1)},
  });

  run_test({
      {r(0), s(-3)},
      {s(-1), s(-2)},
      {s(-2), s(-1)},
  });

  run_test({
      {s(-1), s(-2)},
      {s(-2), s(-1)},
      {s(-3), r(0)},
  });

  run_test({
      {s(-1), s(-2)},
      {s(-2), s(-1)},
      {s(-4), r(0)},
      {s(-3), r(0)},
  });

  run_test({
      {s(-1), s(-2)},
      {s(-2), s(-1)},
  });
}

}  // namespace dart
