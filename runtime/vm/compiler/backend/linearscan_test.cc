// Copyright (c) 2022, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/backend/linearscan.h"

#include "vm/compiler/backend/block_builder.h"
#include "vm/compiler/backend/il_printer.h"
#include "vm/compiler/backend/il_test_helper.h"
#include "vm/unit_test.h"
#include "vm/zone_text_buffer.h"

namespace dart {

class DummyDef : public Definition {
 public:
  explicit DummyDef(LocationSummary* summary, std::initializer_list<Value*> inputs) : inputs_(inputs.size()), summary_(summary) {
    EXPECT_EQ(static_cast<intptr_t>(inputs.size()), summary_->input_count());

    intptr_t index = 0;
    for (auto v : inputs) {
      v->set_use_index(index++);
      v->set_instruction(this);
      inputs_.Add(v);
    }
  }

  LocationSummary* MakeLocationSummary(Zone* zone, bool opt) const {
    return summary_;
  }

  virtual void Accept(InstructionVisitor* visitor) {
    UNREACHABLE();
  }

  virtual Tag tag() const { return Instruction::kRedefinition; }

  virtual const char* DebugName() const { return "DummyDef"; }

  virtual intptr_t InputCount() const {
    return inputs_.length();
  }
  virtual Value* InputAt(intptr_t i) const {
    return inputs_[i];
  }

  virtual bool MayThrow() const { return false; }
  virtual bool ComputeCanDeoptimize() const { return false; }
  virtual bool HasUnknownSideEffects() const { return false; }

 private:
  virtual void RawSetInputAt(intptr_t i, Value* value) { inputs_[i] = value; }

  GrowableArray<Value*> inputs_;
  LocationSummary* const summary_;

  DISALLOW_COPY_AND_ASSIGN(DummyDef);
};

LocationSummary* MakeSummary(Zone* zone, std::initializer_list<Location> inputs, Location output, LocationSummary::ContainsCall contains_call = LocationSummary::kNoCall) {
  auto summary = new LocationSummary(zone, inputs.size(), /*temp_count=*/0, contains_call);
  intptr_t i = 0;
  for (auto input : inputs) {
    summary->set_in(i++, input);
  }
  summary->set_out(0, output);
  return summary;
}

ISOLATE_UNIT_TEST_CASE(LinearScan_TestFixedRegister) {
  using compiler::BlockBuilder;
  CompilerState S(thread, /*is_aot=*/false, /*is_optimizing=*/true);
  FlowGraphBuilderHelper H;

  auto zone = H.flow_graph()->zone();

  // We are going to build the following graph:
  //
  //   B0[graph_entry] {
  //     vc0 <- Constant(0)
  //     vc42 <- Constant(42)
  //   }
  //
  //   B1[function_entry] {
  //   }
  //   array <- StaticCall(...) {_Uint32List}
  //   v1 <- LoadIndexed(array)
  //   v2 <- LoadUntagged(array)
  //   StoreIndexed(v2, index=vc0, value=vc42)
  //   v3 <- LoadIndexed(array)
  //   return v3
  // }

  auto b1 = H.flow_graph()->graph_entry()->normal_entry();

  DummyDef* v0;
  DummyDef* v1;
  DummyDef* v2;

  {
    BlockBuilder builder(H.flow_graph(), b1);

    v0 = builder.AddDefinition(new DummyDef(MakeSummary(zone, {}, /*output=*/Location::RequiresRegister()), {}));
    v1 = builder.AddDefinition(new DummyDef(MakeSummary(zone, {}, /*output=*/Location::RegisterLocation(static_cast<Register>(0)), LocationSummary::kCall), {}));
    v2 = builder.AddDefinition(new DummyDef(MakeSummary(zone, {Location::RegisterLocation(static_cast<Register>(0))},
        /*output=*/Location::RequiresRegister()), {
            new Value(v0)
        }));
    builder.AddInstruction(new ReturnInstr(
        InstructionSource(), new Value(v1), S.GetNextDeoptId()));
  }
  H.FinishGraph();

  FlowGraphPrinter::PrintGraph("before regalloc", H.flow_graph());

  H.flow_graph()->InsertPushArguments();
  // Ensure loop hierarchy has been computed.
  H.flow_graph()->GetLoopHierarchy();
  // Perform register allocation on the SSA graph.
  FlowGraphAllocator allocator(*H.flow_graph());
  allocator.AllocateRegisters();

  ZoneTextBuffer buffer(zone);
  v0->locs()->PrintTo(&buffer);
  buffer.AddString("\n");
  v1->locs()->PrintTo(&buffer);
  buffer.AddString("\n");
  v2->locs()->PrintTo(&buffer);
  buffer.AddString("\n");

  OS::PrintErr("%s\n", buffer.buffer());
}

}  // namespace dart
