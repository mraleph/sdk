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
  assembler().LoadDoubleWordFromPoolOffset(
      R5, CODE_REG, call_site_info->native_entry_pool_offset());
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
