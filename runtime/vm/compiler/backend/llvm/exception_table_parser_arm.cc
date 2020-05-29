// Copyright 2019 UCWeb Co., Ltd.

#include "vm/compiler/backend/llvm/exception_table_parser.h"
#if defined(DART_ENABLE_LLVM_COMPILER) && defined(TARGET_ARCH_ARM)
#include "vm/compiler/backend/llvm/llvm_log.h"
#include "vm/compiler/backend/llvm/stack_maps.h"

namespace dart {
namespace dart_llvm {
ExceptionTableParser::ExceptionTableParser(const uint8_t* content,
                                           size_t length) {
  unsigned offset = 0;
  const uint8_t* end = content + length;
  DataView view(content);
  // omit first two words
  view.read<int32_t>(offset, true);
  view.read<int32_t>(offset, true);
  static const uint8_t kDW_EH_PE_omit = 0xff;
  static const uint8_t kDW_EH_PE_uleb128 = 0x01;
  uint8_t lp_start = view.read<uint8_t>(offset, true);
  uint8_t ttype = view.read<uint8_t>(offset, true);
  uint8_t callsite_encoding = view.read<uint8_t>(offset, true);
  EMASSERT(lp_start == kDW_EH_PE_omit);
  EMASSERT(ttype == kDW_EH_PE_omit);
  EMASSERT(callsite_encoding == kDW_EH_PE_uleb128);
  uint64_t landing_pad_size = view.ReadULEB128(offset, end);
  EMASSERT(offset + landing_pad_size <= length);
  // Update end.
  end = content + offset + landing_pad_size;
  while (content + offset < end) {
    uint64_t call_begin = view.ReadULEB128(offset, end);
    uint64_t call_length = view.ReadULEB128(offset, end);
    uint64_t landing_pad = view.ReadULEB128(offset, end);
    uint64_t action = view.ReadULEB128(offset, end);
    EMASSERT(action == 0);  // Only allows cleanup.
    if (landing_pad) {
      records_.emplace_back(call_begin + call_length, landing_pad);
    }
  }
}
}  // namespace dart_llvm
}  // namespace dart

#endif  // DART_ENABLE_LLVM_COMPILER && TARGET_ARCH_ARM
