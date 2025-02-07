// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "util.h"

#include <cstring>
#include <fstream>
#include <print>

[[noreturn]] void ReportUnimplemented(const char* func,
                                      const char* file,
                                      int line) {
  std::print(stderr, "UNIMPLEMENTED {} at {}:{}\n", func, file, line);
  abort();
}

Buffer LoadFileBytes(const std::string& path) {
  std::ifstream is(path);
  is.seekg(0, std::ios_base::end);
  const auto size = static_cast<size_t>(is.tellg());
  const auto aligned_size = (size + 63) & ~63;

  is.seekg(0, std::ios_base::beg);

  std::unique_ptr<uint8_t[]> buf{new (std::align_val_t(64))
                                     uint8_t[aligned_size]};
  is.read(reinterpret_cast<char*>(buf.get()), size);
  if (aligned_size != size) {
    memset(buf.get() + size, 0, aligned_size - size);
  }
  is.close();
  return {std::move(buf), size, aligned_size};
}

std::vector<std::string> LoadFileLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    lines.push_back(std::move(line));
  }
  return lines;
}
