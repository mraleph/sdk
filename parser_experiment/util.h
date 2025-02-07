// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef UTIL_H
#define UTIL_H

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#define UNIMPLEMENTED()                                                        \
  do {                                                                         \
    ReportUnimplemented(__FUNCTION__, __FILE__, __LINE__);                     \
    __builtin_unreachable();                                                   \
  } while (0)

struct Buffer {
  std::unique_ptr<uint8_t[]> data;
  size_t size;
  size_t aligned_size;
};

Buffer LoadFileBytes(const std::string& path);

std::vector<std::string> LoadFileLines(const std::string& path);

[[noreturn]] void ReportUnimplemented(const char* func,
                                      const char* file,
                                      int line);

#endif