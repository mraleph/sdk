#include "vm/compiler/backend/llvm/dwarf_info.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
namespace dart {
namespace dart_llvm {
DwarfLineMapper::DwarfLineMapper() {}
DwarfLineMapper::~DwarfLineMapper() {}

bool DwarfLineMapper::Process(const uint8_t* data) {
  start_ = data;
  if (!ReadHeader()) return false;
  ResetStateMachine(is_statment_);
  return ProcessInstrs();
}

bool DwarfLineMapper::ReadHeader() {
  DataView view(start_);
  unsigned offset = 0;

  uint32_t length = view.read<uint32_t>(offset);
  if (length == 0xffffffff) {
    uint64_t length64 = view.read<uint64_t>(offset);
    cu_length_ = length64;
    offset_length_ = 8;
  } else {
    cu_length_ = length;
    offset_length_ = 4;
  }
  instr_end_ = offset + cu_length_;
  cu_version_ = view.read<uint32_t>(offset);
  switch (cu_version_) {
    case 2:
    case 3:
    case 4:
      break;
    default:
      return false;
  }
  prologue_length_ = view.read_amount(offset, offset_length_);
  instr_start_ = offset + prologue_length_;
  min_instr_len_ = view.read<uint8_t>(offset);
  if (cu_version_ >= 4) {
    max_ops_per_insn_ = view.read<uint8_t>(offset);
  } else {
    max_ops_per_insn_ = 1;
  }
  is_statment_ = view.read<uint8_t>(offset);
  line_base_ = view.read<int8_t>(offset);
  line_range_ = view.read<uint8_t>(offset);
  opcode_base_ = view.read<uint8_t>(offset);
  return true;
}

bool DwarfLineMapper::ProcessInstrs() {
  enum dwarf_line_number_ops {
    DW_LNS_extended_op = 0,
    DW_LNS_copy = 1,
    DW_LNS_advance_pc = 2,
    DW_LNS_advance_line = 3,
    DW_LNS_set_file = 4,
    DW_LNS_set_column = 5,
    DW_LNS_negate_stmt = 6,
    DW_LNS_set_basic_block = 7,
    DW_LNS_const_add_pc = 8,
    DW_LNS_fixed_advance_pc = 9,
    /* DWARF 3.  */
    DW_LNS_set_prologue_end = 10,
    DW_LNS_set_epilogue_begin = 11,
    DW_LNS_set_isa = 12
  };

  DataView view(start_);
  unsigned& offset = offset_current_;
  offset = instr_start_;

  int verbose_view = 0;

  while (offset < instr_end_) {
    uint8_t op_code = view.read<uint8_t>(offset);
    unsigned long uladv;
    signed long adv;
    const uint8_t* end = start_ + instr_end_;

    if (op_code >= opcode_base_) {
      op_code -= opcode_base_;
      uladv = (op_code / line_range_);
      if (max_ops_per_insn_ == 1) {
        uladv *= min_instr_len_;
        state_machine_regs_.address += uladv;
        if (uladv) state_machine_regs_.view = 0;
        LLVMLOGV(
            "  Special opcode %d: "
            "advance Address by %lu to 0x%lx%s",
            op_code, uladv, state_machine_regs_.address,
            verbose_view && uladv ? " (reset view)" : "");
      } else {
        unsigned addrdelta =
            ((state_machine_regs_.op_index + uladv) / max_ops_per_insn_) *
            min_instr_len_;

        state_machine_regs_.address += addrdelta;
        state_machine_regs_.op_index =
            (state_machine_regs_.op_index + uladv) % max_ops_per_insn_;
        if (addrdelta) state_machine_regs_.view = 0;
        LLVMLOGV(
            "  Special opcode %d: "
            "advance Address by %lu to 0x%lx[%d]%s",
            op_code, uladv, state_machine_regs_.address,
            state_machine_regs_.op_index,
            verbose_view && addrdelta ? " (reset view)" : "");
      }
      adv = (op_code % line_range_) + line_base_;
      state_machine_regs_.line += adv;
      LLVMLOGV(" and Line by %ld to %d", adv, state_machine_regs_.line);
      if (verbose_view || state_machine_regs_.view)
        LLVMLOGV(" (view %u)\n", state_machine_regs_.view);
      state_machine_regs_.view++;
      RecordLine(false);
    } else
      switch (op_code) {
        case DW_LNS_extended_op:
          EMASSERT(false && "DW_LNS_extended_op not support");
          break;

        case DW_LNS_copy:
          LLVMLOGV("  Copy");
          if (verbose_view || state_machine_regs_.view)
            LLVMLOGV(" (view %u)\n", state_machine_regs_.view);
          else
            LLVMLOGV("\n");
          state_machine_regs_.view++;
          break;

        case DW_LNS_advance_pc:
          uladv = view.ReadULEB128(offset, end);
          if (max_ops_per_insn_ == 1) {
            uladv *= min_instr_len_;
            state_machine_regs_.address += uladv;
            if (uladv) state_machine_regs_.view = 0;
            LLVMLOGV("  Advance PC by %lu to 0x%lx%s\n", uladv,
                     state_machine_regs_.address,
                     verbose_view && uladv ? " (reset view)" : "");
          } else {
            unsigned addrdelta =
                ((state_machine_regs_.op_index + uladv) / max_ops_per_insn_) *
                min_instr_len_;
            state_machine_regs_.address += addrdelta;
            state_machine_regs_.op_index =
                (state_machine_regs_.op_index + uladv) % max_ops_per_insn_;
            if (addrdelta) state_machine_regs_.view = 0;
            LLVMLOGV("  Advance PC by %lu to 0x%lx[%d]%s\n", uladv,
                     state_machine_regs_.address, state_machine_regs_.op_index,
                     verbose_view && addrdelta ? " (reset view)" : "");
          }
          break;

        case DW_LNS_advance_line:
          adv = view.ReadULEB128(offset, end);
          state_machine_regs_.line += adv;
          LLVMLOGV("  Advance Line by %ld to %d\n", adv,
                   state_machine_regs_.line);
          break;

        case DW_LNS_set_file:
          adv = view.ReadULEB128(offset, end);
          LLVMLOGV("  Set File Name to entry %ld in the File Name Table\n",
                   adv);
          state_machine_regs_.file = adv;
          break;

        case DW_LNS_set_column:
          uladv = view.ReadULEB128(offset, end);
          LLVMLOGV("  Set column to %lu\n", uladv);
          state_machine_regs_.column = uladv;
          break;

        case DW_LNS_negate_stmt:
          adv = state_machine_regs_.is_stmt;
          adv = !adv;
          LLVMLOGV("  Set is_stmt to %ld\n", adv);
          state_machine_regs_.is_stmt = adv;
          break;

        case DW_LNS_set_basic_block:
          LLVMLOGV("  Set basic block\n");
          state_machine_regs_.basic_block = 1;
          break;

        case DW_LNS_const_add_pc:
          uladv = ((255 - opcode_base_) / line_range_);
          if (max_ops_per_insn_) {
            uladv *= min_instr_len_;
            state_machine_regs_.address += uladv;
            if (uladv) state_machine_regs_.view = 0;
            LLVMLOGV("  Advance PC by constant %lu to 0x%lx%s\n", uladv,
                     state_machine_regs_.address,
                     verbose_view && uladv ? " (reset view)" : "");
          } else {
            unsigned addrdelta =
                ((state_machine_regs_.op_index + uladv) / max_ops_per_insn_) *
                min_instr_len_;
            state_machine_regs_.address += addrdelta;
            state_machine_regs_.op_index =
                (state_machine_regs_.op_index + uladv) % max_ops_per_insn_;
            if (addrdelta) state_machine_regs_.view = 0;
            LLVMLOGV("  Advance PC by constant %lu to 0x%lx[%d]%s\n", uladv,
                     state_machine_regs_.address, state_machine_regs_.op_index,
                     verbose_view && addrdelta ? " (reset view)" : "");
          }
          break;

        case DW_LNS_fixed_advance_pc:
          uladv = view.read<uint16_t>(offset);
          state_machine_regs_.address += uladv;
          state_machine_regs_.op_index = 0;
          LLVMLOGV("  Advance PC by fixed size amount %lu to 0x%lx\n", uladv,
                   state_machine_regs_.address);
          /* Do NOT reset view.  */
          break;

        case DW_LNS_set_prologue_end:
          LLVMLOGV("  Set prologue_end to true\n");
          break;

        case DW_LNS_set_epilogue_begin:
          LLVMLOGV("  Set epilogue_begin to true\n");
          break;

        case DW_LNS_set_isa:
          uladv = view.ReadULEB128(offset, end);
          LLVMLOGV("  Set ISA to %lu\n", uladv);
          break;

        default:
          LLVMLOGV("  Unknown opcode %d with operands: ", op_code);

          return false;
      }
  }
  RecordLine(true);
  return true;
}

bool DwarfLineMapper::ProcessExtendedLineOp() {
#if 0
  uint8_t op_code;
  unsigned int bytes_read;
  uint64_t len;
  unsigned char *name;
  unsigned char *orig_data = data;
  unsigned long adr;
  DataView view(start_);
  unsigned offset_old = offset_current_;

  len = view.ReadULEB128(offset_current_, start_ + instr_end_);

  if (len == 0 || offset_current_ == instr_end_ || len > (uintptr_t) (instr_end_ - offset_current_))
    {
      LLVMLOGE(("Badly formed extended line op encountered!\n");
      return false;
    }
  bytes_read = offset_current_ - offset_old
  len += bytes_read;
  op_code = view.read<uint8_t>(offset_current_);

  LOGI ("  Extended opcode %d: ", op_code);

enum dwarf_line_number_x_ops
  {
    DW_LNE_end_sequence = 1,
    DW_LNE_set_address = 2,
    DW_LNE_define_file = 3,
    DW_LNE_set_discriminator = 4,
    /* HP extensions.  */
    DW_LNE_HP_negate_is_UV_update      = 0x11,
    DW_LNE_HP_push_context             = 0x12,
    DW_LNE_HP_pop_context              = 0x13,
    DW_LNE_HP_set_file_line_column     = 0x14,
    DW_LNE_HP_set_routine_name         = 0x15,
    DW_LNE_HP_set_sequence             = 0x16,
    DW_LNE_HP_negate_post_semantics    = 0x17,
    DW_LNE_HP_negate_function_exit     = 0x18,
    DW_LNE_HP_negate_front_end_logical = 0x19,
    DW_LNE_HP_define_proc              = 0x20,
    DW_LNE_HP_source_file_correlation  = 0x80,

    DW_LNE_lo_user = 0x80,
    DW_LNE_hi_user = 0xff
  };

  switch (op_code)
    {
    case DW_LNE_end_sequence:
      LOGI ("End of Sequence\n\n");
      ResetStateMachine(is_stmt);
      break;

    case DW_LNE_set_address:
      /* PR 17512: file: 002-100480-0.004.  */
      if (len - bytes_read - 1 > 8)
	{
	  LLVMLOGV(_("Length (%d) of DW_LNE_set_address op is too long\n"),
		len - bytes_read - 1);
	  adr = 0;
	}
      else
      adr = view.read_amount(len - bytes_read - 1);
      LLVMLOGV ("set Address to 0x%x\n", adr);
      state_machine_regs.address = adr;
      state_machine_regs.view = 0;
      state_machine_regs.op_index = 0;
      break;

    case DW_LNE_define_file:
      ++state_machine_regs.last_file_entry;
      LLVMLOGV("define new File Table entry\n");
      LLVMLOGV ("  Entry\tDir\tTime\tSize\tName\n");
      LLVMLOGV ("   %d\t", state_machine_regs.last_file_entry);

      {
	size_t l;

	name = data;
	l = strnlen ((char *) data, end - data);
	data += len + 1;
	LLVMLOGV ("%s\t", dwarf_vmatoa ("u", read_uleb128 (data, & bytes_read, end)));
	data += bytes_read;
	LLVMLOGV ("%s\t", dwarf_vmatoa ("u", read_uleb128 (data, & bytes_read, end)));
	data += bytes_read;
	LLVMLOGV ("%s\t", dwarf_vmatoa ("u", read_uleb128 (data, & bytes_read, end)));
	data += bytes_read;
	LLVMLOGV ("%.*s\n\n", (int) l, name);
      }

      if (((unsigned int) (data - orig_data) != len) || data == end)
	warn (_("DW_LNE_define_file: Bad opcode length\n"));
      break;

    case DW_LNE_set_discriminator:
      LLVMLOGV (_("set Discriminator to %s\n"),
	      dwarf_vmatoa ("u", read_uleb128 (data, & bytes_read, end)));
      break;

    /* HP extensions.  */
    case DW_LNE_HP_negate_is_UV_update:
      LLVMLOGV ("DW_LNE_HP_negate_is_UV_update\n");
      break;
    case DW_LNE_HP_push_context:
      LLVMLOGV ("DW_LNE_HP_push_context\n");
      break;
    case DW_LNE_HP_pop_context:
      LLVMLOGV ("DW_LNE_HP_pop_context\n");
      break;
    case DW_LNE_HP_set_file_line_column:
      LLVMLOGV ("DW_LNE_HP_set_file_line_column\n");
      break;
    case DW_LNE_HP_set_routine_name:
      LLVMLOGV ("DW_LNE_HP_set_routine_name\n");
      break;
    case DW_LNE_HP_set_sequence:
      LLVMLOGV ("DW_LNE_HP_set_sequence\n");
      break;
    case DW_LNE_HP_negate_post_semantics:
      LLVMLOGV ("DW_LNE_HP_negate_post_semantics\n");
      break;
    case DW_LNE_HP_negate_function_exit:
      LLVMLOGV ("DW_LNE_HP_negate_function_exit\n");
      break;
    case DW_LNE_HP_negate_front_end_logical:
      LLVMLOGV ("DW_LNE_HP_negate_front_end_logical\n");
      break;
    case DW_LNE_HP_define_proc:
      LLVMLOGV ("DW_LNE_HP_define_proc\n");
      break;
    case DW_LNE_HP_source_file_correlation:
      {
	unsigned char *edata = data + len - bytes_read - 1;

	LLVMLOGV ("DW_LNE_HP_source_file_correlation\n");

	while (data < edata)
	  {
	    unsigned int opc;

	    opc = read_uleb128 (data, & bytes_read, edata);
	    data += bytes_read;

	    switch (opc)
	      {
	      case DW_LNE_HP_SFC_formfeed:
		LLVMLOGV ("    DW_LNE_HP_SFC_formfeed\n");
		break;
	      case DW_LNE_HP_SFC_set_listing_line:
		LLVMLOGV ("    DW_LNE_HP_SFC_set_listing_line (%s)\n",
			dwarf_vmatoa ("u",
				      read_uleb128 (data, & bytes_read, edata)));
		data += bytes_read;
		break;
	      case DW_LNE_HP_SFC_associate:
		LLVMLOGV ("    DW_LNE_HP_SFC_associate ");
		LLVMLOGV ("(%s",
			dwarf_vmatoa ("u",
				      read_uleb128 (data, & bytes_read, edata)));
		data += bytes_read;
		LLVMLOGV (",%s",
			dwarf_vmatoa ("u",
				      read_uleb128 (data, & bytes_read, edata)));
		data += bytes_read;
		LLVMLOGV (",%s)\n",
			dwarf_vmatoa ("u",
				      read_uleb128 (data, & bytes_read, edata)));
		data += bytes_read;
		break;
	      default:
		LLVMLOGV (_("    UNKNOWN DW_LNE_HP_SFC opcode (%u)\n"), opc);
		data = edata;
		break;
	      }
	  }
      }
      break;

    default:
      {
	unsigned int rlen = len - bytes_read - 1;

	if (op_code >= DW_LNE_lo_user
	    /* The test against DW_LNW_hi_user is redundant due to
	       the limited range of the unsigned char data type used
	       for op_code.  */
	    /*&& op_code <= DW_LNE_hi_user*/)
	  LLVMLOGV (_("user defined: "));
	else
	  LLVMLOGV (_("UNKNOWN: "));
	LLVMLOGV (_("length %d ["), rlen);
	for (; rlen; rlen--)
	  LLVMLOGV (" %02x", *data++);
	LLVMLOGV ("]\n");
      }
      break;
    }

  return len;
#else
  return false;
#endif
}

void DwarfLineMapper::ResetStateMachine(int is_stmt) {
  state_machine_regs_.address = 0;
  state_machine_regs_.view = 0;
  state_machine_regs_.op_index = 0;
  state_machine_regs_.file = 1;
  state_machine_regs_.line = 1;
  state_machine_regs_.column = 0;
  state_machine_regs_.is_stmt = is_stmt;
  state_machine_regs_.basic_block = 0;
  state_machine_regs_.end_sequence = 0;
  state_machine_regs_.last_file_entry = 0;
}

void DwarfLineMapper::RecordLine(bool end_of_seqence) {
  if (state_machine_regs_.op_index && !end_of_seqence) return;
  map_.emplace(state_machine_regs_.address, state_machine_regs_.line);
}

void DwarfLineMapper::Dump() {
  for (auto& p : map_) {
    printf("addr:%x, %u\n", p.first, p.second);
  }
}
}  // namespace dart_llvm
}  // namespace dart
#endif
