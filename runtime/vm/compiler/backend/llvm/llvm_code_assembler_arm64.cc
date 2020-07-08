void CodeAssembler::CallWithCallReg(const CallSiteInfo* call_site_info,
                                    dart::Register reg) {
  if (LIKELY(!call_site_info->is_tailcall()))
    assembler().blr(reg);
  else
    assembler().br(reg);
}

void CodeAssembler::GenerateNativeCall(const CallSiteInfo* call_site_info) {
  EMASSERT(call_site_info->native_entry_pool_offset() +
               compiler::target::kWordSize ==
           call_site_info->stub_pool_offset());
  assembler().add(R2, SP,
                  compiler::Operand(call_site_info->stack_parameter_count() *
                                    compiler::target::kWordSize));
  assembler().LoadWordFromPoolOffset(
      R5, call_site_info->native_entry_pool_offset());
  assembler().LoadWordFromPoolOffset(CODE_REG,
                                     call_site_info->stub_pool_offset());
  assembler().ldr(TMP, compiler::FieldAddress(
                           CODE_REG, compiler::target::Code::entry_point_offset(
                                         CodeEntryKind::kNormal)));
  assembler().blr(
      TMP);  // Use blx instruction so that the return branch prediction works.
}

void CodeAssembler::GeneratePatchableCall(const CallSiteInfo* call_site_info) {
  EMASSERT(call_site_info->ic_pool_offset() + compiler::target::kWordSize ==
           call_site_info->target_stub_pool_offset());
  assembler().LoadDoubleWordFromPoolOffset(R5, LR,
                                           call_site_info->ic_pool_offset());
  assembler().blr(
      LR);  // Use blx instruction so that the return branch prediction works.
}

static size_t RoundUp(size_t v, size_t align_shift) {
  v += (1U << align_shift) - 1;
  v >>= align_shift;
  v <<= align_shift;
  return v;
}

void CodeAssembler::PrepareLoadCPAction() {
  // Find relocation entries.
  const ByteBuffer* rela_buffer = compiler_state().FindByteBuffer(".rela.text");
  if (!rela_buffer) return;
  size_t ro_data_size = 0;
  size_t ro_data_cst8_size = 0;
  const ByteBuffer* rodata_buffer = compiler_state().FindByteBuffer(".rodata");
  if (rodata_buffer) {
    ro_data_size = rodata_buffer->size();
  }
  // FIXME: handle the elf relocatable file instead of these non-sense.
  const ByteBuffer* rodata_buffer_cst8 =
      compiler_state().FindByteBuffer(".rodata.cst8");
  if (rodata_buffer_cst8) {
    EMASSERT(compiler_state().FindByteBuffer(".rodata.cst16") == nullptr);
  }
  if (!rodata_buffer_cst8) {
    rodata_buffer_cst8 = compiler_state().FindByteBuffer(".rodata.cst16");
  }
  if (rodata_buffer_cst8) {
    ro_data_cst8_size = rodata_buffer_cst8->size();
  }
  EMASSERT(ro_data_size + ro_data_cst8_size > 0);

  const Elf64_Rela* rela =
      reinterpret_cast<const Elf64_Rela*>(rela_buffer->data());
  size_t count = rela_buffer->size() / sizeof(Elf64_Rela);
  for (size_t i = 0; i < count; ++i, ++rela) {
    int rtype = ELF64_R_TYPE(rela->r_info);
    size_t offset = rela->r_offset;
    size_t code_size_round_up = RoundUp(code_size_, 3);
    switch (rtype) {
      case R_AARCH64_LD_PREL_LO19: {
        auto func = [this, offset, rela, code_size_round_up] {
          const uint32_t* pc =
              reinterpret_cast<const uint32_t*>(code_start_ + offset);
          uint32_t instr = *pc;
          const uint32_t imm19_mask = ((1U << 19) - 1U) << 5U;
          size_t ro_offset = rela->r_addend;
          size_t new_offset = code_size_round_up + ro_offset - offset;

          size_t new_imm19 = new_offset >> 2;
          EMASSERT(new_imm19 == (new_imm19 & ((1U << 19U) - 1U)));
          instr &= ~imm19_mask;
          instr |= new_imm19 << 5U;
          assembler().EmitRange(&instr, 4);
          return 4;
        };
        AddAction(offset, WrapAction(func));
        break;
      }
      case R_AARCH64_ADR_PREL_LO21: {
        auto func = [this, offset, rela, ro_data_cst8_size,
                     code_size_round_up] {
          const uint32_t* pc =
              reinterpret_cast<const uint32_t*>(code_start_ + offset);
          uint32_t instr = *pc;
          size_t ro_offset = rela->r_addend;
          size_t new_offset =
              (code_size_round_up + ro_offset + ro_data_cst8_size - offset);
          EMASSERT(new_offset == (new_offset & ((1U << 21U) - 1U)));
          size_t new_offset_lo = new_offset & ((1U << 2) - 1U);
          size_t new_offset_hi = new_offset >> 2U;
          instr |= (new_offset_lo << 29) | (new_offset_hi << 5);
          assembler().EmitRange(&instr, 4);
          return 4;
        };
        AddAction(offset, WrapAction(func));
        break;
      }
      default:
        continue;
    }
  }
}

void CodeAssembler::EmitCP() {
  // FIXME: handle the elf relocatable file instead of these non-sense.
  const ByteBuffer* rodata_buffer = compiler_state().FindByteBuffer(".rodata");
  const ByteBuffer* rodata_buffer_cst8 =
      compiler_state().FindByteBuffer(".rodata.cst8");
  if (!rodata_buffer_cst8) {
    rodata_buffer_cst8 = compiler_state().FindByteBuffer(".rodata.cst16");
  }
  if (!rodata_buffer && !rodata_buffer_cst8) return;
  if ((assembler().CodeSize() % 8) != 0) assembler().Breakpoint();
  if (rodata_buffer_cst8)
    assembler().EmitRange(rodata_buffer_cst8->data(),
                          rodata_buffer_cst8->size());
  if (rodata_buffer)
    assembler().EmitRange(rodata_buffer->data(), rodata_buffer->size());
}
