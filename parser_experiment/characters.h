// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef CHARACTERS_H
#define CHARACTERS_H
constexpr int $EOF = -1;
constexpr int $STX = 2;
constexpr int $BS = 8;
constexpr int $TAB = 9;
constexpr int $LF = 10;
constexpr int $VTAB = 11;
constexpr int $FF = 12;
constexpr int $CR = 13;
constexpr int $SPACE = 32;
constexpr int $BANG = 33;
constexpr int $DQ = 34;
constexpr int $HASH = 35;
constexpr int $$ = 36;
constexpr int $PERCENT = 37;
constexpr int $AMPERSAND = 38;
constexpr int $SQ = 39;
constexpr int $OPEN_PAREN = 40;
constexpr int $CLOSE_PAREN = 41;
constexpr int $STAR = 42;
constexpr int $PLUS = 43;
constexpr int $COMMA = 44;
constexpr int $MINUS = 45;
constexpr int $PERIOD = 46;
constexpr int $SLASH = 47;
constexpr int $0 = 48;
constexpr int $1 = 49;
constexpr int $2 = 50;
constexpr int $3 = 51;
constexpr int $4 = 52;
constexpr int $5 = 53;
constexpr int $6 = 54;
constexpr int $7 = 55;
constexpr int $8 = 56;
constexpr int $9 = 57;
constexpr int $COLON = 58;
constexpr int $SEMICOLON = 59;
constexpr int $LT = 60;
constexpr int $EQ = 61;
constexpr int $GT = 62;
constexpr int $QUESTION = 63;
constexpr int $AT = 64;
constexpr int $A = 65;
constexpr int $B = 66;
constexpr int $C = 67;
constexpr int $D = 68;
constexpr int $E = 69;
constexpr int $F = 70;
constexpr int $G = 71;
constexpr int $H = 72;
constexpr int $I = 73;
constexpr int $J = 74;
constexpr int $K = 75;
constexpr int $L = 76;
constexpr int $M = 77;
constexpr int $N = 78;
constexpr int $O = 79;
constexpr int $P = 80;
constexpr int $Q = 81;
constexpr int $R = 82;
constexpr int $S = 83;
constexpr int $T = 84;
constexpr int $U = 85;
constexpr int $V = 86;
constexpr int $W = 87;
constexpr int $X = 88;
constexpr int $Y = 89;
constexpr int $Z = 90;
constexpr int $OPEN_SQUARE_BRACKET = 91;
constexpr int $BACKSLASH = 92;
constexpr int $CLOSE_SQUARE_BRACKET = 93;
constexpr int $CARET = 94;
constexpr int $_ = 95;
constexpr int $BACKPING = 96;
constexpr int $a = 97;
constexpr int $b = 98;
constexpr int $c = 99;
constexpr int $d = 100;
constexpr int $e = 101;
constexpr int $f = 102;
constexpr int $g = 103;
constexpr int $h = 104;
constexpr int $i = 105;
constexpr int $j = 106;
constexpr int $k = 107;
constexpr int $l = 108;
constexpr int $m = 109;
constexpr int $n = 110;
constexpr int $o = 111;
constexpr int $p = 112;
constexpr int $q = 113;
constexpr int $r = 114;
constexpr int $s = 115;
constexpr int $t = 116;
constexpr int $u = 117;
constexpr int $v = 118;
constexpr int $w = 119;
constexpr int $x = 120;
constexpr int $y = 121;
constexpr int $z = 122;
constexpr int $OPEN_CURLY_BRACKET = 123;
constexpr int $BAR = 124;
constexpr int $CLOSE_CURLY_BRACKET = 125;
constexpr int $TILDE = 126;
constexpr int $DEL = 127;
constexpr int $NBSP = 160;
constexpr int $LS = 0x2028;
constexpr int $PS = 0x2029;
constexpr int $FIRST_SURROGATE = 0xd800;
constexpr int $LAST_SURROGATE = 0xdfff;
constexpr int $LAST_CODE_POINT = 0x10ffff;

constexpr int unicodeReplacementCharacter = 0xFFFD;

[[clang::always_inline]] inline bool isDigit(int characterCode) {
  return $0 <= characterCode && characterCode <= $9;
}

[[clang::always_inline]] inline bool _isIdentifierChar(int next,
                                                       bool allowDollar) {
  return ($a <= next && next <= $z) || ($A <= next && next <= $Z) ||
         ($0 <= next && next <= $9) || next == $_ ||
         (next == $$ && allowDollar);
}

#endif