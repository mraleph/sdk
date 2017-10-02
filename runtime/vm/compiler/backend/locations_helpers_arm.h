// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// This file contains few helpful marker types that can be used with
// MakeLocationSummaryFromEmitter and InvokeEmitter to simplify writing
// of ARM code.

#ifndef RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_ARM_H_
#define RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_ARM_H_

namespace dart {

// QRegister_ is a wrapper around QRegister that provides helpers for accessing
// S and D components.
class QRegister_ {
 public:
  explicit QRegister_(QRegister reg) : reg_(reg) {}

  operator QRegister() const { return reg_; }

  inline DRegister d(intptr_t i) const {
    ASSERT(0 <= i && i < 2);
    return static_cast<DRegister>(reg_ * 2 + i);
  }

  inline SRegister s(intptr_t i) const {
    ASSERT(0 <= i && i < 4);
    return static_cast<SRegister>(reg_ * 4 + i);
  }

 private:
  QRegister reg_;
};

// Fixed_<r> is a handy replacement for Fixed<QRegister, r> that provides
// helpers for accessing S and D components.
template <QRegister reg>
class Fixed_ {
 public:
  inline DRegister d(intptr_t i) const {
    return static_cast<DRegister>(reg * 2 + i);
  }

  inline SRegister s(intptr_t i) const {
    return static_cast<SRegister>(reg * 4 + i);
  }

  operator QRegister() const { return reg; }
};

template <>
struct LocationTrait<QRegister_> {
  static const bool kIsTemp = false;

  static QRegister_ Unwrap(const Location& loc) {
    return QRegister_(loc.fpu_reg());
  }

  template <intptr_t arity, intptr_t index>
  static QRegister_ UnwrapInput(LocationSummary* locs) {
    return Unwrap(locs->in(index));
  }

  template <intptr_t arity, intptr_t index>
  static void SetInputConstraint(LocationSummary* locs) {
    locs->set_in(index, ToConstraint());
  }

  static Location ToConstraint() { return Location::RequiresFpuRegister(); }
};

template <QRegister reg>
struct LocationTrait<Fixed_<reg> > {
  static const bool kIsTemp = false;

  static Fixed_<reg> Unwrap(const Location& loc) {
    ASSERT(LocationTrait<QRegister>::Unwrap(loc) == reg);
    return Fixed_<reg>();
  }

  template <intptr_t arity, intptr_t index>
  static Fixed_<reg> UnwrapInput(LocationSummary* locs) {
    return Unwrap(locs->in(index));
  }

  template <intptr_t arity, intptr_t index>
  static void SetInputConstraint(LocationSummary* locs) {
    locs->set_in(index, ToConstraint());
  }

  static Location ToConstraint() { return Location::FpuRegisterLocation(reg); }
};

}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_ARM_H_
