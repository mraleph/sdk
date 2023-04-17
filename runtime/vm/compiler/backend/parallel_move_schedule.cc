
#include "vm/compiler/backend/parallel_move_schedule.h"

namespace dart {

using compiler::MoveSchedule;

template <>
void FlowGraphSerializer::WriteTrait<const MoveSchedule*>::Write(
    FlowGraphSerializer* s,
    const MoveSchedule* schedule) {
  ASSERT(schedule != nullptr);
  const intptr_t len = schedule->length();
  s->Write<intptr_t>(len);
  for (intptr_t i = 0; i < len; ++i) {
    const auto& op = (*schedule)[i];
    s->Write<uint8_t>(static_cast<uint8_t>(op.kind));
    op.operands.Write(s);
  }
}

template <>
const MoveSchedule* FlowGraphDeserializer::ReadTrait<const MoveSchedule*>::Read(
    FlowGraphDeserializer* d) {
  const intptr_t len = d->Read<intptr_t>();
  MoveSchedule& schedule = MoveSchedule::Allocate(len);
  for (intptr_t i = 0; i < len; ++i) {
    schedule[i].kind = static_cast<compiler::MoveOp::Kind>(d->Read<uint8_t>());
    schedule[i].operands = MoveOperands(d);
  }
  return &schedule;
}

}  // namespace dart