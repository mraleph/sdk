// Copyright 2019 UCWeb Co., Ltd.

#include "vm/compiler/backend/llvm/intrinsic_repository.h"
#if defined(DART_ENABLE_LLVM_COMPILER)

namespace dart {
namespace dart_llvm {

IntrinsicRepository::IntrinsicRepository(LContext context, LModule module)
    : CommonValues(context)
#define INTRINSIC_INITIALIZATION(ourName, llvmName, type) , m_##ourName(0)
          FOR_EACH_FTL_INTRINSIC(INTRINSIC_INITIALIZATION)
#undef INTRINSIC_INITIALIZATION
{
  initialize(module);
}

#define INTRINSIC_GETTER_SLOW_DEFINITION(ourName, llvmName, type)              \
  LValue IntrinsicRepository::ourName##IntrinsicSlow() {                       \
    m_##ourName = addExternFunction(module_, llvmName, type);                  \
    return m_##ourName;                                                        \
  }
FOR_EACH_FTL_INTRINSIC(INTRINSIC_GETTER_SLOW_DEFINITION)
#undef INTRINSIC_GETTER
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
