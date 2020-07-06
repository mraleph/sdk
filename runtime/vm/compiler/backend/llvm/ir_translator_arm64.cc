bool AnonImpl::support_integer_div() const {
  return true;
}

void AnonImpl::AdjustInstrSizeForLoadFromConsecutiveAddress(
    size_t& instr_size,
    intptr_t offset) const {
  static const constexpr size_t increment = Instr::kInstrSize;
  const uint32_t upper20 = offset & 0xfffff000;
  const uint32_t lower12 = offset & 0x00000fff;
  compiler::Operand op;
  if (LIKELY(compiler::Address::CanHoldOffset(offset,
                                              compiler::Address::PairOffset))) {
    return;
  }
  if (compiler::Operand::CanHold(offset, kXRegSizeInBits, &op) ==
      compiler::Operand::Immediate) {
    instr_size += increment;
  } else if (compiler::Operand::CanHold(upper20, kXRegSizeInBits, &op) ==
                 compiler::Operand::Immediate &&
             compiler::Address::CanHoldOffset(lower12,
                                              compiler::Address::PairOffset)) {
    instr_size += increment;
  } else {
    instr_size += increment;
    instr_size += increment;
  }
}

void AnonImpl::AdjustInstrSizeForLoad(size_t& instr_size,
                                      intptr_t offset) const {
  static const constexpr size_t increment = Instr::kInstrSize;
  const uint32_t upper20 = offset & 0xfffff000;
  compiler::Operand op;
  if (LIKELY(compiler::Address::CanHoldOffset(offset))) {
    return;
  } else if (compiler::Operand::CanHold(upper20, kXRegSizeInBits, &op) ==
             compiler::Operand::Immediate) {
    instr_size += increment;
  } else {
    instr_size += increment;
    instr_size += increment;
  }
}

LValue AnonImpl::LoadObjectFromPool(intptr_t offset) {
  LValue gep = output().buildGEPWithByteOffset(
      GetPPValue(), output().constIntPtr(offset),
      pointerType(output().tagged_type()));
  return output().buildLoad(gep);
}
