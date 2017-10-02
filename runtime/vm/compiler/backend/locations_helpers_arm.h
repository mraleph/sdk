// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_ARM_H_
#define RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_ARM_H_

namespace dart {

class QRegister_ {
 public:
  inline DRegister d(intptr_t i) const {
    return static_cast<DRegister>(reg_ * 2 + i);
  }

  inline SRegister s(intptr_t i) const {
    return static_cast<SRegister>(reg_ * 4 + i);
  }

  explicit QRegister_(QRegister reg) : reg_(reg) {}

  operator QRegister() const { return reg_; }

 private:
  QRegister reg_;
};

template <QRegister reg>
struct Fixed_ {
  inline DRegister d(intptr_t i) const {
    return static_cast<DRegister>(reg * 2 + i);
  }

  inline SRegister s(intptr_t i) const {
    return static_cast<SRegister>(reg * 4 + i);
  }

  operator QRegister() const { return reg; }
};

template <>
struct UnwrapLocation<QRegister_> {
  static const bool kIsTemp = false;

  static QRegister_ Unwrap(const Location& loc) {
    return QRegister_(loc.fpu_reg());
  }

  template <intptr_t arity, intptr_t index>
  static QRegister_ Unwrap(LocationSummary* locs) {
    return Unwrap(locs->in(index));
  }

  template <intptr_t arity, intptr_t index>
  static void SetConstraint(LocationSummary* locs) {
    locs->set_in(index, ToConstraint());
  }

  static Location ToConstraint() { return Location::RequiresFpuRegister(); }
};

template <QRegister reg>
struct UnwrapLocation<Fixed_<reg> > {
  static const bool kIsTemp = false;

  static Fixed_<reg> Unwrap(const Location& loc) {
    assert(UnwrapLocation<QRegister>::Unwrap(loc) == reg);
    return Fixed_<reg>();
  }

  template <intptr_t arity, intptr_t index>
  static Fixed_<reg> Unwrap(LocationSummary* locs) {
    return Unwrap(locs->in(index));
  }

  template <intptr_t arity, intptr_t index>
  static void SetConstraint(LocationSummary* locs) {
    locs->set_in(index, ToConstraint());
  }

  static Location ToConstraint() { return Location::FpuRegisterLocation(reg); }
};

}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_ARM_H_
