// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_H_
#define RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_H_

namespace dart {

struct SameAsFirstInput {};

template <typename RegisterT, RegisterT t>
struct Fixed {
  operator RegisterT() { return t; }
};

template <typename RegisterT>
struct Temp {
  explicit Temp(RegisterT reg) : reg_(reg) {}

  operator RegisterT() { return reg_; }

  RegisterT reg_;
};

template <typename T>
struct UnwrapLocation;

template <>
struct UnwrapLocation<Register> {
  static const bool kIsTemp = false;

  static Register Unwrap(const Location& loc) { return loc.reg(); }
  template <intptr_t arity, intptr_t index>
  static Register Unwrap(LocationSummary* locs) {
    return Unwrap(locs->in(index));
  }

  template <intptr_t arity, intptr_t index>
  static void SetConstraint(LocationSummary* locs) {
    locs->set_in(index, ToConstraint());
  }

  static Location ToConstraint() { return Location::RequiresRegister(); }
  static Location ToFixedConstraint(Register reg) {
    return Location::RegisterLocation(reg);
  }
};

template <>
struct UnwrapLocation<FpuRegister> {
  static const bool kIsTemp = false;

  static FpuRegister Unwrap(const Location& loc) { return loc.fpu_reg(); }

  template <intptr_t arity, intptr_t index>
  static FpuRegister Unwrap(LocationSummary* locs) {
    return Unwrap(locs->in(index));
  }

  template <intptr_t arity, intptr_t index>
  static void SetConstraint(LocationSummary* locs) {
    locs->set_in(index, ToConstraint());
  }

  static Location ToConstraint() { return Location::RequiresFpuRegister(); }
  static Location ToFixedConstraint(FpuRegister reg) {
    return Location::FpuRegisterLocation(reg);
  }
};

template <typename RegisterType, RegisterType reg>
struct UnwrapLocation<Fixed<RegisterType, reg> > {
  static const bool kIsTemp = false;

  static Fixed<RegisterType, reg> Unwrap(const Location& loc) {
    assert(UnwrapLocation<RegisterType>::Unwrap(loc) == reg);
    return Fixed<RegisterType, reg>();
  }

  template <intptr_t arity, intptr_t index>
  static Fixed<RegisterType, reg> Unwrap(LocationSummary* locs) {
    return Unwrap(locs->in(index));
  }

  template <intptr_t arity, intptr_t index>
  static void SetConstraint(LocationSummary* locs) {
    locs->set_in(index, ToConstraint());
  }

  static Location ToConstraint() {
    return UnwrapLocation<RegisterType>::ToFixedConstraint(reg);
  }
};

template <typename RegisterType>
struct UnwrapLocation<Temp<RegisterType> > {
  static const bool kIsTemp = true;

  static Temp<RegisterType> Unwrap(const Location& loc) {
    return Temp<RegisterType>(UnwrapLocation<RegisterType>::Unwrap(loc));
  }

  template <intptr_t arity, intptr_t index>
  static Temp<RegisterType> Unwrap(LocationSummary* locs) {
    return Unwrap(locs->temp(index - arity));
  }

  template <intptr_t arity, intptr_t index>
  static void SetConstraint(LocationSummary* locs) {
    locs->set_temp(index - arity, ToConstraint());
  }

  static Location ToConstraint() {
    return UnwrapLocation<RegisterType>::ToConstraint();
  }
};

template <>
struct UnwrapLocation<SameAsFirstInput> {
  static const bool kIsTemp = false;

  static SameAsFirstInput Unwrap(const Location& loc) {
    return SameAsFirstInput();
  }

  static Location ToConstraint() { return Location::SameAsFirstInput(); }
};

#define UNPACK(...) __VA_ARGS__

#define TYPE_LIST_0() Nil
#define TYPE_LIST_1(T0) Cons<T0, TYPE_LIST_0()>
#define TYPE_LIST_2(T0, ...) Cons<T0, TYPE_LIST_1(__VA_ARGS__)>
#define TYPE_LIST_3(T0, ...) Cons<T0, TYPE_LIST_2(__VA_ARGS__)>
#define TYPE_LIST_4(T0, ...) Cons<T0, TYPE_LIST_3(__VA_ARGS__)>
#define TYPE_LIST_5(T0, ...) Cons<T0, TYPE_LIST_4(__VA_ARGS__)>

#define SIGNATURE_INFO_TYPE(Arity, ...)                                        \
  SignatureInfo<TYPE_LIST_##Arity(__VA_ARGS__)>

struct Nil;

template <typename T, typename U>
struct Cons {};

template <typename T>
struct SignatureInfo;

template <>
struct SignatureInfo<Nil> {
  enum { kArity = 0, kTempCount = 0, kInputCount = kArity - kTempCount };

  template <intptr_t kArity, intptr_t kOffset>
  static void SetConstraints(LocationSummary* locs) {}
};

template <typename T0, typename Tx>
struct SignatureInfo<Cons<T0, Tx> > {
  typedef SignatureInfo<Tx> Tail;

  enum {
    kArity = 1 + Tail::kArity,
    kTempCount = (UnwrapLocation<T0>::kIsTemp ? 1 : 0) + Tail::kTempCount,
    kInputCount = kArity - kTempCount
  };

  template <intptr_t kArity, intptr_t kOffset>
  static void SetConstraints(LocationSummary* locs) {
    UnwrapLocation<T0>::template SetConstraint<kArity, kOffset>(locs);
    Tail::template SetConstraints<kArity, kOffset + 1>(locs);
  }
};

template <typename Instr, typename Out>
LocationSummary* MakeLocationSummaryFromEmitter(Zone* zone,
                                                const Instr* instr,
                                                void (*Emit)(FlowGraphCompiler*,
                                                             Instr*,
                                                             Out)) {
  typedef SIGNATURE_INFO_TYPE(0) SignatureT;
  ASSERT(instr->InputCount() == SignatureT::kInputCount);
  LocationSummary* summary = new (zone)
      LocationSummary(zone, SignatureT::kInputCount, SignatureT::kTempCount,
                      LocationSummary::kNoCall);
  summary->set_out(0, UnwrapLocation<Out>::ToConstraint());
  return summary;
}

#define DEFINE_MAKE_LOCATION_SUMMARY_SPECIALIZATION(Arity, Types)              \
  LocationSummary* MakeLocationSummaryFromEmitter(                             \
      Zone* zone, const Instr* instr,                                          \
      void (*Emit)(FlowGraphCompiler*, Instr*, Out, UNPACK Types)) {           \
    typedef SIGNATURE_INFO_TYPE(Arity, UNPACK Types) SignatureT;               \
    ASSERT(instr->InputCount() == SignatureT::kInputCount);                    \
    LocationSummary* summary = new (zone)                                      \
        LocationSummary(zone, SignatureT::kInputCount, SignatureT::kTempCount, \
                        LocationSummary::kNoCall);                             \
    SignatureT::template SetConstraints<SignatureT::kInputCount, 0>(summary);  \
    summary->set_out(0, UnwrapLocation<Out>::ToConstraint());                  \
    return summary;                                                            \
  }

template <typename Instr, typename Out, typename T0>
DEFINE_MAKE_LOCATION_SUMMARY_SPECIALIZATION(1, (T0));

template <typename Instr, typename Out, typename T0, typename T1>
DEFINE_MAKE_LOCATION_SUMMARY_SPECIALIZATION(2, (T0, T1));

template <typename Instr, typename Out, typename T0, typename T1, typename T2>
DEFINE_MAKE_LOCATION_SUMMARY_SPECIALIZATION(3, (T0, T1, T2));

template <typename Instr,
          typename Out,
          typename T0,
          typename T1,
          typename T2,
          typename T3>
DEFINE_MAKE_LOCATION_SUMMARY_SPECIALIZATION(4, (T0, T1, T2, T3));

template <typename Instr,
          typename Out,
          typename T0,
          typename T1,
          typename T2,
          typename T3,
          typename T4>
DEFINE_MAKE_LOCATION_SUMMARY_SPECIALIZATION(5, (T0, T1, T2, T3, T4));

template <typename Instr, typename Out>
void InvokeEmitter(FlowGraphCompiler* compiler,
                   Instr* instr,
                   void (*Emit)(FlowGraphCompiler*, Instr*, Out)) {
  typedef SIGNATURE_INFO_TYPE(0) SignatureT;
  ASSERT(instr->InputCount() == SignatureT::kInputCount);
  LocationSummary* locs = instr->locs();
  Emit(compiler, instr, UnwrapLocation<Out>::Unwrap(locs->out(0)));
}

template <typename Instr, typename Out, typename T0>
void InvokeEmitter(FlowGraphCompiler* compiler,
                   Instr* instr,
                   void (*Emit)(FlowGraphCompiler*, Instr*, Out, T0)) {
  typedef SIGNATURE_INFO_TYPE(1, T0) SignatureT;
  ASSERT(instr->InputCount() == SignatureT::kInputCount);
  LocationSummary* locs = instr->locs();
  Emit(compiler, instr, UnwrapLocation<Out>::Unwrap(locs->out(0)),
       UnwrapLocation<T0>::template Unwrap<SignatureT::kInputCount, 0>(locs));
}

template <typename Instr, typename Out, typename T0, typename T1>
void InvokeEmitter(FlowGraphCompiler* compiler,
                   Instr* instr,
                   void (*Emit)(FlowGraphCompiler*, Instr*, Out, T0, T1)) {
  typedef SIGNATURE_INFO_TYPE(2, T0, T1) SignatureT;
  ASSERT(instr->InputCount() == SignatureT::kInputCount);
  LocationSummary* locs = instr->locs();
  Emit(compiler, instr, UnwrapLocation<Out>::Unwrap(locs->out(0)),
       UnwrapLocation<T0>::template Unwrap<SignatureT::kInputCount, 0>(locs),
       UnwrapLocation<T1>::template Unwrap<SignatureT::kInputCount, 1>(locs));
}

template <typename Instr, typename Out, typename T0, typename T1, typename T2>
void InvokeEmitter(FlowGraphCompiler* compiler,
                   Instr* instr,
                   void (*Emit)(FlowGraphCompiler*, Instr*, Out, T0, T1, T2)) {
  typedef SIGNATURE_INFO_TYPE(3, T0, T1, T2) SignatureT;
  ASSERT(instr->InputCount() == SignatureT::kInputCount);
  LocationSummary* locs = instr->locs();
  Emit(compiler, instr, UnwrapLocation<Out>::Unwrap(locs->out(0)),
       UnwrapLocation<T0>::template Unwrap<SignatureT::kInputCount, 0>(locs),
       UnwrapLocation<T1>::template Unwrap<SignatureT::kInputCount, 1>(locs),
       UnwrapLocation<T2>::template Unwrap<SignatureT::kInputCount, 2>(locs));
}

template <typename Instr,
          typename Out,
          typename T0,
          typename T1,
          typename T2,
          typename T3>
void InvokeEmitter(
    FlowGraphCompiler* compiler,
    Instr* instr,
    void (*Emit)(FlowGraphCompiler*, Instr*, Out, T0, T1, T2, T3)) {
  typedef SIGNATURE_INFO_TYPE(4, T0, T1, T2, T3) SignatureT;
  ASSERT(instr->InputCount() == SignatureT::kInputCount);
  LocationSummary* locs = instr->locs();
  Emit(compiler, instr, UnwrapLocation<Out>::Unwrap(locs->out(0)),
       UnwrapLocation<T0>::template Unwrap<SignatureT::kInputCount, 0>(locs),
       UnwrapLocation<T1>::template Unwrap<SignatureT::kInputCount, 1>(locs),
       UnwrapLocation<T2>::template Unwrap<SignatureT::kInputCount, 2>(locs),
       UnwrapLocation<T3>::template Unwrap<SignatureT::kInputCount, 3>(locs));
}

template <typename Instr,
          typename Out,
          typename T0,
          typename T1,
          typename T2,
          typename T3,
          typename T4>
void InvokeEmitter(
    FlowGraphCompiler* compiler,
    Instr* instr,
    void (*Emit)(FlowGraphCompiler*, Instr*, Out, T0, T1, T2, T3, T4)) {
  typedef SIGNATURE_INFO_TYPE(5, T0, T1, T2, T3, T4) SignatureT;
  ASSERT(instr->InputCount() == SignatureT::kInputCount);
  LocationSummary* locs = instr->locs();
  Emit(compiler, instr, UnwrapLocation<Out>::Unwrap(locs->out(0)),
       UnwrapLocation<T0>::template Unwrap<SignatureT::kInputCount, 0>(locs),
       UnwrapLocation<T1>::template Unwrap<SignatureT::kInputCount, 1>(locs),
       UnwrapLocation<T2>::template Unwrap<SignatureT::kInputCount, 2>(locs),
       UnwrapLocation<T3>::template Unwrap<SignatureT::kInputCount, 3>(locs),
       UnwrapLocation<T4>::template Unwrap<SignatureT::kInputCount, 4>(locs));
}

}  // namespace dart

#if defined(TARGET_ARCH_IA32)

#elif defined(TARGET_ARCH_X64)

#elif defined(TARGET_ARCH_ARM)
#include "vm/compiler/assembler/assembler_arm.h"
#elif defined(TARGET_ARCH_ARM64)

#elif defined(TARGET_ARCH_DBC)

#else
#error Unknown architecture.
#endif

#endif  // RUNTIME_VM_COMPILER_BACKEND_LOCATIONS_HELPERS_H_