bool AnonImpl::support_integer_div() const {
  return TargetCPUFeatures::integer_division_supported();
}

static void AdjustAddress(size_t& instr_size, intptr_t offset) {
  static const constexpr size_t increment = Instr::kInstrSize;
  if (LIKELY(
          compiler::Address::CanHoldLoadOffset(kWord, offset, &offset_mask))) {
    return;
  } else {
    int32_t offset_hi = offset & ~offset_mask;  // signed
    compiler::Operand o;
    if (compiler::Operand::CanHold(offset_hi, &o)) {
      instr_size += increment;
    } else {
      instr_size += 2 * increment;
    }
  }
}

void AnonImpl::AdjustInstrSizeForLoadFromConsecutiveAddress(
    size_t& instr_size,
    intptr_t offset) const {
  int32_t offset_mask = 0;
  AdjustAddress(instr_size, offset);
  AdjustAddress(instr_size, offset + compiler::target::kWordSize);
}

void AnonImpl::AdjustInstrSizeForLoad(size_t& instr_size,
                                      intptr_t offset) const {
  AdjustAddress(instr_size, offset);
}

LValue AnonImpl::LoadObjectFromPool(intptr_t offset) {
  LValue gep = output().buildGEPWithByteOffset(
      GetPPValue(), output().constIntPtr(offset - kHeapObjectTag),
      pointerType(output().tagged_type()));
  return output().buildLoad(gep);
}
