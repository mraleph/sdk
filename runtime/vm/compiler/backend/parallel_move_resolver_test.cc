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
  ParallelMoveInstr* instr = new ParallelMoveInstr();
  auto r = [](intptr_t reg) {
    return Location::MachineRegisterLocation(Location::kRegister, reg);
  };

  instr->AddMove(r(1), r(0));
  instr->AddMove(r(3), r(2));
  instr->AddMove(r(2), r(3));
  instr->AddMove(r(0), r(1));

  OS::PrintErr("before scheduling: %s\n", instr->ToCString());
  ParallelMoveResolver().Resolve(instr);
  OS::PrintErr("after scheduling: %s\n", instr->ToCString());
}


}  // namespace dart
