bool AnonImpl::support_integer_div() const {
  return TargetCPUFeatures::integer_division_supported();
}

bool AnonImpl::CanHoldLoadOffset(intptr_t offset) const {
  int32_t offset_mask = 0;
  return compiler::Address::CanHoldLoadOffset(kWord, offset, &offset_mask);
}
