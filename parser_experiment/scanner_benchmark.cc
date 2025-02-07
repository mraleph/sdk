// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "util.h"

#include <chrono>
#include <format>
#include <print>
#include <cstring>

#include "scanner.h"
#include "token.h"

#include "keyword_state.h"

int main(int argc, char* argv[]) {
  if (argc == 3 && strcmp(argv[1], "parse") == 0) {
    auto data = LoadFileBytes(argv[2]);
    auto tok = ScanUtf8(data.data.get(), data.size);
    for (auto curr = tok; curr != nullptr; curr = curr->next()) {
      std::print("{}\n", *curr);
    }
    return 0;
  }

  auto input_files = LoadFileLines("parser_experiment/input.list");
  input_files.resize(3600);

  std::vector<Buffer> input_data;
  size_t total_bytes = 0;
  size_t count = 0;
  for (auto& path : input_files) {
    auto data = LoadFileBytes(path.substr(3));
    total_bytes += data.size;
    input_data.emplace_back(std::move(data));
    count++;
  }
  std::print("loaded {} bytes from {} files\n", total_bytes, count);


  const bool run_forever = argc == 2 && strcmp(argv[1], "forever") == 0;
  do {
    auto start = std::chrono::system_clock::now();
    for (auto& buf : input_data) {
      ScanUtf8(buf.data.get(), buf.size);
    }
    auto end = std::chrono::system_clock::now();
    auto ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::print(
        "C++ took {} to scan {} bytes: {} ns/byte\n", std::chrono::duration_cast<std::chrono::microseconds>(end - start), total_bytes, ns/total_bytes);
  } while (run_forever);


  return 0;
}