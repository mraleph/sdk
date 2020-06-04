#include "vm/compiler/backend/llvm/llvm_code_assembler.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/bitmap.h"
#include "vm/compiler/assembler/assembler.h"
#include "vm/compiler/backend/flow_graph.h"
#include "vm/compiler/backend/flow_graph_compiler.h"
#include "vm/compiler/backend/il.h"
#include "vm/compiler/backend/llvm/compiler_state.h"
#include "vm/compiler/backend/llvm/dwarf_info.h"
#include "vm/compiler/backend/llvm/exception_table_parser.h"
#include "vm/compiler/backend/llvm/stack_map_info.h"
#include "vm/compiler/backend/llvm/stack_maps.h"
#include "vm/compiler/backend/llvm/target_specific.h"

namespace dart {
namespace dart_llvm {
CodeAssembler::CodeAssembler(FlowGraphCompiler* compiler)
    : compiler_(compiler) {
  EMASSERT(compiler_state().code_section_list_.size() == 1);
  const ByteBuffer& code_buffer = compiler_state().code_section_list_.back();
  code_start_ = code_buffer.data();
  bytes_left_ = code_buffer.size();
  compiler->InitCompiler();
}

void CodeAssembler::AssembleCode() {
  PrepareExceptionTable();
  PrepareInstrActions();
  for (auto& p : action_map_) {
    unsigned end = p.first;
    auto& action = p.second;
    if (offset_ != end) {
      EMASSERT(end > offset_);
      assembler().EmitRange(code_start_ + offset_, end - offset_);
      bytes_left_ -= end - offset_;
      offset_ = end;
    }
    action();
  }
  if (bytes_left_ != 0) {
    assembler().EmitRange(code_start_ + offset_, bytes_left_);
  }
  EndLastInstr();
  EmitExceptionHandler();
}

FlowGraphCompiler& CodeAssembler::compiler() {
  return *compiler_;
}
const CompilerState& CodeAssembler::compiler_state() {
  return compiler().flow_graph().llvm_compiler_state();
}
compiler::Assembler& CodeAssembler::assembler() {
  return *compiler().assembler();
}

void CodeAssembler::PrepareExceptionTable() {
  if (!compiler_state().exception_table_) return;
  ExceptionTableParser parser(compiler_state().exception_table_->data(),
                              compiler_state().exception_table_->size());
  exception_tuples_ = std::move(parser.CallSiteHandlerPairs());
  // should I sort it?
}

void CodeAssembler::PrepareInstrActions() {
  PrepareDwarfAction();
  PrepareStackMapAction();
}

void CodeAssembler::PrepareDwarfAction() {
  DwarfLineMapper mapper;
  mapper.Process(compiler_state().dwarf_line_->data());
  auto& debug_instrs_ = compiler_state().debug_instrs_;
  for (auto& p : mapper.GetMap()) {
    if (p.second == 0) continue;
    unsigned pc_offset = p.first;
    unsigned index = p.second - 1;
    Instruction* instr = debug_instrs_[index];
    auto func = [this, instr]() -> size_t {
      if (FLAG_code_comments || FLAG_disassemble ||
          FLAG_disassemble_optimized) {
#if 0
        // This flag is not defined as global.
        if (FLAG_source_lines) {
          compiler().EmitSourceLine(instr);
        }
#endif
        compiler().EmitComment(instr);
      }
      EndLastInstr();
      // FIXME: Handle Yield for return!
      compiler().BeginCodeSourceRange();
      last_instr_ = instr;
      return static_cast<size_t>(0);
    };
    action_map_.emplace(pc_offset, WrapAction(func));
  }
}

void CodeAssembler::PrepareStackMapAction() {
  if (!compiler_state().stackMapsSection_) return;
  StackMaps sm;
  DataView dv(compiler_state().stackMapsSection_->data());
  sm.parse(&dv);
  slot_count_ = sm.stackSize() / compiler::target::kWordSize;
  if (slot_count_ != 0) {
    EMASSERT(slot_count_ >= 2);
    // must minus 2 slot for fp and return address.
    slot_count_ -= 2;
  }
  compiler().flow_graph().graph_entry()->set_spill_slot_count(slot_count_);

  auto rm = sm.computeRecordMap();
  const StackMapInfoMap& stack_map_info_map =
      compiler_state().stack_map_info_map_;
  for (auto& item : rm) {
    EMASSERT(item.second.size() == 1);
    const auto& record = item.second.front();
    uint32_t instruction_offset = item.first;
    auto stack_map_info_found = stack_map_info_map.find(record.patchpointID);
    EMASSERT(stack_map_info_found != stack_map_info_map.end());
    const StackMapInfo* stack_map_info = stack_map_info_found->second.get();
    EMASSERT(stack_map_info->GetType() == StackMapInfoType::kCallInfo);
    const CallSiteInfo* call_site_info =
        static_cast<const CallSiteInfo*>(stack_map_info);
    std::function<void()> f;
    switch (call_site_info->type()) {
      case CallSiteInfo::CallTargetType::kReg:
        f = WrapAction([this, call_site_info, record]() {
#if defined(TARGET_ARCH_ARM)
          if (LIKELY(!call_site_info->is_tailcall()))
            assembler().blx(kCallTargetReg);
          else
            assembler().bx(kCallTargetReg);
#else
#error unsupported arch
#endif
          AddMetaData(call_site_info, record);
          if (call_site_info->return_on_stack()) {
            assembler().LoadMemoryValue(
                CallingConventions::kReturnReg, SP,
                (call_site_info->stack_parameter_count() - 1) *
                    compiler::target::kWordSize);
          }
          return call_site_info->instr_size();
        });
        break;
      case CallSiteInfo::CallTargetType::kCallRelative:
        f = WrapAction([this, call_site_info, record]() {
          assembler().GenerateUnRelocatedPcRelativeCall();
          compiler().AddPcRelativeCallTarget(*call_site_info->target(),
                                             call_site_info->entry_kind());
          AddMetaData(call_site_info, record);
          return call_site_info->instr_size();
        });
        break;
      case CallSiteInfo::CallTargetType::kStubRelative:
        f = WrapAction([this, call_site_info, record]() {
          assembler().GenerateUnRelocatedPcRelativeCall();
          compiler().AddPcRelativeCallStubTarget(*call_site_info->code());
          AddMetaData(call_site_info, record);
          return call_site_info->instr_size();
        });
        break;
      default:
        UNREACHABLE();
    }
    action_map_.emplace(instruction_offset, f);
  }
}

template <typename T>
std::function<void()> CodeAssembler::WrapAction(T f) {
  return [this, f]() {
    intptr_t before_action = assembler().CodeSize();
    size_t expected_adv = f();
    intptr_t after_action = assembler().CodeSize();
    intptr_t adv = after_action - before_action;
    EMASSERT(expected_adv == static_cast<size_t>(adv));
    offset_ += adv;
    bytes_left_ -= adv;
  };
}

void CodeAssembler::AddMetaData(const CallSiteInfo* call_site_info,
                                const StackMaps::Record& r) {
  intptr_t try_index = CollectExceptionInfo(call_site_info);
  compiler().AddDescriptor(call_site_info->kind(), assembler().CodeSize(),
                           call_site_info->deopt_id(),
                           call_site_info->token_pos(), try_index);
  RecordSafePoint(call_site_info, r);
}

static inline uint32_t DART_USED Extract32(uint32_t value,
                                           int start,
                                           int length) {
  assert(start >= 0 && length > 0 && length <= 32 - start);
  return (value >> start) & (~0U >> (32 - length));
}

static inline int32_t DART_USED Sextract32(uint32_t value,
                                           int start,
                                           int length) {
  assert(start >= 0 && length > 0 && length <= 32 - start);
  /* Note that this implementation relies on right shift of signed
     * integers being an arithmetic shift.
     */
  return ((int32_t)(value << (32 - length - start))) >> (32 - length);
}

#if defined(TARGET_ARCH_ARM)
static int BranchOffset(const void* code) {
  uint32_t insn = *reinterpret_cast<const uint32_t*>(code);
  unsigned op1 = (insn >> 24) & 0xf;
  switch (op1) {
    case 0xa:
    case 0xb: {
      int32_t offset;

      /* branch (and link) */
      if (insn & (1 << 24)) {
        break;
      }
      offset = Sextract32(insn << 2, 0, 26);
      offset += 8;
      return offset;
    }
    default:
      break;
  }
  return -1;
}
#elif defined(TARGET_ARCH_ARM64)
static int BranchOffset(const void* code) {
  uint32_t insn = *reinterpret_cast<const uint32_t*>(code);

  switch (Extract32(insn, 25, 7)) {
    case 0x0a:
    case 0x0b:
    case 0x4a:
    case 0x4b: {
      return Sextract32(insn, 0, 26) * 4;
    }
    default:
      break;
  }
  return -1;
}
#else
#error unsupport arch
#endif

intptr_t CodeAssembler::CollectExceptionInfo(
    const CallSiteInfo* call_site_info) {
  if (exception_tuples_.empty()) {
    EMASSERT(call_site_info->try_index() == kInvalidTryIndex);
    return kInvalidTryIndex;
  }
  if (call_site_info->try_index() == kInvalidTryIndex) return kInvalidTryIndex;
  intptr_t try_index = call_site_info->try_index();
#if 0
  printf("printing exception_tuples_\n");
  for (auto& t : exception_tuples_) {
    printf("0x%x %d 0x%x\n", std::get<0>(t), std::get<1>(t), std::get<2>(t));
  }
#endif
  auto found =
      std::lower_bound(exception_tuples_.begin(), exception_tuples_.end(),
                       assembler().CodeSize(),
                       [](const std::tuple<int, int, int>& lhs, intptr_t rhs) {
                         return std::get<0>(lhs) < rhs;
                       });
  if (found == exception_tuples_.end() ||
      std::get<0>(*found) != assembler().CodeSize())
    found -= 1;
  EMASSERT(assembler().CodeSize() >= std::get<0>(*found));
  EMASSERT(assembler().CodeSize() <= std::get<0>(*found) + std::get<1>(*found));
  int exception_block_off = std::get<2>(*found);
  int branch_offset = BranchOffset(code_start_ + exception_block_off);
  if (branch_offset != -1) exception_block_off += branch_offset;
  auto exception_map_found = exception_map_.find(try_index);
  if (exception_map_found != exception_map_.end()) {
    // need a extened try index;
    intptr_t extended_try_index = ((++exception_extend_id_) << 16) | try_index;
    try_index = extended_try_index;
  }
  exception_map_.emplace(try_index, exception_block_off);
  return try_index;
}

void CodeAssembler::RecordSafePoint(const CallSiteInfo* call_site_info,
                                    const StackMaps::Record& record) {
  if (UNLIKELY(call_site_info->is_tailcall())) return;
  BitmapBuilder* builder = new BitmapBuilder();
  builder->SetLength(slot_count_);

  for (auto& location : record.locations) {
    if (location.kind != StackMaps::Location::Indirect) continue;
    // only understand stack slot
    int index;
    if (location.dwarfReg == SP) {
      // Remove the effect from safepoint-table.cc
      EMASSERT(location.offset >= 0);
      index = slot_count_ - 1 - location.offset / compiler::target::kWordSize;
    } else {
      EMASSERT(location.dwarfReg == FP);
      if (location.offset >= 0) continue;
      index = -location.offset / compiler::target::kWordSize - 1;
    }
    builder->Set(index, true);
  }
  // set up parameters
  // FIXME: only support tagged parameter now.
  for (size_t i = 0; i < call_site_info->stack_parameter_count(); ++i) {
    int index = slot_count_ - i - 1;
    builder->Set(index, true);
  }
  compiler().compressed_stackmaps_builder()->AddEntry(assembler().CodeSize(),
                                                      builder, slot_count_);
}

void CodeAssembler::EmitExceptionHandler() {
  if (exception_map_.empty()) return;
  GraphEntryInstr* graph_entry = compiler().flow_graph().graph_entry();
  for (auto& p : exception_map_) {
    intptr_t try_index = p.first;
    intptr_t origin_try_index = try_index & 0xffff;
    CatchBlockEntryInstr* catch_block =
        graph_entry->GetCatchEntry(origin_try_index);
    compiler().AddExceptionHandler(catch_block->catch_try_index(), try_index,
                                   p.second, catch_block->is_generated(),
                                   catch_block->catch_handler_types(),
                                   catch_block->needs_stacktrace());
  }
}

void CodeAssembler::EndLastInstr() {
  if (!last_instr_) return;

  compiler().EndCodeSourceRange(last_instr_->token_pos());
}
}  // namespace dart_llvm
}  // namespace dart
#endif
