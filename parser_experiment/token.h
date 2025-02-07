// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef TOKEN_H
#define TOKEN_H

#include <format>
#include <sstream>

#include "characters.h"
#include "util.h"

#define KEYWORD_LIST(V)                                                        \
  V(ABSTRACT, "abstract")                                                      \
  V(AS, "as")                                                                  \
  V(ASSERT, "assert")                                                          \
  V(ASYNC, "async")                                                            \
  V(AUGMENT, "augment")                                                        \
  V(AWAIT, "await")                                                            \
  V(BASE, "base")                                                              \
  V(BREAK, "break")                                                            \
  V(CASE, "case")                                                              \
  V(CATCH, "catch")                                                            \
  V(CLASS, "class")                                                            \
  V(CONST, "const")                                                            \
  V(CONTINUE, "continue")                                                      \
  V(COVARIANT, "covariant")                                                    \
  V(DEFAULT, "default")                                                        \
  V(DEFERRED, "deferred")                                                      \
  V(DO, "do")                                                                  \
  V(DYNAMIC, "dynamic")                                                        \
  V(ELSE, "else")                                                              \
  V(ENUM, "enum")                                                              \
  V(EXPORT, "export")                                                          \
  V(EXTENDS, "extends")                                                        \
  V(EXTENSION, "extension")                                                    \
  V(EXTERNAL, "external")                                                      \
  V(FACTORY, "factory")                                                        \
  V(FALSE, "false")                                                            \
  V(FINAL, "final")                                                            \
  V(FINALLY, "finally")                                                        \
  V(FOR, "for")                                                                \
  V(FUNCTION, "Function")                                                      \
  V(GET, "get")                                                                \
  V(HIDE, "hide")                                                              \
  V(IF, "if")                                                                  \
  V(IMPLEMENTS, "implements")                                                  \
  V(IMPORT, "import")                                                          \
  V(IN, "in")                                                                  \
  V(INOUT, "inout")                                                            \
  V(INTERFACE, "interface")                                                    \
  V(IS, "is")                                                                  \
  V(LATE, "late")                                                              \
  V(LIBRARY, "library")                                                        \
  V(MIXIN, "mixin")                                                            \
  V(NATIVE, "native")                                                          \
  V(NEW, "new")                                                                \
  V(NULL, "null")                                                              \
  V(OF, "of")                                                                  \
  V(ON, "on")                                                                  \
  V(OPERATOR, "operator")                                                      \
  V(OUT, "out")                                                                \
  V(PART, "part")                                                              \
  V(PATCH, "patch")                                                            \
  V(REQUIRED, "required")                                                      \
  V(RETHROW, "rethrow")                                                        \
  V(RETURN, "return")                                                          \
  V(SEALED, "sealed")                                                          \
  V(SET, "set")                                                                \
  V(SHOW, "show")                                                              \
  V(SOURCE, "source")                                                          \
  V(STATIC, "static")                                                          \
  V(SUPER, "super")                                                            \
  V(SWITCH, "switch")                                                          \
  V(SYNC, "sync")                                                              \
  V(THIS, "this")                                                              \
  V(THROW, "throw")                                                            \
  V(TRUE, "true")                                                              \
  V(TRY, "try")                                                                \
  V(TYPEDEF, "typedef")                                                        \
  V(VAR, "var")                                                                \
  V(VOID, "void")                                                              \
  V(WHEN, "when")                                                              \
  V(WHILE, "while")                                                            \
  V(WITH, "with")                                                              \
  V(YIELD, "yield")

#define TOKEN_LIST(SIMPLE_TOK, PINNED_TOK, KEYWORD_TOK)                        \
  PINNED_TOK(EOF, 0)                                                           \
  PINNED_TOK(KEYWORD, $k)                                                      \
  PINNED_TOK(IDENTIFIER, $a)                                                   \
  PINNED_TOK(SCRIPT, $b)                                                       \
  PINNED_TOK(BAD_INPUT, $X)                                                    \
  PINNED_TOK(DOUBLE, $d)                                                       \
  PINNED_TOK(INT, $i)                                                          \
  PINNED_TOK(RECOVERY, $r)                                                     \
  PINNED_TOK(HEXADECIMAL, $x)                                                  \
  PINNED_TOK(STRING, $SQ)                                                      \
  PINNED_TOK(AMPERSAND, $AMPERSAND)                                            \
  PINNED_TOK(BACKPING, $BACKPING)                                              \
  PINNED_TOK(BACKSLASH, $BACKSLASH)                                            \
  PINNED_TOK(BANG, $BANG)                                                      \
  PINNED_TOK(BAR, $BAR)                                                        \
  PINNED_TOK(COLON, $COLON)                                                    \
  PINNED_TOK(COMMA, $COMMA)                                                    \
  PINNED_TOK(EQ, $EQ)                                                          \
  PINNED_TOK(GT, $GT)                                                          \
  PINNED_TOK(HASH, $HASH)                                                      \
  PINNED_TOK(OPEN_CURLY_BRACKET, $OPEN_CURLY_BRACKET)                          \
  PINNED_TOK(OPEN_SQUARE_BRACKET, $OPEN_SQUARE_BRACKET)                        \
  PINNED_TOK(OPEN_PAREN, $OPEN_PAREN)                                          \
  PINNED_TOK(LT, $LT)                                                          \
  PINNED_TOK(MINUS, $MINUS)                                                    \
  PINNED_TOK(PERIOD, $PERIOD)                                                  \
  PINNED_TOK(PLUS, $PLUS)                                                      \
  PINNED_TOK(QUESTION, $QUESTION)                                              \
  PINNED_TOK(AT, $AT)                                                          \
  PINNED_TOK(CLOSE_CURLY_BRACKET, $CLOSE_CURLY_BRACKET)                        \
  PINNED_TOK(CLOSE_SQUARE_BRACKET, $CLOSE_SQUARE_BRACKET)                      \
  PINNED_TOK(CLOSE_PAREN, $CLOSE_PAREN)                                        \
  PINNED_TOK(SEMICOLON, $SEMICOLON)                                            \
  PINNED_TOK(SLASH, $SLASH)                                                    \
  PINNED_TOK(TILDE, $TILDE)                                                    \
  PINNED_TOK(STAR, $STAR)                                                      \
  PINNED_TOK(PERCENT, $PERCENT)                                                \
  PINNED_TOK(CARET, $CARET)                                                    \
  PINNED_TOK(LT_EQ, 128)                                                       \
  SIMPLE_TOK(SLASH_EQ)                                                         \
  SIMPLE_TOK(PERIOD_PERIOD_PERIOD)                                             \
  SIMPLE_TOK(PERIOD_PERIOD)                                                    \
  SIMPLE_TOK(EQ_EQ_EQ)                                                         \
  SIMPLE_TOK(EQ_EQ)                                                            \
  SIMPLE_TOK(LT_LT_EQ)                                                         \
  SIMPLE_TOK(LT_LT)                                                            \
  SIMPLE_TOK(GT_EQ)                                                            \
  SIMPLE_TOK(GT_GT_EQ)                                                         \
  SIMPLE_TOK(INDEX_EQ)                                                         \
  SIMPLE_TOK(INDEX)                                                            \
  SIMPLE_TOK(BANG_EQ_EQ)                                                       \
  SIMPLE_TOK(BANG_EQ)                                                          \
  SIMPLE_TOK(AMPERSAND_AMPERSAND)                                              \
  SIMPLE_TOK(AMPERSAND_AMPERSAND_EQ)                                           \
  SIMPLE_TOK(AMPERSAND_EQ)                                                     \
  SIMPLE_TOK(BAR_BAR)                                                          \
  SIMPLE_TOK(BAR_BAR_EQ)                                                       \
  SIMPLE_TOK(BAR_EQ)                                                           \
  SIMPLE_TOK(STAR_EQ)                                                          \
  SIMPLE_TOK(PLUS_PLUS)                                                        \
  SIMPLE_TOK(PLUS_EQ)                                                          \
  SIMPLE_TOK(MINUS_MINUS)                                                      \
  SIMPLE_TOK(MINUS_EQ)                                                         \
  SIMPLE_TOK(TILDE_SLASH_EQ)                                                   \
  SIMPLE_TOK(TILDE_SLASH)                                                      \
  SIMPLE_TOK(PERCENT_EQ)                                                       \
  SIMPLE_TOK(GT_GT)                                                            \
  SIMPLE_TOK(CARET_EQ)                                                         \
  SIMPLE_TOK(COMMENT)                                                          \
  SIMPLE_TOK(STRING_INTERPOLATION_IDENTIFIER)                                  \
  SIMPLE_TOK(QUESTION_PERIOD)                                                  \
  SIMPLE_TOK(QUESTION_QUESTION)                                                \
  SIMPLE_TOK(QUESTION_QUESTION_EQ)                                             \
  SIMPLE_TOK(GENERIC_METHOD_TYPE_ASSIGN)                                       \
  SIMPLE_TOK(GENERIC_METHOD_TYPE_LIST)                                         \
  SIMPLE_TOK(GT_GT_GT)                                                         \
  SIMPLE_TOK(PERIOD_PERIOD_PERIOD_QUESTION)                                    \
  SIMPLE_TOK(GT_GT_GT_EQ)                                                      \
  SIMPLE_TOK(QUESTION_PERIOD_PERIOD)                                           \
  SIMPLE_TOK(STRING_INTERPOLATION_EXPRESSION)                                  \
  SIMPLE_TOK(INT_WITH_SEPARATORS)                                              \
  SIMPLE_TOK(HEXADECIMAL_WITH_SEPARATORS)                                      \
  SIMPLE_TOK(DOUBLE_WITH_SEPARATORS)                                           \
  SIMPLE_TOK(SINGLE_LINE_COMMENT)                                              \
  SIMPLE_TOK(MULTI_LINE_COMMENT)                                               \
  KEYWORD_LIST(KEYWORD_TOK)                                                    \
  SIMPLE_TOK(SCRIPT_TAG)                                                       \
  SIMPLE_TOK(LANGUAGE_VERSION)     \
  SIMPLE_TOK(UNMATCHED_TOKEN)                                                  \
  SIMPLE_TOK(UNSUPPORTED_OPERATOR)                                             \
  SIMPLE_TOK(UNTERMINATED_TOKEN)                                               \
  SIMPLE_TOK(ASCII_CONTROL_CHARACTER_TOKEN)                                    \
  SIMPLE_TOK(ENCODING_ERROR)                                                   \
  SIMPLE_TOK(NON_ASCII_WHITESPACE_TOKEN)                                       \
  SIMPLE_TOK(NON_ASCII_IDENTIFIER_TOKEN)

enum class TokenType : uint16_t {
#define SIMPLE_TOK(Name) k##Name,
#define PINNED_TOK(Name, Val) k##Name = Val,
#define KEYWORD_TOK(Name, lexeme) k##Name,
  TOKEN_LIST(SIMPLE_TOK, PINNED_TOK, KEYWORD_TOK)
#undef SIMPLE_TOK
#undef PINNED_TOK
#undef KEYWORD_TOK
};

class CommentToken;

class Token {
 public:
  static Token* NewToken(TokenType type,
                         int offset,
                         CommentToken* comments = nullptr) {
    return new Token(type, offset, comments);
  }

  static Token* NewEof(int offset, CommentToken* comments = nullptr) {
    return new Token(TokenType::kEOF, offset, comments);
  }

  TokenType kind() const { return type_; }

  Token* next() const { return next_; }
  void set_next(Token* next) { next_ = next; }

  Token* previous() const { return previous_; }
  void set_previous(Token* previous) { previous_ = previous; }

  int charOffset() const { return offset_; }
  int charEnd() const { UNIMPLEMENTED(); }

  CommentToken* precedingComments() const { return comments_; }

  bool isSynthetic() const { return flags_ & 1; }

  bool isErrorToken() const { return TokenType::kUNMATCHED_TOKEN <= kind(); }

  bool isNonAsciiIdentifierToken() const {
    return kind() == TokenType::kNON_ASCII_IDENTIFIER_TOKEN;
  }

 protected:
  Token(TokenType type, int offset, CommentToken* comments, int8_t flags = 0)
      : comments_(comments), offset_(offset), type_(type), flags_(flags) {}

  Token* next_ = nullptr;
  Token* previous_ = nullptr;
  CommentToken* comments_ = nullptr;
  int32_t offset_;
  TokenType type_;
  uint8_t flags_ = 0;
};

inline std::string_view TokenTypeName(TokenType kind) {
  switch (kind) {
#define SIMPLE_TOK(Name)                                                       \
  case TokenType::k##Name:                                                     \
    return #Name;
#define PINNED_TOK(Name, Val)                                                  \
  case TokenType::k##Name:                                                     \
    return #Name;
#define KEYWORD_TOK(Name, lexeme)                                              \
  case TokenType::k##Name:                                                     \
    return #Name;
    TOKEN_LIST(SIMPLE_TOK, PINNED_TOK, KEYWORD_TOK)
#undef SIMPLE_TOK
#undef PINNED_TOK
#undef KEYWORD_TOK

    default:
      return "<?>";
  }
}

template <>
struct std::formatter<TokenType, char> {
  template <class ParseContext>
  constexpr ParseContext::iterator parse(ParseContext& ctx) {
    auto it = ctx.begin();
    if (it == ctx.end() || *it == '}') return it;

    throw std::format_error("Invalid format args for Token.");
  }

  template <class FmtContext>
  FmtContext::iterator format(TokenType type, FmtContext& ctx) const {
    return std::ranges::copy(TokenTypeName(type), ctx.out()).out;
  }
};

class SimpleToken : public Token {
 protected:
  SimpleToken(TokenType type,
              int offset,
              CommentToken* comments,
              int8_t flags = 0)
      : Token(type, offset, comments, flags) {}
};

class StringToken : public SimpleToken {
 public:
  StringToken(TokenType type,
              int offset,
              CommentToken* comments,
              const std::string_view& content,
              bool isAscii,
              bool allowLazy)
      : SimpleToken(type, offset, comments), content_(content) {
    if (!allowLazy) {
      UNIMPLEMENTED();
    }
    if (isAscii) {
      flags_ |= 0x2;
    }
  }

  const std::string_view& content() const { return content_; }

 private:
  const std::string_view content_;
};

class CommentToken : public StringToken {
 public:
  CommentToken(TokenType type,
            int offset,
            CommentToken* comments,
            const std::string_view& content,
            bool isAscii,
            bool allowLazy) : StringToken(type, offset, comments, content, isAscii, allowLazy) {}
};

class DartDocToken : public CommentToken {};

class LanguageVersionToken : public CommentToken {
 public:
  LanguageVersionToken(const std::string_view& content, int offset, int major, int minor) : CommentToken(TokenType::kLANGUAGE_VERSION, offset, nullptr, content, true, true), major_(major), minor_(minor) {}

  int8_t major() const { return major_; }
  int8_t minor() const { return minor_; }


 private:
  int8_t major_;
  int8_t minor_;
};

class KeywordToken : public SimpleToken {
 public:
  KeywordToken(TokenType keyword, int offset, CommentToken* comments)
      : SimpleToken(keyword, offset, comments) {}
};

class BeginToken : public SimpleToken {
 public:
  BeginToken(TokenType type, int offset, CommentToken* comments)
      : SimpleToken(type, offset, comments) {}

  Token* endGroup = nullptr;
};

class ErrorToken : public SimpleToken {
 public:
  ErrorToken(TokenType type, int offset, CommentToken* comments)
      : SimpleToken(type, offset, comments) {}
};

class SyntheticToken : public SimpleToken {
 public:
  SyntheticToken(TokenType type, int offset, Token* before)
      : SimpleToken(type, offset, nullptr, 1), before_(before) {}

  Token* before() const { return before_; }

 private:
  Token* before_;
};

class UnmatchedToken : public ErrorToken {
 public:
  UnmatchedToken(BeginToken* token)
      : ErrorToken(TokenType::kUNMATCHED_TOKEN, -1, nullptr) {}
};

class UnsupportedOperator : public ErrorToken {
 public:
  UnsupportedOperator(Token* token, int offset)
      : ErrorToken(TokenType::kUNSUPPORTED_OPERATOR, offset, nullptr) {}
};

enum class Message {
  messageUnexpectedSeparatorInNumber,
  messageExpectedHexDigit,
  messageMissingExponent,
  messageUnterminatedComment,
  messageUnexpectedDollarInString,
};

class UnterminatedToken : public ErrorToken {
 public:
  UnterminatedToken(Message assertionMessage, int charOffset, int endOffset)
      : ErrorToken(TokenType::kUNTERMINATED_TOKEN, charOffset, nullptr) {}
};

class AsciiControlCharacterToken : public ErrorToken {
 public:
  AsciiControlCharacterToken(int character, int charOffset)
      : ErrorToken(TokenType::kASCII_CONTROL_CHARACTER_TOKEN,
                   charOffset,
                   nullptr),
        character_(character) {}

  int character() const { return character_; }

 private:
  int character_;
};

class EncodingErrorToken : public ErrorToken {
 public:
  EncodingErrorToken(int charOffset)
      : ErrorToken(TokenType::kENCODING_ERROR, charOffset, nullptr) {}
};

class NonAsciiWhitespaceToken : public ErrorToken {
 public:
  NonAsciiWhitespaceToken(int character, int charOffset)
      : ErrorToken(TokenType::kNON_ASCII_WHITESPACE_TOKEN, charOffset, nullptr),
        character_(character) {}

  int character() const { return character_; }

 private:
  int character_;
};

class NonAsciiIdentifierToken : public ErrorToken {
 public:
  NonAsciiIdentifierToken(int character, int charOffset)
      : ErrorToken(TokenType::kNON_ASCII_IDENTIFIER_TOKEN, charOffset, nullptr),
        character_(character) {}

  int character() const { return character_; }

 private:
  int character_;
};

inline ErrorToken* buildUnexpectedCharacterToken(int character,
                                                 int charOffset) {
  if (character < 0x1f) {
    return new AsciiControlCharacterToken(character, charOffset);
  }
  switch (character) {
    case unicodeReplacementCharacter:
      return new EncodingErrorToken(charOffset);

    /// See [General Punctuation]
    /// (http://www.unicode.org/charts/PDF/U2000.pdf).
    case 0x00A0:  // No-break space.
    case 0x1680:  // Ogham space mark.
    case 0x180E:  // Mongolian vowel separator.
    case 0x2000:  // En quad.
    case 0x2001:  // Em quad.
    case 0x2002:  // En space.
    case 0x2003:  // Em space.
    case 0x2004:  // Three-per-em space.
    case 0x2005:  // Four-per-em space.
    case 0x2006:  // Six-per-em space.
    case 0x2007:  // Figure space.
    case 0x2008:  // Punctuation space.
    case 0x2009:  // Thin space.
    case 0x200A:  // Hair space.
    case 0x200B:  // Zero width space.
    case 0x2028:  // Line separator.
    case 0x2029:  // Paragraph separator.
    case 0x202F:  // Narrow no-break space.
    case 0x205F:  // Medium mathematical space.
    case 0x3000:  // Ideographic space.
    case 0xFEFF:  // Zero width no-break space.
      return new NonAsciiWhitespaceToken(character, charOffset);

    default:
      return new NonAsciiIdentifierToken(character, charOffset);
  }
}

template <>
struct std::formatter<Token, char> {
  template <class ParseContext>
  constexpr ParseContext::iterator parse(ParseContext& ctx) {
    auto it = ctx.begin();
    if (it == ctx.end() || *it == '}') return it;

    throw std::format_error("Invalid format args for Token.");
  }

  template <class FmtContext>
  FmtContext::iterator format(const Token& tok, FmtContext& ctx) const {
    std::ostringstream out;
    out << "Token {" << "offset: " << tok.charOffset()
        << ", type: " << TokenTypeName(tok.kind());
    if (tok.kind() == TokenType::kIDENTIFIER) {
      out << ", content: " << static_cast<const StringToken&>(tok).content();
    }
    out << "}";

    return std::ranges::copy(std::move(out).str(), ctx.out()).out;
  }
};

#endif