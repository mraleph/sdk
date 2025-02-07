// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include <immintrin.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <print>
#include <ranges>
#include <utility>
#include <vector>

/*

{0x24} -> Set1

{0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39
0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,} -> Set2

{0x4A, 0x4B, 0x4C, 0x4D, 0x4F,
 0x6A, 0x6B, 0x6C, 0x6D, 0x6F} -> Set3

{0x5A, 0x5F} -> Set4

{0x7A} -> Set5

{0x30} -> Set6

*/

namespace {
struct ClassificationTables {
  uint8_t lo_[32] = {};
  uint8_t hi_[32] = {};

  constexpr __m256i lo() const {
    return _mm256_load_si256(reinterpret_cast<const __m256i*>(&lo_[0]));
  }
  constexpr __m256i hi() const {
    return _mm256_load_si256(reinterpret_cast<const __m256i*>(&hi_[0]));
  }
};

[[clang::always_inline]] constexpr ClassificationTables CreateTables(
    std::initializer_list<std::initializer_list<uint8_t>> sets) {
  ClassificationTables tbls;

  uint8_t bit = 1;
  for (auto& set : sets) {
    for (auto v : set) {
      tbls.lo_[v & 0xF] |= bit;
      tbls.hi_[v >> 4] |= bit;
    }
    bit <<= 1;
  }

  for (int i = 0; i < 16; i++) {
    tbls.lo_[i + 16] = tbls.lo_[i];
    tbls.hi_[i + 16] = tbls.hi_[i];
  }
  return tbls;
}
}  // namespace

struct simd8 {
  uint8_t values[32];
};

void print(__m256i v) {
  simd8 bytes = *reinterpret_cast<simd8*>(&v);
  for (int i = 0; i < 32; i++) {
    std::printf("%02x ", bytes.values[i]);
  }
  std::printf("\n");
}

void printBinary(uint32_t v) {
  for (int i = 0; i < 32; i++) {
    std::printf((v & (1 << i)) ? "1" : "0");
  }
  std::printf("\n");
}

void printBinary(uint64_t v) {
  for (int i = 0; i < 64; i++) {
    std::printf((v & (static_cast<uint64_t>(1) << i)) ? "1" : "0");
  }
  std::printf("\n");
}

constexpr auto kClassificationTables = CreateTables(
    {{0x24},
     {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x41, 0x42, 0x43,
      0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56,
      0x57, 0x58, 0x59, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
      0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79},
     {0x4A, 0x4B, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x6A, 0x6B, 0x6B, 0x6C, 0x6D,
      0x6E, 0x6F},
     {0x50, 0x5A, 0x5F},
     {0x7A, 0x70},
     {0x30}});

[[clang::always_inline]] uint32_t Classify(__m256i chars) {
  auto low_nibs_classified =
      _mm256_shuffle_epi8(kClassificationTables.lo(), chars);
  auto hi_nibs_classified = _mm256_shuffle_epi8(
      kClassificationTables.hi(),
      _mm256_and_si256(_mm256_srli_epi16(chars, 4), _mm256_set1_epi8(0x0F)));
  auto chars_classified =
      _mm256_and_si256(low_nibs_classified, hi_nibs_classified);
  return _mm256_movemask_epi8(
      _mm256_cmpgt_epi8(chars_classified, _mm256_setzero_si256()));
}

std::vector<std::pair<uint32_t, uint32_t>> ScanUsingSimd(uint8_t* content,
                                                         size_t length) {
  std::vector<std::pair<uint32_t, uint32_t>> result(0);
  result.reserve(100);

  uint32_t starts[65] = {0};

  size_t base = 0;
  uint64_t state = 0;
  while (base < length) {
    __m256i a =
        _mm256_load_si256(reinterpret_cast<const __m256i*>(content + base));
    __m256i b = _mm256_load_si256(
        reinterpret_cast<const __m256i*>(content + base + 32));
    uint64_t bits = static_cast<uint64_t>(Classify(a)) |
                    (static_cast<uint64_t>(Classify(b)) << 32);
    uint64_t boundaries = bits ^ (bits << 1) ^ state;
    uint32_t* offsets = starts + state;
    int32_t offsets_cnt = __builtin_popcountl(boundaries) + state;
    while (boundaries) {
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
    }
    state = bits >> 63;
    if (offsets_cnt > 0) {
      for (int i = 0; i < offsets_cnt - 1; i += 2) {
        result.push_back(std::make_pair(starts[i], starts[i + 1]));
      }
      starts[0] = starts[offsets_cnt - 1];
    }
    base += 64;
  }

  if (state) {
    result.push_back(std::make_pair(starts[0], length));
  }

  return result;
}

void JustScanUsingSimd(uint8_t* content, size_t length) {
  uint32_t starts[65] = {0};

  size_t base = 0;
  uint64_t state = 0;
  while (base < length) {
    __m256i a =
        _mm256_load_si256(reinterpret_cast<const __m256i*>(content + base));
    __m256i b = _mm256_load_si256(
        reinterpret_cast<const __m256i*>(content + base + 32));
    uint64_t bits = static_cast<uint64_t>(Classify(a)) |
                    (static_cast<uint64_t>(Classify(b)) << 32);
    uint64_t boundaries = bits ^ (bits << 1) ^ state;
    uint32_t* offsets = starts + state;
    uint32_t offsets_cnt = __builtin_popcountl(boundaries) + state;
    while (boundaries) {
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
      *offsets++ = base + __builtin_ctzl(boundaries);
      boundaries = boundaries & (boundaries - 1);
    }
    state = bits >> 63;
    if (offsets_cnt > 0) {
      starts[0] = starts[offsets_cnt - 1];
    }
    base += 64;
  }
}

namespace {
constexpr std::array<uint8_t, 256> CreateIsIdenfierTable() {
  std::array<uint8_t, 256> tbl{};

  for (char c = 'a'; c <= 'z'; c++) {
    tbl[c] = 0x1;
  }

  for (char c = 'A'; c <= 'Z'; c++) {
    tbl[c] = 0x1;
  }

  for (char c = '0'; c <= '9'; c++) {
    tbl[c] = 0x1;
  }

  tbl['_'] = tbl['$'] = 0x1;

  return tbl;
}
}  // namespace

[[clang::always_inline]] bool IsIdentifierCharacter(uint8_t ch) {
  constexpr auto kIsIdentifierCharacterTable = CreateIsIdenfierTable();
  return kIsIdentifierCharacterTable[ch];
}

namespace {

struct Buffer {
  std::unique_ptr<uint8_t[]> data;
  size_t size;
};

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
  return {std::move(buf), aligned_size};
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
}  // namespace

[[clang::noinline]] std::vector<std::pair<uint32_t, uint32_t>> ScanUsingLoop(
    uint8_t* content,
    size_t length) {
  std::vector<std::pair<uint32_t, uint32_t>> result(0);
  result.reserve(100);

  size_t pos = 0;
  while (pos < length) {
    while (pos < length && !IsIdentifierCharacter(content[pos]))
      pos++;
    if (pos >= length) {
      break;
    }

    int start = pos;
    while (pos < length && IsIdentifierCharacter(content[pos]))
      pos++;
    result.push_back(std::make_pair(start, pos));
  }

  return result;
}

[[clang::noinline]] void JustScanUsingLoop(uint8_t* content, size_t length) {
  size_t pos = 0;
  size_t total_len = 0;
  while (pos < length) {
    while (pos < length && !IsIdentifierCharacter(content[pos]))
      pos++;
    if (pos >= length) {
      break;
    }

    int start = pos;
    while (pos < length && IsIdentifierCharacter(content[pos]))
      pos++;

    total_len += pos - start;
  }

  if (total_len == 42) {
    exit(1);
  }
}

bool CompareResults(std::vector<std::pair<uint32_t, uint32_t>>& a,
                    std::vector<std::pair<uint32_t, uint32_t>>& b) {
  if (a.size() != b.size()) {
    std::print("different number {} != {}\n", a.size(), b.size());
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i] != b[i]) {
      std::print("at {}: {} != {}\n", i, a[i], b[i]);
      return false;
    }
  }

  return true;
}

int main(int argc, char* argv[]) {
  const auto input_files = LoadFileLines("parser_experiment/input.list");

  {
    const size_t test_data_size = 256 * 2;
    std::unique_ptr<uint8_t[]> test_buf{new (std::align_val_t(64))
                                            uint8_t[test_data_size]};
    memset(test_buf.get(), 0, test_data_size);
    for (int i = 0; i < 256; i++) {
      test_buf[i * 2] = i;
    }

    auto a = ScanUsingSimd(test_buf.get(), test_data_size);
    auto b = ScanUsingLoop(test_buf.get(), test_data_size);

    if (!CompareResults(a, b)) {
      std::print("mismatch found in test data\n");

      for (size_t i = 0; auto& p : a) {
        std::print(
            "{}: {} {} {:x}\n", i, p,
            i < b.size() ? b[i] : std::make_pair<uint32_t, uint32_t>(0u, 0u),
            i < b.size() ? (char)test_buf[b[i].first] : ' ');
        i++;
      }

      return 1;
    }
  }

  std::vector<Buffer> input_data;
  size_t total_bytes = 0;
  size_t count = 0;
  for (auto& path : input_files) {
    auto data = LoadFileBytes(path.substr(3));
    total_bytes += data.size;
    input_data.emplace_back(std::move(data));
    count++;
    //if (total_bytes > 20 * 1024 * 1024) {
    //  break;
    //}
  }
  std::print("loaded {} bytes from {} files\n", total_bytes, count);

  {
    auto start = std::chrono::system_clock::now();
    for (auto& bytes : input_data) {
      JustScanUsingLoop(bytes.data.get(), bytes.size);
    }
    auto end = std::chrono::system_clock::now();
    std::print(
        "scanning using loop took: {}\n",
        std::chrono::duration_cast<std::chrono::microseconds>(end - start));
  }

  {
    auto start = std::chrono::system_clock::now();
    for (auto& bytes : input_data) {
      JustScanUsingSimd(bytes.data.get(), bytes.size);
    }
    auto end = std::chrono::system_clock::now();
    std::print(
        "scanning using simd took: {}\n",
        std::chrono::duration_cast<std::chrono::microseconds>(end - start));
  }

  if (false) {
    for (auto i = 0; auto& bytes : input_data) {
      auto a = ScanUsingSimd(bytes.data.get(), bytes.size);
      auto b = ScanUsingLoop(bytes.data.get(), bytes.size);

      if (!CompareResults(a, b)) {
        std::print("mismatch found in file {}\n", input_files[i]);
        break;
      }
      i++;
    }
  }

  /*
    alignas(64) char string[129] = "xyz asdasd as12 asd asdwe 2642 aasdd    asd dwe 2642 aasdd    as xyz asdasd as12 asd asdwe 2642 aasdd    asd dwe 2642 aasdd    a";
    const auto result1 = ScanUsingLoop(reinterpret_cast<uint8_t*>(string), 128);
    const auto result2 = ScanUsingLoop(reinterpret_cast<uint8_t*>(string), 128);

    if (result1.size() != result2.size()) {
      std::print("{} != {}\n", result1.size(), result2.size());
    } else {
      for (int i = 0; i < result1.size(); i++) {
        if (result1[i] != result2[i]) {
          std::print("{} {} != {}\n", i, result1[i], result2[i]);
        }
      }
    }

  */
  return 0;
}