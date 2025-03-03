// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "package:expect/expect.dart";

void main() {
  checkMatch(r'abc', 'xxxabcxxx', expectedCaptures: [(start: 3, end: 6)]);

  checkMatch(
    r'abc|(d.*)',
    'xxxabcxxx',
    expectedCaptures: [(start: 3, end: 6), null],
  );

  checkMatch(
    r'(?<foo>d.*)|abc',
    'xxxabcxxx',
    expectedCaptures: [(start: 3, end: 6), null],
    expectedNamedCaptures: {},
  );

  checkMatch(
    r'(?:(?:(\d+)|(\w+)),)+',
    '--- aaa,111,bbb,222,---',
    expectedCaptures: [(start: 4, end: 20), (start: 16, end: 19), null],
  );

  checkMatch(
    r'(?:(?:(\d+)|(\w+)),)+',
    '--- aaa,111,222,bbb,---',
    expectedCaptures: [(start: 4, end: 20), null, (start: 16, end: 19)],
  );

  checkMatch(
    r'(?<!\\)\{\{(\w*)\}\}',
    'A captured word I {{capture}}',
    expectedCaptures: [(start: 18, end: 29), (start: 20, end: 27)],
  );
  checkMatch(
    r'((let)|(const)) (\w+)',
    '  let v  ',
    expectedCaptures: [
      (start: 2, end: 7),
      (start: 2, end: 5),
      (start: 2, end: 5),
      null,
      (start: 6, end: 7),
    ],
  );
  checkMatch(
    r'((let)|(const)) (\w+)',
    '  const vv  ',
    expectedCaptures: [
      (start: 2, end: 10),
      (start: 2, end: 7),
      null,
      (start: 2, end: 7),
      (start: 8, end: 10),
    ],
    expectedNamedCaptures: {},
  );
  checkMatch(
    r'((?<let>let)|(?<const>const)) (?<name>\w+)',
    '  const vv  ',
    expectedCaptures: [
      (start: 2, end: 10),
      (start: 2, end: 7),
      null,
      (start: 2, end: 7),
      (start: 8, end: 10),
    ],
    expectedNamedCaptures: {
      'const': (start: 2, end: 7),
      'name': (start: 8, end: 10),
    },
  );
  checkMatch(
    r'((?<let>let)|(?<const>const)) (?<name>\w+)',
    '  let v  ',
    expectedCaptures: [
      (start: 2, end: 7),
      (start: 2, end: 5),
      (start: 2, end: 5),
      null,
      (start: 6, end: 7),
    ],
    expectedNamedCaptures: {
      'let': (start: 2, end: 5),
      'name': (start: 6, end: 7),
    },
  );
}

final reFactories = [
  (pattern) => RegExp(pattern),
  (pattern) => RegExp(pattern, multiLine: true),
  (pattern) => RegExp(pattern, caseSensitive: false),
  (pattern) => RegExp(pattern, unicode: true),
  (pattern) => RegExp(pattern, dotAll: true),
];

void checkMatch(
  String pattern,
  String input, {
  List<({int start, int end})?> expectedCaptures = const [],
  Map<String, ({int start, int end})> expectedNamedCaptures = const {},
}) {
  for (var f in reFactories) {
    final re = f(pattern);
    checkMatchImpl(
      re.firstMatch(input)!,
      input,
      expectedCaptures: expectedCaptures,
      expectedNamedCaptures: expectedNamedCaptures,
    );
    for (var pos = 0; pos < input.length; pos++) {
      final m = re.matchAsPrefix(input.substring(pos));
      if (m is RegExpMatch) {
        checkMatchImpl(
          m,
          input,
          expectedCaptures: [
            for (var capture in expectedCaptures)
              capture == null ? null : translateRange(-pos, capture),
          ],
          expectedNamedCaptures: {
            for (var e in expectedNamedCaptures.entries)
              e.key: translateRange(-pos, e.value),
          },
        );
        break;
      }
    }
  }
}

void checkMatchImpl(
  RegExpMatch match,
  String input, {
  List<({int start, int end})?> expectedCaptures = const [],
  Map<String, ({int start, int end})> expectedNamedCaptures = const {},
}) {
  Expect.listEquals(expectedCaptures, match.captures);
  Expect.mapEquals(expectedNamedCaptures, match.namedCaptures);

  for (var (i, capture) in match.captures.indexed) {
    if (capture != null) {
      Expect.equals(
        match[i],
        match.input.substring(capture.start, capture.end),
      );
    } else {
      Expect.isNull(match[i]);
    }
  }
}

typedef StringRange = ({int start, int end});

StringRange translateRange(int diff, StringRange range) {
  return (start: range.start + diff, end: range.end + diff);
}
