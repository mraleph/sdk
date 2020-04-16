// Copyright 2019 UCWeb Co., Ltd.
#ifndef EXCEPTION_TABLE_PARSER_H
#define EXCEPTION_TABLE_PARSER_H

#include "vm/compiler/backend/llvm/llvm_config.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include <stddef.h>
#include <stdint.h>

#include <tuple>
#include <vector>

namespace dart {
namespace dart_llvm {
class ExceptionTableParser {
 public:
  ExceptionTableParser(const uint8_t*, size_t);
  ~ExceptionTableParser() = default;
  const std::vector<std::tuple<int, int>>& CallSiteHandlerPairs() const {
    return records_;
  }

 private:
  std::vector<std::tuple<int, int>> records_;
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // EXCEPTION_TABLE_PARSER_H
