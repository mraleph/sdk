// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "keyword_state.h"

#include <algorithm>
#include <cassert>
#include <print>

KeywordState* KeywordState::KEYWORD_STATE = nullptr;

KeywordState* KeywordState::ComputeKeywordStateTable(
    size_t start,
    const std::vector<std::pair<TokenType, std::string_view>>& strings,
    size_t offset,
    size_t length) {
  bool isLowercase = true;

  std::array<KeywordState*, $z - $A + 1> table{};
  int chunk = 0;
  size_t chunkStart = static_cast<size_t>(-1);
  bool isLeaf = false;
  for (size_t i = offset; i < offset + length; i++) {
    if (strings[i].second.size() == start) {
      isLeaf = true;
    }
    if (strings[i].second.size() > start) {
      int c = strings[i].second[start];
      if ($A <= c && c <= $Z) {
        isLowercase = false;
      }
      if (chunk != c) {
        if (chunkStart != static_cast<size_t>(-1)) {
          assert(table[chunk - $A] == nullptr);
          table[chunk - $A] = ComputeKeywordStateTable(
              start + 1, strings, chunkStart, i - chunkStart);
        }
        chunkStart = i;
        chunk = c;
      }
    }
  }
  if (chunkStart != static_cast<size_t>(-1)) {
    assert(table[chunk - $A] == nullptr);
    table[chunk - $A] = ComputeKeywordStateTable(start + 1, strings, chunkStart,
                                                 offset + length - chunkStart);
  } else {
    assert(length == 1);
    return new LeafKeywordState(strings[offset].first);
  }
  TokenType tok = isLeaf ? strings[offset].first : TokenType::kEOF;
  if (isLowercase) {
    std::array<KeywordState*, $z - $a + 1> lower_case_table;
    std::copy(std::begin(table) + ($a - $A), std::end(table), std::begin(lower_case_table));
    return new RangeKeywordState<$a, $z>(std::move(lower_case_table), tok);
  } else {
    return new RangeKeywordState<$A, $z>(std::move(table), tok);
  }
}

void DumpTable(KeywordState* state, intptr_t indent, int A, int Z, KeywordState** table) {
  std::print("({})", (void*) state);
  for (int ch = A; ch <= Z; ch++) {
    if (table[ch - A] != nullptr) {
      std::print("\n");
      for (intptr_t i = 0; i < indent; i++) std::print("  ");
      std::print("{}:", (char)ch);
      table[ch - A]->Dump(indent + 1);
    }
  }
  if (indent == 0) {
    std::print("\n");
  }
}

void LeafKeywordState::Dump(intptr_t indent) {
  std::print("({}) {}", (void*) this, keyword);
  if (indent == 0) {
    std::print("\n");
  }
}


void KeywordState::Init() {
  std::vector<std::pair<TokenType, std::string_view>> strings = {
#define K(Name, lexeme) {TokenType::k##Name, lexeme},
      KEYWORD_LIST(K)
#undef K
  };
  std::sort(std::begin(strings), std::end(strings),
            [](auto a, auto b) { return a.second < b.second; });

  // TODO(fill and sort)
  KEYWORD_STATE = ComputeKeywordStateTable(
      /* start = */ 0, strings,
      /* offset = */ 0, strings.size());

  // KEYWORD_STATE->Dump(0);
}
