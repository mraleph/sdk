// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_SCHEDULE_H_
#define RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_SCHEDULE_H_

#if defined(DART_PRECOMPILED_RUNTIME)
#error "AOT runtime should not use compiler sources (including header files)"
#endif  // defined(DART_PRECOMPILED_RUNTIME)

#include "vm/allocation.h"
#include "vm/compiler/backend/flow_graph_compiler.h"
#include "vm/compiler/backend/locations.h"
#include "vm/constants.h"

namespace dart {
namespace compiler {

// Simple dynamically allocated array of fixed length.
template <typename Subclass, typename Element>
class FixedArray {
 public:
  static Subclass& Allocate(intptr_t length) {
    static_assert(Utils::IsAligned(alignof(Subclass), alignof(Element)));
    auto result =
        reinterpret_cast<void*>(Thread::Current()->zone()->AllocUnsafe(
            sizeof(Subclass) + length * sizeof(Element)));
    return *new (result) Subclass(length);
  }

  intptr_t length() const { return length_; }

  Element& operator[](intptr_t i) {
    ASSERT(0 <= i && i < length_);
    return data()[i];
  }

  const Element& operator[](intptr_t i) const {
    ASSERT(0 <= i && i < length_);
    return data()[i];
  }

  Element* data() { OPEN_ARRAY_START(Element, Element); }
  const Element* data() const { OPEN_ARRAY_START(Element, Element); }

  Element* begin() { return data(); }
  const Element* begin() const { return data(); }

  Element* end() { return data() + length_; }
  const Element* end() const { return data() + length_; }

 protected:
  explicit FixedArray(intptr_t length) : length_(length) {}

 private:
  intptr_t length_;

  DISALLOW_COPY_AND_ASSIGN(FixedArray);
};


struct MoveOp {
  enum class Kind : uint8_t {
    kNop,
    kMove,
    kSwap,
  };

  Kind kind;
  MoveOperands operands;
};

class MoveSchedule : public FixedArray<MoveSchedule, MoveOp> {
 public:
  // Converts the given list of |ParallelMoveResolver::Op| operations
  // into a |MoveSchedule| and filters out all |kNop| operations.
  static const MoveSchedule& From(
      const GrowableArray<MoveOp>& ops) {
    intptr_t count = 0;
    for (const auto& op : ops) {
      if (op.kind != MoveOp::Kind::kNop) count++;
    }

    auto& result = FixedArray::Allocate(count);
    intptr_t i = 0;
    for (const auto& op : ops) {
      if (op.kind != MoveOp::Kind::kNop) {
        result[i++] = op;
      }
    }
    return result;
  }

 private:
  friend class FixedArray<MoveSchedule, MoveOp>;

  explicit MoveSchedule(intptr_t length) : FixedArray(length) {}

  DISALLOW_COPY_AND_ASSIGN(MoveSchedule);
};

}  // namespace compiler
}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_BACKEND_PARALLEL_MOVE_SCHEDULE_H_
