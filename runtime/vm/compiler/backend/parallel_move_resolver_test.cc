// Copyright (c) 2019, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// Unit tests specific to BCE (bounds check eliminaton),
// which runs as part of range analysis optimizations.

#include "vm/compiler/backend/parallel_move_resolver.h"

#include "vm/unit_test.h"

namespace dart {
namespace compiler {

struct AbstractState {
  static constexpr intptr_t kStackSize = 100;

  const char* cpu[kNumberOfCpuRegisters];
  const char* fpu[kNumberOfFpuRegisters];
  const char* stack[kStackSize];

  explicit AbstractState(Zone* zone) {
    for (intptr_t i = 0; i < kNumberOfCpuRegisters; i++) {
      cpu[i] = RegisterNames::RegisterName(static_cast<Register>(i));
    }
    for (intptr_t i = 0; i < kNumberOfFpuRegisters; i++) {
      fpu[i] = RegisterNames::FpuRegisterName(static_cast<FpuRegister>(i));
    }
    for (intptr_t i = 0; i < kStackSize; i++) {
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
    //for (intptr_t i = 0; i < kStackSize; i++) {
    //  OS::PrintErr("  S(%" Pd "): %s\n", i, stack[i]);
    //}
    OS::PrintErr("}\n");
  }

  AbstractState(const AbstractState& other) = default;

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
      return &stack[(loc.base_reg() == FPREG ? 50 : 0) + loc.stack_index()];
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

  bool CheckEqual(const AbstractState& other, const RegisterSet& temporaries) {
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
    for (intptr_t i = 0; i < kStackSize; i++) {
      if (!Equal(stack[i], other.stack[i])) {
        OS::PrintErr("stack slot %" Pd " mismatch: %s expected, got %s\n", i,
                     stack[i], other.stack[i]);
        ok = false;
      }
    }
    return ok;
  }
};

namespace {

Location Reg(intptr_t reg) {
  return Location::MachineRegisterLocation(Location::kRegister, reg);
}

Location FpuReg(intptr_t reg) {
  return Location::MachineRegisterLocation(Location::kFpuRegister, reg);
}

Location Stack(intptr_t index) {
  return Location::StackSlot(index, FPREG);
};

Location Const(int v) {
  return Location::Constant(
      new ConstantInstr(Integer::ZoneHandle(Integer::New(v))));
}
Location Const(double v) {
  return Location::Constant(
      new ConstantInstr(Double::ZoneHandle(Double::New(v))));
}

}  // namespace

ISOLATE_UNIT_TEST_CASE(ParallelMove_TwoCycles) {
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

  auto run_test =
      [&](std::initializer_list<std::pair<Location, Location>> moves) {
        ParallelMoveInstr* instr = new ParallelMoveInstr();

        for (const auto& move : moves) {
          instr->AddMove(move.first, move.second);
        }
        constexpr intptr_t kInitialSpills = 10;
        OS::PrintErr("before scheduling: %s\n", instr->ToCString());
        ParallelMoveResolver resolver(/*is_intrinsic=*/false,
                                      /*spill_slot_count=*/kInitialSpills);
        resolver.Resolve(instr);
        OS::PrintErr("after scheduling: %s\n", instr->ToCString());

        AbstractState initial(thread->zone());

        AbstractState copy(initial);
        //copy.Print();
        copy.ExecuteInParallel(instr->moves());
        //copy.Print();

        initial.ExecuteInSequence(instr->move_schedule());
        //initial.Print();

        if (resolver.additional_spill_slots_required() != 0) {
          // Restore spill slots from the copy.
          for (intptr_t i = 0; i < resolver.additional_spill_slots_required();
               i++) {
            const auto slot_index = kInitialSpills + i;
            const auto stack_index =
                compiler::target::frame_layout.FrameSlotForVariableIndex(
                    -slot_index);
            const auto spill_loc = Location::StackSlot(stack_index, FPREG);
            *initial.SlotFor(spill_loc) = *copy.SlotFor(spill_loc);
          }
        }

        EXPECT(copy.CheckEqual(initial, temporaries));
      };

  run_test({
      {Reg(1), Reg(0)},
      {Reg(3), Reg(2)},
      {Reg(2), Reg(3)},
      {Reg(0), Reg(1)},
  });

  run_test({
      {Reg(0), Stack(-3)},
      {Stack(-1), Stack(-2)},
      {Stack(-2), Stack(-1)},
  });

  run_test({
      {Stack(-1), Stack(-2)},
      {Stack(-2), Stack(-1)},
      {Stack(-3), Reg(0)},
  });

  run_test({
      {Stack(-1), Stack(-2)},
      {Stack(-2), Stack(-1)},
      {Stack(-4), Reg(0)},
      {Stack(-3), Reg(0)},
  });

  run_test({
      {Stack(-1), Stack(-2)},
      {Stack(-2), Stack(-1)},
  });

  run_test({
      {Stack(-1), Stack(-2)},
      {Stack(-2), Stack(-1)},
      {Reg(0), Reg(1)},
  });

  run_test({
      {Stack(-1), Stack(-2)},
      {Stack(-2), Stack(-1)},
      {Stack(-3), Reg(0)},
  });

  run_test({
      {Reg(1), Reg(0)},
      {Reg(0), Const(1)},
      {FpuReg(1), Const(1.0)},
  });

  run_test({
      {Reg(1), Reg(0)},
      {Reg(0), Const(1)},
      {FpuReg(1), Const(1.0)},
  });

  // TODO XXX add test for spill slot fusion
}

}  // namespace compiler
}  // namespace dart