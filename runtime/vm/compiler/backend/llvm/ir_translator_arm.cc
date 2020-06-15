bool AnonImpl::support_integer_div() const {
  return TargetCPUFeatures::integer_division_supported();
}

void AnonImpl::CallWriteBarrier(LValue object, LValue value) {
  COMPILE_ASSERT(kWriteBarrierObjectReg == R1);
  COMPILE_ASSERT(kWriteBarrierValueReg == R0);
  LValue entry_gep = output().buildGEPWithByteOffset(
      output().thread(),
      compiler::target::Thread::write_barrier_entry_point_offset(),
      pointerType(output().repo().ref8));
  LValue entry = output().buildLoad(entry_gep);
  static const char kAsmStr[] = "blx $0\n";
  static const char kContraintStr[] = "{r12},{r1},{r0},~{lr},~{memory}";
  LValue func = LLVMGetInlineAsm(
      functionType(output().repo().void_type, output().repo().ref8,
                   output().tagged_type(), output().tagged_type()),
      kAsmStr, sizeof(kAsmStr) - 1, kContraintStr, sizeof(kContraintStr) - 1,
      true, false, LLVMInlineAsmDialectATT);
  output().buildCall(func, entry, object, value);
}

void AnonImpl::CallArrayWriteBarrier(LValue object, LValue value, LValue slot) {
  COMPILE_ASSERT(kWriteBarrierObjectReg == R1);
  COMPILE_ASSERT(kWriteBarrierValueReg == R0);
  COMPILE_ASSERT(kWriteBarrierSlotReg == R9);
  LValue entry_gep = output().buildGEPWithByteOffset(
      output().thread(),
      compiler::target::Thread::array_write_barrier_entry_point_offset(),
      pointerType(output().repo().ref8));
  LValue entry = output().buildLoad(entry_gep);
  static const char kAsmStr[] = "blx $0\n";
  static const char kContraintStr[] = "{r12},{r1},{r0},{r9},~{lr},~{memory}";
  LValue func = LLVMGetInlineAsm(
      functionType(output().repo().void_type, output().repo().ref8,
                   output().tagged_type(), output().tagged_type()),
      kAsmStr, sizeof(kAsmStr) - 1, kContraintStr, sizeof(kContraintStr) - 1,
      true, false, LLVMInlineAsmDialectATT);
  output().buildCall(func, entry, object, value, slot);
}
