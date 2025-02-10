// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "keyword_state.h"
#include "token.h"

template <typename T>
class AbstractScanner;

typedef void (*LanguageVersionChanged)(void* scanner, TokenRef languageVersion);

/// [ScannerConfiguration] contains information for configuring which tokens
/// the scanner produces based upon the Dart language level.
struct ScannerConfiguration {
  static constexpr ScannerConfiguration nonNullable() {
    return ScannerConfiguration();
  }

  /// Experimental flag for enabling scanning of `>>>`.
  /// See https://github.com/dart-lang/language/issues/61
  /// and https://github.com/dart-lang/language/issues/60
  const bool enableTripleShift;

  /// If `true`, 'augment' is treated as a built-in identifier.
  const bool forAugmentationLibrary;

  constexpr ScannerConfiguration(bool enableTripleShift = false,
                                 bool forAugmentationLibrary = false)
      : enableTripleShift(enableTripleShift),
        forAugmentationLibrary(forAugmentationLibrary) {}
};

constexpr std::array<TokenType, 256> ClosingBraceTable() {
  std::array<TokenType, 256> table{};
  table[(int)TokenType::kOPEN_PAREN] = TokenType::kCLOSE_PAREN;
  table[(int)TokenType::kOPEN_SQUARE_BRACKET] =
      TokenType::kCLOSE_SQUARE_BRACKET;
  table[(int)TokenType::kOPEN_CURLY_BRACKET] = TokenType::kCLOSE_CURLY_BRACKET;
  table[(int)TokenType::kLT] = TokenType::kGT;
  table[(int)TokenType::kSTRING_INTERPOLATION_EXPRESSION] =
      TokenType::kCLOSE_CURLY_BRACKET;
  return table;
}

template <typename ConcreteScanner>
class AbstractScanner {
 public:
  /**
   * A flag indicating whether character sequences `&&=` and `||=`
   * should be tokenized as the assignment operators
   * [AMPERSAND_AMPERSAND_EQ] and [BAR_BAR_EQ] respectively.
   * See issue https://github.com/dart-lang/sdk/issues/30340
   */
  static constexpr bool LAZY_ASSIGNMENT_ENABLED = false;

  const bool includeComments;

  /// Called when the scanner detects a language version comment
  /// so that the listener can update the scanner configuration
  /// based upon the specified language version.
  const LanguageVersionChanged languageVersionChanged = nullptr;

  /// Experimental flag for enabling scanning of `>>>`.
  /// See https://github.com/dart-lang/language/issues/61
  /// and https://github.com/dart-lang/language/issues/60
  bool _enableTripleShift = false;

  /// If `true`, 'augment' is treated as a built-in identifier.
  bool _forAugmentationLibrary = false;

  /**
   * The string offset for the next token that will be created.
   *
   * Note that in the [Utf8BytesScanner], [string_offset()] and [scan_offset()] values
   * are different. One string character can be encoded using multiple UTF-8
   * bytes.
   */
  int tokenStart = -1;

  bool hasErrors = false;

  /**
   * A pointer to the stream of comment tokens created by this scanner
   * before they are assigned to the [Token] precedingComments field
   * of a non-comment token. A value of `null` indicates no comment tokens.
   */
  CommentToken* comments;

  /**
   * A pointer to the last scanned comment token or `null` if none.
   */
  // Token* commentsTail;

  // TODO: LineStarts is optimized for size in Dart version.
  std::vector<int> lineStarts;

  /**
   * The stack of open groups, e.g [: { ... ( .. :]
   * Each BeginToken has a pointer to the token where the group
   * ends. This field is set when scanning the end group token.
   */
  // TODO: Link<BeginToken> groupingStack = const Link<BeginToken>();
  // Link<TokenRef>* groupingStack = &emptyStack;
  std::vector<TokenRef> groupingStack{10};
  uint32_t groupingStackLen = 0;

  const bool inRecoveryOption;
  int recoveryCount = 0;
  const bool allowLazyStrings;

  TokenWriter tokens_;

  [[clang::always_inline]] TokenType closeBraceInfoFor(TokenRef begin) {
    constexpr auto table = ClosingBraceTable();
    return table[static_cast<int>(begin.type())];
  }

  AbstractScanner(ScannerConfiguration* config,
                  bool includeComments,
                  LanguageVersionChanged languageVersionChanged,
                  int numberOfBytesHint,
                  bool allowLazyStrings = true)
      : includeComments(includeComments),
        languageVersionChanged(languageVersionChanged),
        lineStarts(numberOfBytesHint),
        inRecoveryOption(false),
        allowLazyStrings(allowLazyStrings),
        tokens_(numberOfBytesHint) {
    // tail = tokens;
    // errorTail = tokens;
    set_configuration(config);
  }

  AbstractScanner createRecoveryOptionScanner() {
    return concrete_scanner()->createRecoveryOptionScanner();
  }

  /*
  AbstractScanner.recoveryOptionScanner(AbstractScanner copyFrom)
      : lineStarts = [],
        includeComments = false,
        languageVersionChanged = null,
        inRecoveryOption = true,
        allowLazyStrings = true {
    this.tail = this.tokens;
    this.errorTail = this.tokens;
    this._enableTripleShift = copyFrom._enableTripleShift;
    this.tokenStart = copyFrom.tokenStart;
    this.groupingStack = copyFrom.groupingStack;
  }*/

  void set_configuration(ScannerConfiguration* config) {
    if (config != nullptr) {
      _enableTripleShift = config->enableTripleShift;
      _forAugmentationLibrary = config->forAugmentationLibrary;
    }
  }

  /**
   * Advances and returns the next character.
   *
   * If the next character is non-ASCII, then the returned value depends on the
   * scanner implementation. The [Utf8BytesScanner] returns a UTF-8 byte, while
   * the [StringScanner] returns a UTF-16 code unit.
   *
   * The scanner ensures that [advance] is not invoked after it returned [$EOF].
   * This allows implementations to omit bound checks if the data structure ends
   * with '0'.
   */
  [[clang::always_inline]] int advance() {
    return concrete_scanner()->advance();
  }

  /**
   * Returns the current unicode character.
   *
   * If the current character is ASCII, then it is returned unchanged.
   *
   * The [Utf8BytesScanner] decodes the next unicode code point starting at the
   * current position. Note that every unicode character is returned as a single
   * code point, that is, for '\u{1d11e}' it returns 119070, and the following
   * [advance] returns the next character.
   *
   * The [StringScanner] returns the current character unchanged, which might
   * be a surrogate character. In the case of '\u{1d11e}', it returns the first
   * code unit 55348, and the following [advance] returns the second code unit
   * 56606.
   *
   * Invoking [currentAsUnicode] multiple times is safe, i.e.,
   * [:currentAsUnicode(next) == currentAsUnicode(currentAsUnicode(next)):].
   */
  [[clang::always_inline]] int currentAsUnicode(int next) {
    return concrete_scanner()->currentAsUnicode(next);
  }

  /**
   * Returns the character at the next position. Like in [advance], the
   * [Utf8BytesScanner] returns a UTF-8 byte, while the [StringScanner] returns
   * a UTF-16 code unit.
   */
  [[clang::always_inline]] int peek() { return concrete_scanner()->peek(); }

  /**
   * Notifies the scanner that unicode characters were detected in either a
   * comment or a string literal between [startScanOffset] and the current
   * scan offset.
   */
  [[clang::always_inline]] void handleUnicode(int startScanOffset) {
    concrete_scanner()->handleUnicode(startScanOffset);
  }

  ConcreteScanner* concrete_scanner() {
    return static_cast<ConcreteScanner*>(this);
  }
  const ConcreteScanner* concrete_scanner() const {
    return static_cast<const ConcreteScanner*>(this);
  }

  /**
   * Returns the current scan offset.
   *
   * In the [Utf8BytesScanner] this is the offset into the byte list, in the
   * [StringScanner] the offset in the source string.
   */
  // int scan_offset() const { UNIMPLEMENTED(); };
  [[clang::always_inline]] int scan_offset() const {
    return concrete_scanner()->scan_offset();
  }

  /**
   * Returns the current string offset.
   *
   * In the [StringScanner] this is identical to the [scan_offset()]. In the
   * [Utf8BytesScanner] it is computed based on encountered UTF-8 characters.
   */
  [[clang::always_inline]] int string_offset() const {
    return concrete_scanner()->string_offset();
  }

  /**
   * Returns the first token scanned by this [Scanner].
   */
  // Token* firstToken() { return tokens->next(); }

  /**
   * Notifies that a new token starts at current offset.
   */
  [[clang::always_inline]] void beginToken() { tokenStart = string_offset(); }

  /**
   * Appends a substring from the scan offset [:start:] to the current
   * [:scan_offset():] plus the [:extraOffset:]. For example, if the current
   * scan_offset() is 10, then [:appendSubstringToken(5, -1):] will append the
   * substring string [5,9).
   *
   * Note that [extraOffset] can only be used if the covered character(s) are
   * known to be ASCII.
   */
  void appendSubstringToken(TokenType type,
                            int start,
                            bool asciiOnly,
                            int extraOffset = 0) {
    appendToken(createSubstringToken(type, start, asciiOnly, extraOffset,
                                     allowLazyStrings));
  }

  /**
   * Returns a new substring from the scan offset [start] to the current
   * [scan_offset()] plus the [extraOffset]. For example, if the current
   * scan_offset() is 10, then [appendSubstringToken(5, -1)] will append the
   * substring string [5,9).
   *
   * Note that [extraOffset] can only be used if the covered character(s) are
   * known to be ASCII.
   */
  [[clang::always_inline]] TokenRef createSubstringToken(TokenType type,
                                                         int start,
                                                         bool asciiOnly,
                                                         int extraOffset,
                                                         bool allowLazy) {
    return concrete_scanner()->createSubstringToken(type, start, asciiOnly,
                                                    extraOffset, allowLazy);
  }

  /**
   * Appends a substring from the scan offset [start] to the current
   * [scan_offset()] plus [syntheticChars]. The additional char(s) will be added
   * to the unterminated string literal's lexeme but the returned
   * token's length will *not* include those additional char(s)
   * so as to be true to the original source.
   */
  void appendSyntheticSubstringToken(TokenType type,
                                     int start,
                                     bool asciiOnly,
                                     const std::string_view& syntheticChars) {
    appendToken(
        createSyntheticSubstringToken(type, start, asciiOnly, syntheticChars));
  }

  /**
   * Returns a new synthetic substring from the scan offset [start]
   * to the current [scan_offset()] plus the [syntheticChars].
   * The [syntheticChars] are appended to the unterminated string
   * literal's lexeme but the returned token's length will *not* include
   * those additional characters so as to be true to the original source.
   */
  [[clang::always_inline]] TokenRef createSyntheticSubstringToken(
      TokenType type,
      int start,
      bool asciiOnly,
      const std::string_view& syntheticChars) {
    return concrete_scanner()->createSyntheticSubstringToken(
        type, start, asciiOnly, syntheticChars);
  }

  /**
   * Appends a fixed token whose kind and content is determined by [type].
   * Appends an *operator* token from [type].
   *
   * An operator token represent operators like ':', '.', ';', '&&', '==', '--',
   * '=>', etc.
   */
  TokenRef appendPrecedenceToken(TokenType type) {
    return tokens_.Add(type, tokenStart, comments);
  }

  /**
   * Appends a fixed token based on whether the current char is [choice] or not.
   * If the current char is [choice] a fixed token whose kind and content
   * is determined by [yes] is appended, otherwise a fixed token whose kind
   * and content is determined by [no] is appended.
   */
  int select(int choice, TokenType yes, TokenType no) {
    int next = advance();
    if (next == choice) {
      appendPrecedenceToken(yes);
      return advance();
    } else {
      appendPrecedenceToken(no);
      return next;
    }
  }

  /**
   * Appends a keyword token whose kind is determined by [keyword].
   */
  void appendKeywordToken(TokenType keyword) {
    // String syntax = keyword.lexeme;
    // Type parameters and arguments cannot contain 'this'.
    if (keyword == TokenType::kTHIS) {
      discardOpenLt();
    }
    tokens_.Add(keyword, tokenStart, comments);
  }

  void appendEofToken() {
    beginToken();
    discardOpenLt();
    while (groupingStackLen > 0) {
      unmatchedBeginGroup(groupingStack[groupingStackLen - 1]);
      groupingStackLen--;
    }
    tokens_.Add(TokenType::kEOF, tokenStart, comments);
  }

  /**
   * Notifies scanning a whitespace character. Note that [appendWhiteSpace] is
   * not always invoked for [$SPACE] characters.
   *
   * This method is used by the scanners to track line breaks and create the
   * [lineStarts] map.
   */
  void appendWhiteSpace(int next) {
    if (next == $LF) {
      lineStarts.push_back(string_offset() +
                           1);  // +1, the line starts after the $LF.
    }
  }

  /**
   * Notifies on [$LF] characters in multi-line comments or strings.
   *
   * This method is used by the scanners to track line breaks and create the
   * [lineStarts] map.
   */
  void lineFeedInMultiline() { lineStarts.push_back(string_offset() + 1); }

  /**
   * Appends a token that begins a new group, represented by [type].
   * Group begin tokens are '{', '(', '[', '<' and '${'.
   */
  void appendBeginGroup(TokenType type) {
    auto token = tokens_.Add(type, tokenStart, comments);

    // { [ ${ cannot appear inside a type parameters / arguments.
    if (type != TokenType::kLT && type != TokenType::kOPEN_PAREN) {
      discardOpenLt();
    }

    if (groupingStackLen == groupingStack.size()) {
      groupingStack.resize(groupingStackLen * 2);
    }
    groupingStack[groupingStackLen++] = token;
  }

  /**
   * Appends a token that begins an end group, represented by [type].
   * It handles the group end tokens '}', ')' and ']'. The tokens '>',
   * '>>' and '>>>' are handled separately by [appendGt], [appendGtGt]
   * and [appendGtGtGt].
   */
  int appendEndGroup(TokenType type, TokenType openKind) {
    assert(openKind != TokenType::kLT);  // openKind is < for > and >>
    bool foundMatchingBrace = discardBeginGroupUntil(openKind);
    return appendEndGroupInternal(foundMatchingBrace, type, openKind);
  }

  /// Append the end group (parenthesis, bracket etc).
  /// If [foundMatchingBrace] is true the grouping stack (stack of parenthesis
  /// etc) is updated, otherwise it's left alone.
  /// In effect, if [foundMatchingBrace] is false this end token is basically
  /// ignored, i.e. not really seen as an end group.
  int appendEndGroupInternal(bool foundMatchingBrace,
                             TokenType type,
                             TokenType openKind) {
    if (!foundMatchingBrace) {
      // No begin group. Leave the grouping stack alone and just continue.
      appendPrecedenceToken(type);
      return advance();
    }
    TokenRef close = appendPrecedenceToken(type);
    TokenRef begin = groupingStack[groupingStackLen - 1];
    tokens_.set_end_group_of(begin, close);
    groupingStackLen--;
    if (begin.type() != openKind) {
      assert(begin.type() == TokenType::kSTRING_INTERPOLATION_EXPRESSION &&
             openKind == TokenType::kOPEN_CURLY_BRACKET);
      // We're ending an interpolated expression.
      // Using "start-of-text" to signal that we're back in string
      // scanning mode.
      return $STX;
    }
    return advance();
  }

  /**
   * Appends a token for '>'.
   * This method does not issue unmatched errors, because > is also the
   * greater-than operator. It does not necessarily have to close a group.
   */
  void appendGt(TokenType type) {
    auto close = appendPrecedenceToken(type);
    if (groupingStackLen == 0) return;
    auto front = groupingStack[groupingStackLen - 1];
    if (front.type() == TokenType::kLT) {
      tokens_.set_end_group_of(front, close);
      groupingStackLen--;
    }
  }

  /**
   * Appends a token for '>>'.
   * This method does not issue unmatched errors, because >> is also the
   * shift operator. It does not necessarily have to close a group.
   */
  void appendGtGt(TokenType type) {
    auto close = appendPrecedenceToken(type);
    if (groupingStackLen == 0) return;
    if (groupingStack[groupingStackLen - 1].type() == TokenType::kLT) {
      // Don't assign endGroup: in "T<U<V>>", the '>>' token closes the outer
      // '<', the inner '<' is left without endGroup.
      groupingStackLen--;
    }
    if (groupingStackLen == 0) return;
    if (groupingStack[groupingStackLen - 1].type() == TokenType::kLT) {
      tokens_.set_end_group_of(groupingStack[groupingStackLen - 1], close);
      groupingStackLen--;
    }
  }

  /// Appends a token for '>>>'.
  ///
  /// This method does not issue unmatched errors, because >>> is also the
  /// triple shift operator. It does not necessarily have to close a group.
  void appendGtGtGt(TokenType type) {
    auto close = appendPrecedenceToken(type);
    if (groupingStackLen == 0) return;

    // Don't assign endGroup: in "T<U<V<X>>>", the '>>>' token closes the
    // outer '<', all the inner '<' are left without endGroups.
    if (groupingStack[groupingStackLen - 1].type() == TokenType::kLT) {
      groupingStackLen--;
    }
    if (groupingStackLen == 0) return;
    if (groupingStack[groupingStackLen - 1].type() == TokenType::kLT) {
      groupingStackLen--;
    }
    if (groupingStackLen == 0) return;
    if (groupingStack[groupingStackLen - 1].type() == TokenType::kLT) {
      tokens_.set_end_group_of(groupingStack[groupingStackLen - 1], close);
      groupingStackLen--;
    }
  }

  /// Prepend [token] to the token stream.
  void prependErrorToken(void* token) {
    UNIMPLEMENTED();
#if 0
    hasErrors = true;
    if (errorTail == tail) {
      appendToken(token);
      errorTail = tail;
    } else {
      token->set_next(errorTail->next());
      token->next()->set_previous(token);
      errorTail->set_next(token);
      token->set_previous(errorTail);
      errorTail = errorTail->next();
    }
#endif
  }

  /**
   * Returns a new comment from the scan offset [start] to the current
   * [scan_offset()] plus the [extraOffset]. For example, if the current
   * scan_offset() is 10, then [appendSubstringToken(5, -1)] will append the
   * substring string [5,9).
   *
   * Note that [extraOffset] can only be used if the covered character(s) are
   * known to be ASCII.
   */
  [[clang::always_inline]] CommentToken* createCommentToken(
      TokenType type,
      int start,
      bool asciiOnly,
      int extraOffset = 0) {
    return concrete_scanner()->createCommentToken(type, start, asciiOnly,
                                                  extraOffset);
  }

  /**
   * Returns a new dartdoc from the scan offset [start] to the current
   * [scan_offset()] plus the [extraOffset]. For example, if the current
   * scan_offset() is 10, then [appendSubstringToken(5, -1)] will append the
   * substring string [5,9).
   *
   * Note that [extraOffset] can only be used if the covered character(s) are
   * known to be ASCII.
   */
  [[clang::always_inline]] DartDocToken* createDartDocToken(
      TokenType type,
      int start,
      bool asciiOnly,
      int extraOffset = 0) {
    return concrete_scanner()->createDartDocToken(type, start, asciiOnly,
                                                  extraOffset);
  }

  /**
   * Returns a new language version token from the scan offset [start]
   * to the current [scan_offset()] similar to createCommentToken.
   */
  [[clang::always_inline]] TokenRef createLanguageVersionToken(int start,
                                                               int major,
                                                               int minor) {
    return concrete_scanner()->createLanguageVersionToken(start, major, minor);
  }

  /**
   * If a begin group token matches [openKind],
   * then discard begin group tokens up to that match and return `true`,
   * otherwise return `false`.
   * This recovers nicely from situations like "{[}" and "{foo());}",
   * but not "foo(() {bar());});"
   */
  bool discardBeginGroupUntil(TokenType openKind) {
    // auto originalStackLen = groupingStackLen;

    bool first = true;
    do {
      // Don't report unmatched errors for <; it is also the less-than operator.
      discardOpenLt();
      if (groupingStackLen == 0) break;  // recover
      TokenRef begin = groupingStack[groupingStackLen - 1];
      if (openKind == begin.type() ||
          (openKind == TokenType::kOPEN_CURLY_BRACKET &&
           begin.type() == TokenType::kSTRING_INTERPOLATION_EXPRESSION)) {
        if (first) {
          // If the expected opener has been found on the first pass
          // then no recovery necessary.
          return true;
        }
        break;  // recover
      }
      first = false;
      groupingStackLen--;
    } while (!(groupingStackLen == 0));

    UNIMPLEMENTED();
#if 0
    recoveryCount++;

    // If the stack does not have any opener of the given type,
    // then return without discarding anything.
    // This recovers nicely from situations like "{foo());}".
    if (groupingStackLen == 0) {
      groupingStackLen = originalStackLen;
      return false;
    }

    // We found a matching group somewhere in the stack, but generally don't
    // know if we should recover by inserting synthetic closers or
    // basically ignore the current token.
    // We're in a recovery setting so we're allowed to be 'relatively slow' ---
    // try both and see which is better (i.e. gives fewest rewrites later).
    // To not get exponential runtime we will not do this nested though.
    // E.g. we can recover "{[}" as "{[]}" (better) or (with . for ignored
    // tokens) "{[.".
    // Or we can recover "[(])]" as "[()].." or "[(.)]" (better).
    if (!inRecoveryOption) {
      TokenType type;
      switch (openKind) {
        case TokenType::kOPEN_SQUARE_BRACKET:
        case TokenType::kOPEN_CURLY_BRACKET:
        case TokenType::kOPEN_PAREN:
          type = openKind;
          break;
        default:
          UNIMPLEMENTED();
          //          throw new StateError("Unexpected openKind");
      }

      // Option #1: Insert synthetic closers.
      int option1Recoveries;
      {
        AbstractScanner option1 = createRecoveryOptionScanner();
        option1.insertSyntheticClosers(originalStackLen, groupingStackLen);
        option1Recoveries =
            option1.recoveryOptionTokenizer(option1.appendEndGroupInternal(
                /* foundMatchingBrace = */ true, type, openKind));
        option1Recoveries += option1.groupingStackLen;
      }

      // Option #2: ignore this token.
      int option2Recoveries;
      {
        AbstractScanner option2 = createRecoveryOptionScanner();
        option2.groupingStack = groupingStack;
        option2.groupingStackLen = originalStackLen;
        option2Recoveries =
            option2.recoveryOptionTokenizer(option2.appendEndGroupInternal(
                /* foundMatchingBrace = */ false, type, openKind));
        // We add 1 to make this option pay for ignoring this token.
        option2Recoveries += option2.groupingStackLen + 1;
      }

      // The option-runs might have set invalid endGroup pointers. Reset them.
      for (uint32_t i = 0; i < originalStackLen; i++) {
        groupingStack[i]->endGroup = nullptr;
      }

      if (option2Recoveries < option1Recoveries) {
        // Perform option #2 recovery.
        groupingStackLen = originalStackLen;
        return false;
      }
      // option #1 is the default, so fall though.
    }

    // Insert synthetic closers and report errors for any unbalanced openers.
    // This recovers nicely from situations like "{[}".
    insertSyntheticClosers(originalStackLen, groupingStackLen);
    return true;
#endif
  }

  void insertSyntheticClosers(int32_t originalStackLen, int32_t currentLen) {
    UNIMPLEMENTED();
#if 0
    // Insert synthetic closers and report errors for any unbalanced openers.
    // This recovers nicely from situations like "{[}".
    while (originalStack != entryToUse) {
      // Don't report unmatched errors for <; it is also the less-than operator.
      if (entryToUse->front().type() != TokenType::kLT) {
        unmatchedBeginGroup(originalStack->front());
      }
      originalStack = originalStack->next();
    }
#endif
  }

  /**
   * This method is called to discard '<' from the "grouping" stack.
   *
   * [ClassMemberParser.skipExpression] relies on the fact that we do not
   * create groups for stuff like:
   * [:a = b < c, d = e > f:].
   *
   * In other words, this method is called when the scanner recognizes
   * something which cannot possibly be part of a type parameter/argument
   * list, like the '=' in the above example.
   */
  void discardOpenLt() {
    while (!(groupingStackLen == 0) &&
           groupingStack[groupingStackLen - 1].type() == TokenType::kLT) {
      groupingStackLen--;
    }
  }

  /**
   * This method is called to discard '${' from the "grouping" stack.
   *
   * This method is called when the scanner finds an unterminated
   * interpolation expression.
   */
  void discardInterpolation() {
    while (!(groupingStackLen == 0)) {
      TokenRef beginToken = groupingStack[groupingStackLen - 1];
      unmatchedBeginGroup(beginToken);
      groupingStackLen--;
      if (beginToken.type() == TokenType::kSTRING_INTERPOLATION_EXPRESSION)
        break;
    }
  }

  void unmatchedBeginGroup(TokenRef begin) {
    UNIMPLEMENTED();
#if 0
    // We want to ensure that unmatched BeginTokens are reported as
    // errors.  However, the diet parser assumes that groups are well-balanced
    // and will never look at the endGroup token.  This is a nice property that
    // allows us to skip quickly over correct code. By inserting an additional
    // synthetic token in the stream, we can keep ignoring endGroup tokens.
    //
    // [begin] --next--> [tail]
    // [begin] --endG--> [synthetic] --next--> [next] --next--> [tail]
    //
    // This allows the diet parser to skip from [begin] via endGroup to
    // [synthetic] and ignore the [synthetic] token (assuming it's correct),
    // then the error will be reported when parsing the [next] token.
    //
    // For example, tokenize("{[1};") produces:
    //
    // SymbolToken({) --endGroup------------------------+
    //      |                                           |
    //     next                                         |
    //      v                                           |
    // SymbolToken([) --endGroup--+                     |
    //      |                     |                     |
    //     next                   |                     |
    //      v                     |                     |
    // StringToken(1)             |                     |
    //      |                     |                     |
    //     next                   |                     |
    //      v                     |                     |
    // SymbolToken(])<------------+ <-- Synthetic token |
    //      |                                           |
    //     next                                         |
    //      v                                           |
    // UnmatchedToken([)                                |
    //      |                                           |
    //     next                                         |
    //      v                                           |
    // SymbolToken(})<----------------------------------+
    //      |
    //     next
    //      v
    // SymbolToken(;)
    //      |
    //     next
    //      v
    //     EOF
    TokenType type = closeBraceInfoFor(begin);
    appendToken(new SyntheticToken(type, tokenStart, tail));
    begin->endGroup = tail;
    prependErrorToken(new UnmatchedToken(begin));
    recoveryCount++;
#endif
  }

  /// Return true when at EOF.
  [[clang::always_inline]] bool atEndOfFile() const {
    return concrete_scanner()->atEndOfFile();
  }

  void tokenize() {
    while (!atEndOfFile()) {
      int next = advance();

      // Scan the header looking for a language version
      if (next != $EOF) {
        auto oldSize = tokens_.size();
        next = bigHeaderSwitch(next);
        if (next != $EOF && tokens_.size() > 0 &&
            tokens_.last().type() == TokenType::kSCRIPT) {
          oldSize = tokens_.size();
          next = bigHeaderSwitch(next);
        }
        while (next != $EOF && oldSize == tokens_.size()) {
          next = bigHeaderSwitch(next);
        }
        if (tokens_.last().type() == TokenType::kLANGUAGE_VERSION) {
          tokens_.drop(1);
        }
      }

      while (next != $EOF) {
        next = bigSwitch(next);
      }
      if (atEndOfFile()) {
        appendEofToken();
      } else {
        unexpectedEof();
      }
    }

    // Always pretend that there's a line at the end of the file.
    lineStarts.push_back(string_offset() + 1);
  }

  /// Tokenize a (small) part of the data. Used for recovery "option testing".
  ///
  /// Returns the number of recoveries performed.
  int recoveryOptionTokenizer(int next) {
    int iterations = 0;
    while (!atEndOfFile()) {
      while (next != $EOF) {
        // TODO(jensj): Look at number of lines, tokens, parenthesis stack,
        // semi-colon etc, not just number of iterations.
        next = bigSwitch(next);
        iterations++;

        if (iterations > 100) {
          return recoveryCount;
        }
      }
      if (!atEndOfFile()) {
        // $EOF in the middle of the file. Skip it as `tokenize`.
        next = advance();
        iterations++;

        if (iterations > 100) {
          return recoveryCount;
        }
      }
    }
    return recoveryCount;
  }

  int bigHeaderSwitch(int next) {
    if (next != $SLASH) {
      return bigSwitch(next);
    }
    beginToken();
    if ($SLASH != peek()) {
      return tokenizeSlashOrComment(next);
    }
    return tokenizeLanguageVersionOrSingleLineComment(next);
  }

  int bigSwitch(int next) {
    beginToken();
    if (next == $SPACE || next == $TAB || next == $LF || next == $CR) {
      appendWhiteSpace(next);
      next = advance();
      // Sequences of spaces are common, so advance through them fast.
      while (next == $SPACE) {
        // We don't invoke [:appendWhiteSpace(next):] here for efficiency,
        // assuming that it does not do anything for space characters.
        next = advance();
      }
      return next;
    }

    int nextLower = next | 0x20;

    if ($a <= nextLower && nextLower <= $z) {
      if ($r == next) {
        return tokenizeRawStringKeywordOrIdentifier(next);
      }
      return tokenizeKeywordOrIdentifier(next, /* allowDollar = */ true);
    }

    if (next == $CLOSE_PAREN) {
      return appendEndGroup(TokenType::kCLOSE_PAREN, TokenType::kOPEN_PAREN);
    }

    if (next == $OPEN_PAREN) {
      appendBeginGroup(TokenType::kOPEN_PAREN);
      return advance();
    }

    if (next == $SEMICOLON) {
      appendPrecedenceToken(TokenType::kSEMICOLON);
      // Type parameters and arguments cannot contain semicolon.
      discardOpenLt();
      return advance();
    }

    if (next == $PERIOD) {
      return tokenizeDotsOrNumber(next);
    }

    if (next == $COMMA) {
      appendPrecedenceToken(TokenType::kCOMMA);
      return advance();
    }

    if (next == $EQ) {
      return tokenizeEquals(next);
    }

    if (next == $CLOSE_CURLY_BRACKET) {
      return appendEndGroup(TokenType::kCLOSE_CURLY_BRACKET,
                            TokenType::kOPEN_CURLY_BRACKET);
    }

    if (next == $SLASH) {
      return tokenizeSlashOrComment(next);
    }

    if (next == $OPEN_CURLY_BRACKET) {
      appendBeginGroup(TokenType::kOPEN_CURLY_BRACKET);
      return advance();
    }

    if (next == $DQ || next == $SQ) {
      return tokenizeString(next, scan_offset(), /* raw = */ false);
    }

    if (next == $_) {
      return tokenizeKeywordOrIdentifier(next, /* allowDollar = */ true);
    }

    if (next == $COLON) {
      appendPrecedenceToken(TokenType::kCOLON);
      return advance();
    }

    if (next == $LT) {
      return tokenizeLessThan(next);
    }

    if (next == $GT) {
      return tokenizeGreaterThan(next);
    }

    if (next == $BANG) {
      return tokenizeExclamation(next);
    }

    if (next == $OPEN_SQUARE_BRACKET) {
      return tokenizeOpenSquareBracket(next);
    }

    if (next == $CLOSE_SQUARE_BRACKET) {
      return appendEndGroup(TokenType::kCLOSE_SQUARE_BRACKET,
                            TokenType::kOPEN_SQUARE_BRACKET);
    }

    if (next == $AT) {
      return tokenizeAt(next);
    }

    if (next >= $1 && next <= $9) {
      return tokenizeNumber(next);
    }

    if (next == $AMPERSAND) {
      return tokenizeAmpersand(next);
    }

    if (next == $0) {
      return tokenizeHexOrNumber(next);
    }

    if (next == $QUESTION) {
      return tokenizeQuestion(next);
    }

    if (next == $BAR) {
      return tokenizeBar(next);
    }

    if (next == $PLUS) {
      return tokenizePlus(next);
    }

    if (next == $$) {
      return tokenizeKeywordOrIdentifier(next, /* allowDollar = */ true);
    }

    if (next == $MINUS) {
      return tokenizeMinus(next);
    }

    if (next == $STAR) {
      return tokenizeMultiply(next);
    }

    if (next == $CARET) {
      return tokenizeCaret(next);
    }

    if (next == $TILDE) {
      return tokenizeTilde(next);
    }

    if (next == $PERCENT) {
      return tokenizePercent(next);
    }

    if (next == $BACKPING) {
      appendPrecedenceToken(TokenType::kBACKPING);
      return advance();
    }

    if (next == $BACKSLASH) {
      appendPrecedenceToken(TokenType::kBACKSLASH);
      return advance();
    }

    if (next == $HASH) {
      return tokenizeTag(next);
    }

    if (next < 0x1f) {
      return unexpected(next);
    }

    next = currentAsUnicode(next);

    return unexpected(next);
  }

  int tokenizeTag(int next) {
    // # or #!.*[\n\r]
    if (scan_offset() == 0) {
      if (peek() == $BANG) {
        int start = scan_offset();
        bool asciiOnly = true;
        do {
          next = advance();
          if (next > 127) asciiOnly = false;
        } while (next != $LF && next != $CR && next != $EOF);
        if (!asciiOnly) handleUnicode(start);
        appendSubstringToken(TokenType::kSCRIPT_TAG, start, asciiOnly);
        return next;
      }
    }
    appendPrecedenceToken(TokenType::kHASH);
    return advance();
  }

  int tokenizeTilde(int next) {
    // ~ ~/ ~/=
    next = advance();
    if (next == $SLASH) {
      return select($EQ, TokenType::kTILDE_SLASH_EQ, TokenType::kTILDE_SLASH);
    } else {
      appendPrecedenceToken(TokenType::kTILDE);
      return next;
    }
  }

  int tokenizeOpenSquareBracket(int next) {
    // [ [] []=
    next = advance();
    if (next == $CLOSE_SQUARE_BRACKET) {
      return select($EQ, TokenType::kINDEX_EQ, TokenType::kINDEX);
    }
    appendBeginGroup(TokenType::kOPEN_SQUARE_BRACKET);
    return next;
  }

  int tokenizeCaret(int next) {
    // ^ ^=
    return select($EQ, TokenType::kCARET_EQ, TokenType::kCARET);
  }

  int tokenizeQuestion(int next) {
    // ? ?. ?.. ?? ??=
    next = advance();
    if (next == $QUESTION) {
      return select($EQ, TokenType::kQUESTION_QUESTION_EQ,
                    TokenType::kQUESTION_QUESTION);
    } else if (next == $PERIOD) {
      next = advance();
      if ($PERIOD == next) {
        appendPrecedenceToken(TokenType::kQUESTION_PERIOD_PERIOD);
        return advance();
      }
      appendPrecedenceToken(TokenType::kQUESTION_PERIOD);
      return next;
    } else {
      appendPrecedenceToken(TokenType::kQUESTION);
      return next;
    }
  }

  int tokenizeBar(int next) {
    // | || |= ||=
    next = advance();
    if (next == $BAR) {
      next = advance();
      if (LAZY_ASSIGNMENT_ENABLED && next == $EQ) {
        appendPrecedenceToken(TokenType::kBAR_BAR_EQ);
        return advance();
      }
      appendPrecedenceToken(TokenType::kBAR_BAR);
      return next;
    } else if (next == $EQ) {
      appendPrecedenceToken(TokenType::kBAR_EQ);
      return advance();
    } else {
      appendPrecedenceToken(TokenType::kBAR);
      return next;
    }
  }

  int tokenizeAmpersand(int next) {
    // && &= & &&=
    next = advance();
    if (next == $AMPERSAND) {
      next = advance();
      if (LAZY_ASSIGNMENT_ENABLED && next == $EQ) {
        appendPrecedenceToken(TokenType::kAMPERSAND_AMPERSAND_EQ);
        return advance();
      }
      appendPrecedenceToken(TokenType::kAMPERSAND_AMPERSAND);
      return next;
    } else if (next == $EQ) {
      appendPrecedenceToken(TokenType::kAMPERSAND_EQ);
      return advance();
    } else {
      appendPrecedenceToken(TokenType::kAMPERSAND);
      return next;
    }
  }

  int tokenizePercent(int next) {
    // % %=
    return select($EQ, TokenType::kPERCENT_EQ, TokenType::kPERCENT);
  }

  int tokenizeMultiply(int next) {
    // * *=
    return select($EQ, TokenType::kSTAR_EQ, TokenType::kSTAR);
  }

  int tokenizeMinus(int next) {
    // - -- -=
    next = advance();
    if (next == $MINUS) {
      appendPrecedenceToken(TokenType::kMINUS_MINUS);
      return advance();
    } else if (next == $EQ) {
      appendPrecedenceToken(TokenType::kMINUS_EQ);
      return advance();
    } else {
      appendPrecedenceToken(TokenType::kMINUS);
      return next;
    }
  }

  int tokenizePlus(int next) {
    // + ++ +=
    next = advance();
    if ($PLUS == next) {
      appendPrecedenceToken(TokenType::kPLUS_PLUS);
      return advance();
    } else if ($EQ == next) {
      appendPrecedenceToken(TokenType::kPLUS_EQ);
      return advance();
    } else {
      appendPrecedenceToken(TokenType::kPLUS);
      return next;
    }
  }

  int tokenizeExclamation(int next) {
    // ! !=
    // !== is kept for user-friendly error reporting.

    next = advance();
    if (next == $EQ) {
      //was `return select($EQ, TokenType::kBANG_EQ_EQ, TokenType::kBANG_EQ);`
      int next = advance();
      if (next == $EQ) {
        appendPrecedenceToken(TokenType::kBANG_EQ_EQ);
        UNIMPLEMENTED();
        // prependErrorToken(new UnsupportedOperator(tail, tokenStart));
        return advance();
      } else {
        appendPrecedenceToken(TokenType::kBANG_EQ);
        return next;
      }
    }
    appendPrecedenceToken(TokenType::kBANG);
    return next;
  }

  int tokenizeEquals(int next) {
    // = == =>
    // === is kept for user-friendly error reporting.

    // Type parameters and arguments cannot contain any token that
    // starts with '='.
    discardOpenLt();

    next = advance();
    if (next == $EQ) {
      // was `return select($EQ, TokenType::kEQ_EQ_EQ, TokenType::kEQ_EQ);`
      int next = advance();
      if (next == $EQ) {
        appendPrecedenceToken(TokenType::kEQ_EQ_EQ);
        UNIMPLEMENTED();
        // prependErrorToken(new UnsupportedOperator(tail, tokenStart));
        return advance();
      } else {
        appendPrecedenceToken(TokenType::kEQ_EQ);
        return next;
      }
    } else if (next == $GT) {
      appendPrecedenceToken(TokenType::kFUNCTION);
      return advance();
    }
    appendPrecedenceToken(TokenType::kEQ);
    return next;
  }

  int tokenizeGreaterThan(int next) {
    // > >= >> >>= >>> >>>=
    next = advance();
    if ($EQ == next) {
      // Saw `>=` only.
      appendPrecedenceToken(TokenType::kGT_EQ);
      return advance();
    } else if ($GT == next) {
      // Saw `>>` so far.
      next = advance();
      if ($EQ == next) {
        // Saw `>>=` only.
        appendPrecedenceToken(TokenType::kGT_GT_EQ);
        return advance();
      } else if (_enableTripleShift && $GT == next) {
        // Saw `>>>` so far.
        next = advance();
        if ($EQ == next) {
          // Saw `>>>=` only.
          appendPrecedenceToken(TokenType::kGT_GT_GT_EQ);
          return advance();
        } else {
          // Saw `>>>` only.
          appendGtGtGt(TokenType::kGT_GT_GT);
          return next;
        }
      } else {
        // Saw `>>` only.
        appendGtGt(TokenType::kGT_GT);
        return next;
      }
    } else {
      // Saw `>` only.
      appendGt(TokenType::kGT);
      return next;
    }
  }

  int tokenizeLessThan(int next) {
    // < <= << <<=
    next = advance();
    if ($EQ == next) {
      appendPrecedenceToken(TokenType::kLT_EQ);
      return advance();
    } else if ($LT == next) {
      return select($EQ, TokenType::kLT_LT_EQ, TokenType::kLT_LT);
    } else {
      appendBeginGroup(TokenType::kLT);
      return next;
    }
  }

  int tokenizeNumber(int next) {
    int start = scan_offset();
    bool hasSeparators = false;
    bool previousWasSeparator = false;
    while (true) {
      next = advance();
      if ($0 <= next && next <= $9) {
        previousWasSeparator = false;
        continue;
      } else if (next == $_) {
        hasSeparators = true;
        previousWasSeparator = true;
        continue;
      } else if (next == $e || next == $E) {
        if (previousWasSeparator) {
          // Not allowed.
          UNIMPLEMENTED();
          // prependErrorToken(
          //    new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
          //                          start, string_offset()));
        }
        return tokenizeFractionPart(next, start, hasSeparators);
      } else {
        if (next == $PERIOD) {
          if (previousWasSeparator) {
            // Not allowed.
            UNIMPLEMENTED();
            // prependErrorToken(new UnterminatedToken(
            //    Message::messageUnexpectedSeparatorInNumber, start,
            //    string_offset()));
          }
          int nextnext = peek();
          if ($0 <= nextnext && nextnext <= $9) {
            return tokenizeFractionPart(nextnext, start, hasSeparators);
          } else {
            TokenType tokenType = hasSeparators
                                      ? TokenType::kINT_WITH_SEPARATORS
                                      : TokenType::kINT;
            appendSubstringToken(tokenType, start, /* asciiOnly = */ true);
            return next;
          }
        }
        if (previousWasSeparator) {
          // End of the number is a separator; not allowed.
          UNIMPLEMENTED();
          // prependErrorToken(
          //    new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
          //                          start, string_offset()));
        }
        TokenType tokenType =
            hasSeparators ? TokenType::kINT_WITH_SEPARATORS : TokenType::kINT;
        appendSubstringToken(tokenType, start, /* asciiOnly = */ true);
        return next;
      }
    }
  }

  int tokenizeHexOrNumber(int next) {
    int x = peek();
    if (x == $x || x == $X) {
      return tokenizeHex(next);
    }
    return tokenizeNumber(next);
  }

  int tokenizeHex(int next) {
    int start = scan_offset();
    next = advance();  // Advance past the $x or $X.
    bool hasDigits = false;
    bool hasSeparators = false;
    bool previousWasSeparator = false;
    while (true) {
      next = advance();
      if (($0 <= next && next <= $9) || ($A <= next && next <= $F) ||
          ($a <= next && next <= $f)) {
        hasDigits = true;
        previousWasSeparator = false;
      } else if (next == $_) {
        if (!hasDigits) {
          // Not allowed.
          prependErrorToken(
              new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
                                    start, string_offset()));
        }
        hasSeparators = true;
        previousWasSeparator = true;
      } else {
        if (!hasDigits) {
          UNIMPLEMENTED();
          // prependErrorToken(new UnterminatedToken(
          //    Message::messageExpectedHexDigit, start, string_offset()));
          // Recovery
          appendSyntheticSubstringToken(TokenType::kHEXADECIMAL, start,
                                        /* asciiOnly = */ true, "0");
          return next;
        }
        if (previousWasSeparator) {
          // End of the number is a separator; not allowed.
          UNIMPLEMENTED();
          // prependErrorToken(
          //    new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
          //                          start, string_offset()));
        }
        TokenType tokenType = hasSeparators
                                  ? TokenType::kHEXADECIMAL_WITH_SEPARATORS
                                  : TokenType::kHEXADECIMAL;
        appendSubstringToken(tokenType, start, /* asciiOnly = */ true);
        return next;
      }
    }
  }

  int tokenizeDotsOrNumber(int next) {
    int start = scan_offset();
    next = advance();
    if (($0 <= next && next <= $9)) {
      return tokenizeFractionPart(next, start, /* hasSeparators = */ false);
    } else if ($PERIOD == next) {
      next = advance();
      if (next == $PERIOD) {
        next = advance();
        if (next == $QUESTION) {
          appendPrecedenceToken(TokenType::kPERIOD_PERIOD_PERIOD_QUESTION);
          return advance();
        } else {
          appendPrecedenceToken(TokenType::kPERIOD_PERIOD_PERIOD);
          return next;
        }
      } else {
        appendPrecedenceToken(TokenType::kPERIOD_PERIOD);
        return next;
      }
    } else {
      appendPrecedenceToken(TokenType::kPERIOD);
      return next;
    }
  }

  int tokenizeFractionPart(int next, int start, bool hasSeparators) {
    bool hasDigit = false;
    bool previousWasSeparator = false;
    while (true) {
      if ($0 <= next && next <= $9) {
        hasDigit = true;
        previousWasSeparator = false;
      } else if ($_ == next) {
        if (!hasDigit) {
          prependErrorToken(
              new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
                                    start, string_offset()));
        }
        hasSeparators = true;
        previousWasSeparator = true;
      } else if ($e == next || $E == next) {
        if (previousWasSeparator) {
          // Not allowed.
          prependErrorToken(
              new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
                                    start, string_offset()));
        }
        hasDigit = true;
        previousWasSeparator = false;
        next = advance();
        while (next == $_) {
          prependErrorToken(
              new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
                                    start, string_offset()));
          hasSeparators = true;
          previousWasSeparator = true;
          next = advance();
        }
        if (next == $PLUS || next == $MINUS) {
          previousWasSeparator = false;
          next = advance();
        }
        bool hasExponentDigits = false;
        while (true) {
          if ($0 <= next && next <= $9) {
            hasExponentDigits = true;
            previousWasSeparator = false;
          } else if (next == $_) {
            if (!hasExponentDigits) {
              prependErrorToken(new UnterminatedToken(
                  Message::messageUnexpectedSeparatorInNumber, start,
                  string_offset()));
            }
            hasSeparators = true;
            previousWasSeparator = true;
          } else {
            if (!hasExponentDigits) {
              appendSyntheticSubstringToken(TokenType::kDOUBLE, start,
                                            /* asciiOnly = */ true, "0");
              prependErrorToken(
                  new UnterminatedToken(Message::messageMissingExponent,
                                        tokenStart, string_offset()));
              return next;
            }
            break;
          }
          next = advance();
        }
        if (previousWasSeparator) {
          // End of the number is a separator; not allowed.
          prependErrorToken(
              new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
                                    start, string_offset()));
        }

        break;
      } else {
        if (previousWasSeparator) {
          // End of the number is a separator; not allowed.
          prependErrorToken(
              new UnterminatedToken(Message::messageUnexpectedSeparatorInNumber,
                                    start, string_offset()));
        }

        break;
      }
      next = advance();
    }
    if (!hasDigit) {
      // Reduce offset, we already advanced to the token past the period.
      appendSubstringToken(TokenType::kINT, start, /* asciiOnly = */ true,
                           /* extraOffset = */ -1);

      // TODO(ahe): Wrong offset for the period. Cannot call beginToken because
      // the scanner already advanced past the period.
      if ($PERIOD == next) {
        return select($PERIOD, TokenType::kPERIOD_PERIOD_PERIOD,
                      TokenType::kPERIOD_PERIOD);
      }
      appendPrecedenceToken(TokenType::kPERIOD);
      return next;
    }
    TokenType tokenType =
        hasSeparators ? TokenType::kDOUBLE_WITH_SEPARATORS : TokenType::kDOUBLE;
    appendSubstringToken(tokenType, start, /* asciiOnly = */ true);
    return next;
  }

  int tokenizeSlashOrComment(int next) {
    int start = scan_offset();
    next = advance();
    if ($STAR == next) {
      return tokenizeMultiLineComment(next, start);
    } else if ($SLASH == next) {
      return tokenizeSingleLineComment(next, start);
    } else if ($EQ == next) {
      appendPrecedenceToken(TokenType::kSLASH_EQ);
      return advance();
    } else {
      appendPrecedenceToken(TokenType::kSLASH);
      return next;
    }
  }

  int tokenizeLanguageVersionOrSingleLineComment(int next) {
    int start = scan_offset();
    next = advance();

    // Dart doc
    if ($SLASH == peek()) {
      return tokenizeSingleLineComment(next, start);
    }

    // "@dart"
    next = advance();
    while ($SPACE == next) {
      next = advance();
    }
    if ($AT != next) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }
    next = advance();
    if ($d != next) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }
    next = advance();
    if ($a != next) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }
    next = advance();
    if ($r != next) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }
    next = advance();
    if ($t != next) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }
    next = advance();

    // "="
    while ($SPACE == next) {
      next = advance();
    }
    if ($EQ != next) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }
    next = advance();

    // major
    while ($SPACE == next) {
      next = advance();
    }
    int major = 0;
    int majorStart = scan_offset();
    while (isDigit(next)) {
      major = major * 10 + next - $0;
      next = advance();
    }
    if (scan_offset() == majorStart) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }

    // minor
    if ($PERIOD != next) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }
    next = advance();
    int minor = 0;
    int minorStart = scan_offset();
    while (isDigit(next)) {
      minor = minor * 10 + next - $0;
      next = advance();
    }
    if (scan_offset() == minorStart) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }

    // trailing spaces
    while ($SPACE == next) {
      next = advance();
    }
    if (next != $LF && next != $CR && next != $EOF) {
      return tokenizeSingleLineCommentRest(next, start, /* dartdoc = */ false);
    }

    TokenRef languageVersion = createLanguageVersionToken(start, major, minor);
    if (languageVersionChanged != nullptr) {
      // TODO(danrubel): make this required and remove the languageVersion field
      languageVersionChanged(this, languageVersion);
    }
    if (includeComments) {
      UNIMPLEMENTED();
      // _appendToCommentStream(languageVersion);
    }
    return next;
  }

  int tokenizeSingleLineComment(int next, int start) {
    bool dartdoc = $SLASH == peek();
    next = advance();
    return tokenizeSingleLineCommentRest(next, start, dartdoc);
  }

  int tokenizeSingleLineCommentRest(int next, int start, bool dartdoc) {
    bool asciiOnly = true;
    while (true) {
      if (next > 127) asciiOnly = false;
      if ($LF == next || $CR == next || $EOF == next) {
        if (!asciiOnly) handleUnicode(start);
        if (dartdoc) {
          appendDartDoc(start, TokenType::kSINGLE_LINE_COMMENT, asciiOnly);
        } else {
          appendComment(start, TokenType::kSINGLE_LINE_COMMENT, asciiOnly);
        }
        return next;
      }
      next = advance();
    }
  }

  int tokenizeMultiLineComment(int next, int start) {
    bool asciiOnlyComment = true;  // Track if the entire comment is ASCII.
    bool asciiOnlyLines = true;    // Track ASCII since the last handleUnicode.
    int unicodeStart = start;
    int nesting = 1;
    next = advance();
    bool dartdoc = $STAR == next;
    while (true) {
      if ($EOF == next) {
        if (!asciiOnlyLines) handleUnicode(unicodeStart);
        prependErrorToken(new UnterminatedToken(
            Message::messageUnterminatedComment, tokenStart, string_offset()));
        advanceAfterError();
        break;
      } else if ($STAR == next) {
        next = advance();
        if ($SLASH == next) {
          --nesting;
          if (0 == nesting) {
            if (!asciiOnlyLines) handleUnicode(unicodeStart);
            next = advance();
            if (dartdoc) {
              appendDartDoc(start, TokenType::kMULTI_LINE_COMMENT,
                            asciiOnlyComment);
            } else {
              appendComment(start, TokenType::kMULTI_LINE_COMMENT,
                            asciiOnlyComment);
            }
            break;
          } else {
            next = advance();
          }
        }
      } else if ($SLASH == next) {
        next = advance();
        if ($STAR == next) {
          next = advance();
          ++nesting;
        }
      } else if (next == $LF) {
        if (!asciiOnlyLines) {
          // Synchronize the string offset in the utf8 scanner.
          handleUnicode(unicodeStart);
          asciiOnlyLines = true;
          unicodeStart = scan_offset();
        }
        lineFeedInMultiline();
        next = advance();
      } else {
        if (next > 127) {
          asciiOnlyLines = false;
          asciiOnlyComment = false;
        }
        next = advance();
      }
    }
    return next;
  }

  void appendComment(int start, TokenType type, bool asciiOnly) {
    if (!includeComments) return;
    CommentToken* newComment = createCommentToken(type, start, asciiOnly);
    _appendToCommentStream(newComment);
  }

  void appendDartDoc(int start, TokenType type, bool asciiOnly) {
    if (!includeComments) return;
    CommentToken* newComment = createDartDocToken(type, start, asciiOnly);
    _appendToCommentStream(newComment);
  }

  /**
   * Append the given token to the [tail] of the current stream of tokens.
   */
  [[clang::always_inline]] void appendToken(TokenRef token) {
    assert(token.index() == (tokens_.size() - 1));
#if 0
    tail->set_next(token);
    token->set_previous(tail);
    tail = token;
    if (comments != nullptr && comments == token->precedingComments()) {
      comments = nullptr;
      commentsTail = nullptr;
    } else {
      // It is the responsibility of the caller to construct the token
      // being appended with preceding comments if any
      assert(comments == nullptr || token->isSynthetic() ||
             token->isErrorToken());
    }
#endif
  }

  void _appendToCommentStream(CommentToken* newComment) {
    UNIMPLEMENTED();
#if 0
    if (comments == nullptr) {
      comments = newComment;
      commentsTail = comments;
    } else {
      commentsTail->set_next(newComment);
      commentsTail->next()->set_previous(commentsTail);
      commentsTail = commentsTail->next();
    }
#endif
  }

  int tokenizeRawStringKeywordOrIdentifier(int next) {
    // [next] is $r.
    int nextnext = peek();
    if (nextnext == $DQ || nextnext == $SQ) {
      int start = scan_offset();
      next = advance();
      return tokenizeString(next, start, /* raw = */ true);
    }
    return tokenizeKeywordOrIdentifier(next, /* allowDollar = */ true);
  }

  int tokenizeKeywordOrIdentifier(int next, bool allowDollar) {
    KeywordState* state = KeywordState::KEYWORD_STATE;
    int start = scan_offset();
    // We allow a leading capital character.
    if ($A <= next && next <= $Z) {
      state = state->nextCapital(next);
      next = advance();
    } else if ($a <= next && next <= $z) {
      // Do the first next call outside the loop to avoid an additional test
      // and to make the loop monomorphic.
      state = state->next(next);
      next = advance();
    }
    while (state != nullptr && $a <= next && next <= $z) {
      state = state->next(next);
      next = advance();
    }
    if (state == nullptr) {
      return tokenizeIdentifier(next, start, allowDollar);
    }
    TokenType keyword = state->keyword;
    if (keyword == TokenType::kEOF) {
      return tokenizeIdentifier(next, start, allowDollar);
    }
    if (!_forAugmentationLibrary && keyword == TokenType::kAUGMENT) {
      return tokenizeIdentifier(next, start, allowDollar);
    }
    if (($A <= next && next <= $Z) || ($0 <= next && next <= $9) ||
        next == $_ || (allowDollar && next == $$)) {
      return tokenizeIdentifier(next, start, allowDollar);
    } else {
      appendKeywordToken(keyword);
      return next;
    }
  }

  /**
   * [allowDollar] can exclude '$', which is not allowed as part of a string
   * interpolation identifier.
   */
  int tokenizeIdentifier(int next, int start, bool allowDollar) {
    while (true) {
      if (_isIdentifierChar(next, allowDollar)) {
        next = advance();
      } else {
        // Identifier ends here.
        if (start == scan_offset()) {
          return unexpected(next);
        } else {
          appendSubstringToken(TokenType::kIDENTIFIER, start,
                               /* asciiOnly = */ true);
        }
        break;
      }
    }
    return next;
  }

  int tokenizeAt(int next) {
    appendPrecedenceToken(TokenType::kAT);
    return advance();
  }

  int tokenizeString(int next, int start, bool raw) {
    int quoteChar = next;
    next = advance();
    if (quoteChar == next) {
      next = advance();
      if (quoteChar == next) {
        // Multiline string.
        return tokenizeMultiLineString(quoteChar, start, raw);
      } else {
        // Empty string.
        appendSubstringToken(TokenType::kSTRING, start, /* asciiOnly = */ true);
        return next;
      }
    }
    if (raw) {
      return tokenizeSingleLineRawString(next, quoteChar, start);
    } else {
      return tokenizeSingleLineString(next, quoteChar, start);
    }
  }

  /**
   * [next] is the first character after the quote.
   * [quoteStart] is the scan_offset() of the quote.
   *
   * The token contains a substring of the source file, including the
   * string quotes, backslashes for escaping. For interpolated strings,
   * the parts before and after are separate tokens.
   *
   *   "a $b c"
   *
   * gives StringToken("a $), StringToken(b) and StringToken( c").
   */
  int tokenizeSingleLineString(int next, int quoteChar, int quoteStart) {
    int start = quoteStart;
    bool asciiOnly = true;
    while (next != quoteChar) {
      if (next == $BACKSLASH) {
        next = advance();
      } else if (next == $$) {
        if (!asciiOnly) handleUnicode(start);
        next = tokenizeStringInterpolation(start, asciiOnly);
        start = scan_offset();
        asciiOnly = true;
        continue;
      }
      if (next <= $CR && (next == $LF || next == $CR || next == $EOF)) {
        if (!asciiOnly) handleUnicode(start);
        unterminatedString(quoteChar, quoteStart, start,
                           /*asciiOnly:*/ asciiOnly, /*isMultiLine:*/ false,
                           /*isRaw:*/ false);
        return next;
      }
      if (next > 127) asciiOnly = false;
      next = advance();
    }
    if (!asciiOnly) handleUnicode(start);
    // Advance past the quote character.
    next = advance();
    appendSubstringToken(TokenType::kSTRING, start, asciiOnly);
    return next;
  }

  int tokenizeStringInterpolation(int start, bool asciiOnly) {
    appendSubstringToken(TokenType::kSTRING, start, asciiOnly);
    beginToken();  // $ starts here.
    int next = advance();
    if (next == $OPEN_CURLY_BRACKET) {
      return tokenizeInterpolatedExpression(next);
    } else {
      return tokenizeInterpolatedIdentifier(next);
    }
  }

  int tokenizeInterpolatedExpression(int next) {
    appendBeginGroup(TokenType::kSTRING_INTERPOLATION_EXPRESSION);
    beginToken();      // The expression starts here.
    next = advance();  // Move past the curly bracket.
    while (next != $EOF && next != $STX) {
      next = bigSwitch(next);
    }
    if (next == $EOF) {
      beginToken();
      discardInterpolation();
      return next;
    }
    next = advance();  // Move past the $STX.
    beginToken();      // The string interpolation suffix starts here.
    return next;
  }

  int tokenizeInterpolatedIdentifier(int next) {
    appendPrecedenceToken(TokenType::kSTRING_INTERPOLATION_IDENTIFIER);

    if (($a <= next && next <= $z) || ($A <= next && next <= $Z) ||
        (next == $_)) {
      beginToken();  // The identifier starts here.
      next = tokenizeKeywordOrIdentifier(next, /* allowDollar = */ false);
    } else {
      beginToken();  // The synthetic identifier starts here.
      appendSyntheticSubstringToken(TokenType::kIDENTIFIER, scan_offset(),
                                    /* asciiOnly = */ true, "");
      prependErrorToken(
          new UnterminatedToken(Message::messageUnexpectedDollarInString,
                                tokenStart, string_offset()));
    }
    beginToken();  // The string interpolation suffix starts here.
    return next;
  }

  int tokenizeSingleLineRawString(int next, int quoteChar, int quoteStart) {
    bool asciiOnly = true;
    while (next != $EOF) {
      if (next == quoteChar) {
        if (!asciiOnly) handleUnicode(quoteStart);
        next = advance();
        appendSubstringToken(TokenType::kSTRING, quoteStart, asciiOnly);
        return next;
      } else if (next == $LF || next == $CR) {
        if (!asciiOnly) handleUnicode(quoteStart);
        unterminatedString(quoteChar, quoteStart, quoteStart,
                           /*asciiOnly:*/ asciiOnly, /*isMultiLine:*/ false,
                           /*isRaw:*/ true);
        return next;
      } else if (next > 127) {
        asciiOnly = false;
      }
      next = advance();
    }
    if (!asciiOnly) handleUnicode(quoteStart);
    unterminatedString(quoteChar, quoteStart, quoteStart,
                       /*asciiOnly:*/ asciiOnly, /*isMultiLine:*/ false,
                       /*isRaw:*/ true);
    return next;
  }

  int tokenizeMultiLineRawString(int quoteChar, int quoteStart) {
    bool asciiOnlyString = true;
    bool asciiOnlyLine = true;
    int unicodeStart = quoteStart;
    int next = advance();  // Advance past the (last) quote (of three).
    while (next != $EOF) {
      while (next != quoteChar) {
        if (next == $LF) {
          if (!asciiOnlyLine) {
            // Synchronize the string offset in the utf8 scanner.
            handleUnicode(unicodeStart);
            asciiOnlyLine = true;
            unicodeStart = scan_offset();
          }
          lineFeedInMultiline();
        } else if (next > 127) {
          asciiOnlyLine = false;
          asciiOnlyString = false;
        }
        next = advance();
        if (next == $EOF) goto outer;
      }
      next = advance();
      if (next == quoteChar) {
        next = advance();
        if (next == quoteChar) {
          if (!asciiOnlyLine) handleUnicode(unicodeStart);
          next = advance();
          appendSubstringToken(TokenType::kSTRING, quoteStart, asciiOnlyString);
          return next;
        }
      }
    }
  outer:
    if (!asciiOnlyLine) handleUnicode(unicodeStart);
    unterminatedString(quoteChar, quoteStart, quoteStart,
                       /*asciiOnly:*/ asciiOnlyLine, /*isMultiLine:*/ true,
                       /*isRaw:*/ true);
    return next;
  }

  int tokenizeMultiLineString(int quoteChar, int quoteStart, bool raw) {
    if (raw) return tokenizeMultiLineRawString(quoteChar, quoteStart);
    int start = quoteStart;
    bool asciiOnlyString = true;
    bool asciiOnlyLine = true;
    int unicodeStart = start;
    int next = advance();  // Advance past the (last) quote (of three).
    while (next != $EOF) {
      if (next == $$) {
        if (!asciiOnlyLine) handleUnicode(unicodeStart);
        next = tokenizeStringInterpolation(start, asciiOnlyString);
        start = scan_offset();
        unicodeStart = start;
        asciiOnlyString = true;  // A new string token is created for the rest.
        asciiOnlyLine = true;
        continue;
      }
      if (next == quoteChar) {
        next = advance();
        if (next == quoteChar) {
          next = advance();
          if (next == quoteChar) {
            if (!asciiOnlyLine) handleUnicode(unicodeStart);
            next = advance();
            appendSubstringToken(TokenType::kSTRING, start, asciiOnlyString);
            return next;
          }
        }
        continue;
      }
      if (next == $BACKSLASH) {
        next = advance();
        if (next == $EOF) break;
      }
      if (next == $LF) {
        if (!asciiOnlyLine) {
          // Synchronize the string offset in the utf8 scanner.
          handleUnicode(unicodeStart);
          asciiOnlyLine = true;
          unicodeStart = scan_offset();
        }
        lineFeedInMultiline();
      } else if (next > 127) {
        asciiOnlyString = false;
        asciiOnlyLine = false;
      }
      next = advance();
    }
    if (!asciiOnlyLine) handleUnicode(unicodeStart);
    unterminatedString(quoteChar, quoteStart, start,
                       /*asciiOnly:*/ asciiOnlyString, /*isMultiLine:*/ true,
                       /*isRaw:*/ false);
    return next;
  }

  int unexpected(int character) {
    UNIMPLEMENTED();
#if 0
    auto errorToken = buildUnexpectedCharacterToken(character, tokenStart);
    if (errorToken.isNonAsciiIdentifierToken()) {
      int charOffset;
      std::vector<int> codeUnits;
      if (tail.type() == TokenType::kIDENTIFIER &&
          tail->charEnd() == tokenStart) {
        charOffset = tail->charOffset();
        // TODO: codeUnits.addAll(tail.lexeme.codeUnits);
        UNIMPLEMENTED();
        tail = tail->previous();
      } else {
        charOffset = errorToken->charOffset();
      }
      codeUnits.push_back(
          static_cast<NonAsciiIdentifierToken*>(errorToken)->character());
      prependErrorToken(errorToken);
      int next = advanceAfterError();
      while (_isIdentifierChar(next, /* allowDollar = */ true)) {
        codeUnits.push_back(next);
        next = advance();
      }
      UNIMPLEMENTED();
      // appendToken(StringToken::FromString(
      //    TokenType::kIDENTIFIER, new String.fromCharCodes(codeUnits), charOffset,
      //    precedingComments: comments));
      return next;
    } else {
      prependErrorToken(errorToken);
      return advanceAfterError();
    }
#endif
  }

  void unexpectedEof() {
    UNIMPLEMENTED();
#if 0
    ErrorToken* errorToken = buildUnexpectedCharacterToken($EOF, tokenStart);
    prependErrorToken(errorToken);
#endif
  }

  void unterminatedString(int quoteChar,
                          int quoteStart,
                          int start,
                          bool asciiOnly,
                          bool isMultiLine,
                          bool isRaw) {
    std::print(stderr, "Unterminated string at {}", quoteStart);
    UNIMPLEMENTED();
    /*
    String suffix = new String.fromCharCodes(
        isMultiLine ? [quoteChar, quoteChar, quoteChar] : [quoteChar]);
    String prefix = isRaw ? 'r$suffix' : suffix;

    appendSyntheticSubstringToken(TokenType::kSTRING, start, asciiOnly, suffix);
    // Ensure that the error is reported on a visible token
    int errorStart = tokenStart < string_offset() ? tokenStart : quoteStart;
    prependErrorToken(new UnterminatedString(prefix, errorStart, string_offset()));
    */
  }

  int advanceAfterError() {
    if (atEndOfFile()) return $EOF;
    return advance();  // Ensure progress.
  }
};

/**
 * Scanner that reads from a UTF-8 encoded list of bytes and creates tokens
 * that points to substrings.
 */
class Utf8BytesScanner : public AbstractScanner<Utf8BytesScanner> {
 public:
  /**
   * Points to the offset of the last byte returned by [advance].
   *
   * After invoking [currentAsUnicode], the [byteOffset] points to the last
   * byte that is part of the (unicode or ASCII) character. That way, [advance]
   * can always increase the byte offset by 1.
   */
  int byteOffset = -1;

  /**
   * The getter [scanOffset] is expected to return the index where the current
   * character *starts*. In case of a non-ascii character, after invoking
   * [currentAsUnicode], the byte offset points to the *last* byte.
   *
   * This field keeps track of the number of bytes for the current unicode
   * character. For example, if bytes 7,8,9 encode one unicode character, the
   * [byteOffset] is 9 (after invoking [currentAsUnicode]). The [scanSlack]
   * will be 2, so that [scanOffset] returns 7.
   */
  int scanSlack = 0;

  /**
   * Holds the [byteOffset] value for which the current [scanSlack] is valid.
   */
  int scanSlackOffset = -1;

  /**
   * Returns the byte offset of the first byte that belongs to the current
   * character.
   */
  int scan_offset() const {
    if (byteOffset == scanSlackOffset) {
      return byteOffset - scanSlack;
    } else {
      return byteOffset;
    }
  }

  /**
   * The difference between the number of bytes and the number of corresponding
   * string characters, up to the current [byteOffset].
   */
  int utf8Slack = 0;

  Utf8BytesScanner(uint8_t* bytes,
                   int length,
                   ScannerConfiguration* configuration = nullptr,
                   bool includeComments = false,
                   LanguageVersionChanged languageVersionChanged = nullptr,
                   bool allowLazyStrings = true)
      : AbstractScanner(configuration,
                        includeComments,
                        languageVersionChanged,
                        length,
                        allowLazyStrings),
        bytes_(bytes),
        bytesLengthMinusOne_(length - 1) {
    // TODO
    // Skip a leading BOM.
    if (containsBomAt(/* offset = */ 0)) {
      byteOffset += 3;
      utf8Slack += 3;
    }
  }

#if 0
  Utf8BytesScanner(const Utf8BytesScanner& copyFrom)
      : bytes_ = copyFrom.bytes_,
        bytesLengthMinusOne_ = copyFrom.bytesLengthMinusOne_,
        super.recoveryOptionScanner(copyFrom) {
    this.byteOffset = copyFrom.byteOffset;
    this.scanSlack = copyFrom.scanSlack;
    this.scanSlackOffset = copyFrom.scanSlackOffset;
    this.utf8Slack = copyFrom.utf8Slack;
  }
#endif

  Utf8BytesScanner createRecoveryOptionScanner() {
    // return new Utf8BytesScanner.createRecoveryOptionScanner(this);
    UNIMPLEMENTED();
  }

  bool containsBomAt(int offset) {
    constexpr uint8_t BOM_UTF8[3] = {0xEF, 0xBB, 0xBF};

    return offset + 2 <= bytesLengthMinusOne_ &&
           bytes_[offset] == BOM_UTF8[0] && bytes_[offset + 1] == BOM_UTF8[1] &&
           bytes_[offset + 2] == BOM_UTF8[2];
  }

  // @override
  // @pragma('vm:unsafe:no-bounds-checks')
  [[clang::always_inline]] int advance() {
    // Always increment so byteOffset goes past the end.
    ++byteOffset;
    if (byteOffset > bytesLengthMinusOne_) return $EOF;
    return bytes_[byteOffset];
  }

  // @override
  // @pragma('vm:unsafe:no-bounds-checks')
  [[clang::always_inline]] int peek() {
    int next = byteOffset + 1;
    if (next > bytesLengthMinusOne_) return $EOF;
    return bytes_[next];
  }

  /// Returns the unicode code point starting at the byte offset [startOffset]
  /// with the byte [nextByte].
  int nextCodePoint(int startOffset, int nextByte) {
    int expectedHighBytes;
    if (nextByte < 0xC2) {
      expectedHighBytes = 1;  // Bad code unit.
    } else if (nextByte < 0xE0) {
      expectedHighBytes = 2;
    } else if (nextByte < 0xF0) {
      expectedHighBytes = 3;
    } else if (nextByte < 0xF5) {
      expectedHighBytes = 4;
    } else {
      expectedHighBytes = 1;  // Bad code unit.
    }
    int numBytes = 0;
    for (int i = 0; i < expectedHighBytes; i++) {
      int next = byteOffset + i;
      if (next > bytesLengthMinusOne_) break;
      if (bytes_[next] < 0x80) {
        break;
      }
      numBytes++;
    }
    int end = startOffset + numBytes;
    byteOffset = end - 1;
    if (expectedHighBytes == 1 || numBytes != expectedHighBytes) {
      return unicodeReplacementCharacter;
    }
    UNIMPLEMENTED();
#if 0
    // TODO(lry): measurably slow, decode creates first a Utf8Decoder and a
    // _Utf8Decoder instance. Also the sublist is eagerly allocated.
    String codePoint =
        utf8.decode(bytes_.sublist(startOffset, end), allowMalformed: true);
    if (codePoint.length == 0) {
      // The UTF-8 decoder discards leading BOM characters.
      // TODO(floitsch): don't just assume that removed characters were the
      // BOM.
      assert(containsBomAt(startOffset));
      codePoint = new String.fromCharCode(unicodeBomCharacterRune);
    }
    if (codePoint.length == 1) {
      utf8Slack += (numBytes - 1);
      scanSlack = numBytes - 1;
      scanSlackOffset = byteOffset;
      return codePoint.codeUnitAt(/* index = */ 0);
    } else if (codePoint.length == 2) {
      utf8Slack += (numBytes - 2);
      scanSlack = numBytes - 1;
      scanSlackOffset = byteOffset;
      stringOffsetSlackOffset = byteOffset;
      // In case of a surrogate pair, return a single code point.
      // Gracefully degrade given invalid UTF-8.
      RuneIterator runes = codePoint.runes.iterator;
      if (!runes.moveNext()) return unicodeReplacementCharacter;
      int codeUnit = runes.current;
      return !runes.moveNext() ? codeUnit : unicodeReplacementCharacter;
    } else {
      return unicodeReplacementCharacter;
    }
#endif
  }

  int lastUnicodeOffset = -1;

  // @override
  int currentAsUnicode(int next) {
    if (next < 128) return next;
    // Check if currentAsUnicode was already invoked.
    if (byteOffset == lastUnicodeOffset) return next;
    int res = nextCodePoint(byteOffset, next);
    lastUnicodeOffset = byteOffset;
    return res;
  }

  enum Type {
    kLatin1 = 0,     // Latin-1 code point [U+0000, U+00FF].
    kBMP,            // Basic Multilingual Plane code point [U+0000, U+FFFF].
    kSupplementary,  // Supplementary code point [U+010000, U+10FFFF].
  };

  [[clang::always_inline]] static bool IsTrailByte(uint8_t code_unit) {
    return (code_unit & 0xC0) == 0x80;
  }

  [[clang::always_inline]] static bool IsNonShortestForm(
      uint32_t code_point,
      size_t num_code_units) {
    // Minimum values of code points used to check shortest form.
    constexpr uint32_t kOverlongMinimum[7] = {0,  // Padding.
                                              0x0,     0x80,       0x800,
                                              0x10000, 0xFFFFFFFF, 0xFFFFFFFF};
    return code_point < kOverlongMinimum[num_code_units];
  }

  [[clang::always_inline]] static bool IsLatin1SequenceStart(
      uint8_t code_unit) {
    // Check if utf8 sequence is the start of a codepoint <= U+00FF
    return (code_unit <= 0xC3);
  }

  [[clang::always_inline]] static bool IsSupplementarySequenceStart(
      uint8_t code_unit) {
    // Check if utf8 sequence is the start of a codepoint >= U+10000.
    return (code_unit >= 0xF0);
  }

  // Returns the most restricted coding form in which the sequence of utf8
  // characters in 'utf8_array' can be represented in, and the number of
  // code units needed in that form.
  static intptr_t CodeUnitCount(const uint8_t* utf8_array, intptr_t array_len) {
    intptr_t len = 0;
    Type char_type = kLatin1;
    for (intptr_t i = 0; i < array_len; i++) {
      uint8_t code_unit = utf8_array[i];
      if (!IsTrailByte(code_unit)) {
        ++len;
        if (!IsLatin1SequenceStart(code_unit)) {          // > U+00FF
          if (IsSupplementarySequenceStart(code_unit)) {  // >= U+10000
            char_type = kSupplementary;
            ++len;
          } else if (char_type == kLatin1) {
            char_type = kBMP;
          }
        }
      }
    }
    return len;
  }

  // @override
  void handleUnicode(int startScanOffset) {
    //    UNIMPLEMENTED();
    //#if 0
    int end = byteOffset;
    // TODO(lry): this measurably slows down the scanner for files with unicode.
    int codeUnits =
        CodeUnitCount(bytes_ + startScanOffset, end - startScanOffset);
    utf8Slack += (end - startScanOffset) - codeUnits;
    //#endif
  }

  /**
   * This field remembers the byte offset of the last character decoded with
   * [nextCodePoint] that used two code units in UTF-16.
   *
   * [nextCodePoint] returns a single code point for each unicode character,
   * even if it needs two code units in UTF-16.
   *
   * For example, '\u{1d11e}' uses 4 bytes in UTF-8, and two code units in
   * UTF-16. The [utf8Slack] is therefore 2. After invoking [nextCodePoint], the
   * [byteOffset] points to the last (of 4) bytes. The [stringOffset] should
   * return the offset of the first one, which is one position more left than
   * the [utf8Slack].
   */
  int stringOffsetSlackOffset = -1;

  // @override
  int string_offset() const {
    if (stringOffsetSlackOffset == byteOffset) {
      return byteOffset - utf8Slack - 1;
    } else {
      return byteOffset - utf8Slack;
    }
  }

  // @override
  TokenRef createSubstringToken(TokenType type,
                                int start,
                                bool asciiOnly,
                                int extraOffset,
                                bool allowLazy) {
    std::string_view content{
        reinterpret_cast<char*>(bytes_ + start),
        static_cast<size_t>(byteOffset + extraOffset - start)};
    return tokens_.AddStringToken(type, tokenStart, comments, content,
                                  asciiOnly, allowLazy);
    // return new StringToken(type, tokenStart, comments, content, asciiOnly,
    //                       allowLazy);
    UNIMPLEMENTED();
#if 0
    return new StringTokenImpl.fromUtf8Bytes(
        type, bytes_, start, byteOffset + extraOffset, asciiOnly, tokenStart,
        precedingComments: comments, allowLazy: allowLazy);
#endif
  }

  // @override
  TokenRef createSyntheticSubstringToken(
      TokenType type,
      int start,
      bool asciiOnly,
      const std::string_view& syntheticChars) {
    UNIMPLEMENTED();
#if 0
    String value = syntheticChars.length == 0
        ? canonicalizeUtf8SubString(bytes_, start, byteOffset, asciiOnly)
        : canonicalizeString(
            decodeString(bytes_, start, byteOffset, asciiOnly) +
                syntheticChars);
    return new SyntheticStringToken(
        type, value, tokenStart, value.length - syntheticChars.length);
#endif
  }

  // @override
  CommentToken* createCommentToken(TokenType type,
                                   int start,
                                   bool asciiOnly,
                                   int extraOffset = 0) {
    UNIMPLEMENTED();
#if 0
    return new CommentTokenImpl.fromUtf8Bytes(
        type, bytes_, start, byteOffset + extraOffset, asciiOnly, tokenStart);
#endif
  }

  // @override
  DartDocToken* createDartDocToken(TokenType type,
                                   int start,
                                   bool asciiOnly,
                                   int extraOffset = 0) {
    UNIMPLEMENTED();
#if 0
    return new DartDocToken.fromUtf8Bytes(
        type, bytes_, start, byteOffset + extraOffset, asciiOnly, tokenStart);
#endif
  }

  // @override
  TokenRef createLanguageVersionToken(int start, int major, int minor) {
    std::string_view content{reinterpret_cast<char*>(bytes_ + start),
                             static_cast<size_t>(byteOffset - start)};
    return tokens_.AddStringToken(TokenType::kLANGUAGE_VERSION, tokenStart,
                                  comments, content, true, true);
  }

  // @override
  // This class used to require zero-terminated input, so we only return true
  // once advance has been out of bounds.
  // TODO(jensj): This should probably change.
  // It's at least used in tests (where the eof token has its offset reduced
  // by one to 'fix' this.)
  [[clang::always_inline]] bool atEndOfFile() const {
    return byteOffset > bytesLengthMinusOne_;
  }

 private:
  /// The raw file content.
  uint8_t* const bytes_;
  const int bytesLengthMinusOne_;
};

TokenWriter ScanUtf8(uint8_t* bytes, size_t length) {
  if (KeywordState::KEYWORD_STATE == nullptr) {
    KeywordState::Init();
  }
  Utf8BytesScanner scanner(
      bytes, length, new ScannerConfiguration(/*enableTripleShift=*/true));
  scanner.tokenize();
  return std::move(scanner.tokens_);
}