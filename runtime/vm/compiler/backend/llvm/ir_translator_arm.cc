bool AnonImpl::support_integer_div() const {
  return TargetCPUFeatures::integer_division_supported();
}

void AnonImpl::AdjustInstrSizeForLoadFromConsecutiveAddress(
    size_t& instr_size,
    intptr_t offset) const {
  static const constexpr size_t increment = Instr::kInstrSize;
  int32_t offset_mask = 0;
  auto f = [&](intptr_t offset) {
    if (LIKELY(compiler::Address::CanHoldLoadOffset(kWord, offset,
                                                    &offset_mask))) {
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
  };
  f(offset);
  f(offset + compiler::target::kWordSize);
}
