// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef KEYWORD_STATE_H
#define KEYWORD_STATE_H

#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include "token.h"

/**
 * Abstract state in a state machine for scanning keywords.
 */
class KeywordState {
 public:
  virtual KeywordState* next(int c) = 0;
  virtual KeywordState* nextCapital(int c) = 0;

  static void Init();
  static KeywordState* KEYWORD_STATE;

  const TokenType keyword;

  virtual void Dump(intptr_t indent) = 0;

 protected:
  KeywordState(TokenType keyword) : keyword(keyword) {}

 private:
  static KeywordState* ComputeKeywordStateTable(
      size_t start,
      const std::vector<std::pair<TokenType, std::string_view>>& strings,
      size_t offset,
      size_t length);
};

/**
 * A state with multiple outgoing transitions.
 */
template <int N>
class ArrayKeywordState : public KeywordState {
 public:
  using Table = std::array<KeywordState*, N>;

  ArrayKeywordState(Table&& table, TokenType keyword)
      : KeywordState(keyword), table_(std::move(table)) {}

 protected:
  const Table table_;
};

void DumpTable(KeywordState* state,
               intptr_t indent,
               int A,
               int Z,
               KeywordState** table);

template <int A, int Z>
class RangeKeywordState final : public ArrayKeywordState<Z - A + 1> {
 private:
  using BaseType = ArrayKeywordState<Z - A + 1>;

 public:
  RangeKeywordState(BaseType::Table&& table, TokenType keyword)
      : BaseType(std::move(table), keyword) {}

  KeywordState* next(int c) override { return BaseType::table_[c - A]; }
  KeywordState* nextCapital(int c) override {
    return (A == $a) ? nullptr : BaseType::table_[c - A];
  }

  void Dump(intptr_t indent) override {
    DumpTable(this, indent, A, Z, (KeywordState**)BaseType::table_.data());
  }
};

/**
 * A state that has no outgoing transitions.
 */
class LeafKeywordState final : public KeywordState {
 public:
  LeafKeywordState(TokenType keyword) : KeywordState(keyword) {}

  KeywordState* next(int c) override { return nullptr; }
  KeywordState* nextCapital(int c) override { return nullptr; }

  virtual void Dump(intptr_t indent) override;
};

#endif
