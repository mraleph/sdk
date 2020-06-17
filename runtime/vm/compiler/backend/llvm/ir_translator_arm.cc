bool AnonImpl::support_integer_div() const {
  return TargetCPUFeatures::integer_division_supported();
}
