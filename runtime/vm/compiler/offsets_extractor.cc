// Copyright (c) 2019, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include <iostream>

#include "vm/compiler/runtime_api.h"
#include "vm/compiler/runtime_offsets_list.h"
#include "vm/dart_api_state.h"
#include "vm/dart_entry.h"
#include "vm/longjump.h"
#include "vm/native_arguments.h"
#include "vm/native_entry.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/runtime_entry.h"
#include "vm/symbols.h"
#include "vm/timeline.h"

#if defined(PRODUCT)
#define PRODUCT_DEF "defined(PRODUCT)"
#else
#define PRODUCT_DEF "!defined(PRODUCT)"
#endif

#if defined(TARGET_ARCH_ARM)
#define ARCH_DEF_CPU "defined(TARGET_ARCH_ARM)"
#elif defined(TARGET_ARCH_X64)
#define ARCH_DEF_CPU "defined(TARGET_ARCH_X64)"
#elif defined(TARGET_ARCH_IA32)
#define ARCH_DEF_CPU "defined(TARGET_ARCH_IA32)"
#elif defined(TARGET_ARCH_ARM64)
#define ARCH_DEF_CPU "defined(TARGET_ARCH_ARM64)"
#elif defined(TARGET_ARCH_RISCV32)
#define ARCH_DEF_CPU "defined(TARGET_ARCH_RISCV32)"
#elif defined(TARGET_ARCH_RISCV64)
#define ARCH_DEF_CPU "defined(TARGET_ARCH_RISCV64)"
#else
#error Unknown architecture
#endif

#if defined(DART_COMPRESSED_POINTERS)
#define COMPRESSED_DEF "defined(DART_COMPRESSED_POINTERS)"
#else
#define COMPRESSED_DEF "!defined(DART_COMPRESSED_POINTERS)"
#endif

#define PREPROCESSOR_CONDITION                                                 \
  "#if " PRODUCT_DEF " && " ARCH_DEF_CPU " && " COMPRESSED_DEF

#define PREPROCESSOR_CONDITION_END                                             \
  "#endif  // " PRODUCT_DEF " && \n        // " ARCH_DEF_CPU                   \
  " && \n        // " COMPRESSED_DEF

namespace dart {

#define PRINT_ARRAY_SIZEOF(...)

#define PRINT_PAYLOAD_SIZEOF(...)

#define PRINT_FIELD_OFFSET(Class, Name)                                        \
  extern "C" const intptr_t EXTRACTED_##Class##_##Name = Class::Name();

#define PRINT_ARRAY_LAYOUT(Class, Name)                                        \
  extern "C" const intptr_t EXTRACTED_##Class##_elements_start_offset =        \
      Class::ArrayTraits::elements_start_offset();                             \
  extern "C" const intptr_t EXTRACTED_##Class##_element_size =                 \
      Class::ArrayTraits::kElementSize;

#define PRINT_SIZEOF(Class, Name, What)                                        \
  extern "C" const intptr_t EXTRACTED_##Class##_##Name = sizeof(What);

#define RANGE_ELEM(I, Class, Name, Type, First, Last, Filter)                  \
  (((static_cast<int>(First) <= (I)) && ((I) <= static_cast<int>(Last)) &&     \
    Filter((I)))                                                               \
       ? Class::Name(static_cast<Type>(I))                                     \
       : -1)

#define RANGE_ELEM10(I, Class, Name, Type, First, Last, Filter)                \
  RANGE_ELEM(I * 10 + 0, Class, Name, Type, First, Last, Filter),              \
      RANGE_ELEM(I * 10 + 1, Class, Name, Type, First, Last, Filter),          \
      RANGE_ELEM(I * 10 + 2, Class, Name, Type, First, Last, Filter),          \
      RANGE_ELEM(I * 10 + 3, Class, Name, Type, First, Last, Filter),          \
      RANGE_ELEM(I * 10 + 4, Class, Name, Type, First, Last, Filter),          \
      RANGE_ELEM(I * 10 + 5, Class, Name, Type, First, Last, Filter),          \
      RANGE_ELEM(I * 10 + 6, Class, Name, Type, First, Last, Filter),          \
      RANGE_ELEM(I * 10 + 7, Class, Name, Type, First, Last, Filter),          \
      RANGE_ELEM(I * 10 + 8, Class, Name, Type, First, Last, Filter),          \
      RANGE_ELEM(I * 10 + 9, Class, Name, Type, First, Last, Filter)

#define PRINT_RANGE(Class, Name, Type, First, Last, Filter)                    \
  extern "C" constexpr intptr_t EXTRACTED_##Class##_##Name##_FirstIndex =      \
      static_cast<intptr_t>(First);                                            \
  extern "C" constexpr intptr_t EXTRACTED_##Class##_##Name##_LastIndex =       \
      static_cast<intptr_t>(Last);                                             \
  static_assert((EXTRACTED_##Class##_##Name##_LastIndex -                      \
                 EXTRACTED_##Class##_##Name##_FirstIndex) <= 40);              \
  extern "C" const intptr_t EXTRACTED_##Class##_##Name[] = {                   \
      RANGE_ELEM10(0, Class, Name, Type, First, Last, Filter),                 \
      RANGE_ELEM10(1, Class, Name, Type, First, Last, Filter),                 \
      RANGE_ELEM10(2, Class, Name, Type, First, Last, Filter),                 \
      RANGE_ELEM10(3, Class, Name, Type, First, Last, Filter),                 \
  };

#define PRINT_CONSTANT(Class, Name)                                            \
  extern "C" const intptr_t EXTRACTED_##Class##_##Name = Class::Name;

JIT_OFFSETS_LIST(PRINT_FIELD_OFFSET,
                 PRINT_ARRAY_LAYOUT,
                 PRINT_SIZEOF,
                 PRINT_ARRAY_SIZEOF,
                 PRINT_PAYLOAD_SIZEOF,
                 PRINT_RANGE,
                 PRINT_CONSTANT)

COMMON_OFFSETS_LIST(PRINT_FIELD_OFFSET,
                    PRINT_ARRAY_LAYOUT,
                    PRINT_SIZEOF,
                    PRINT_ARRAY_SIZEOF,
                    PRINT_PAYLOAD_SIZEOF,
                    PRINT_RANGE,
                    PRINT_CONSTANT)

#undef PRINT_FIELD_OFFSET
#undef PRINT_ARRAY_LAYOUT
#undef PRINT_SIZEOF
#undef PRINT_RANGE
#undef PRINT_CONSTANT
#undef PRINT_ARRAY_SIZEOF
#undef PRINT_PAYLOAD_SIZEOF

}  // namespace dart
