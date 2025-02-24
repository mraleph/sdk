// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "package:expect/expect.dart";

void main() {
  checkMatch(
    r'(?:(?<=[^\\])|^){{(\w*)}}',
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

void checkMatch(
  String re,
  String input, {
  List<({int start, int end})?> expectedCaptures = const [],
  Map<String, ({int start, int end})> expectedNamedCaptures = const {},
}) {
  final match = RegExp(re).firstMatch(input)!;
  print(match.captures);
  print(match.namedCaptures);
  Expect.equals(expectedCaptures.length, match.captures.length);
  for (var (i, expected) in expectedCaptures.indexed) {
    Expect.equals(expected, match.captures[i]);
  }
  for (final MapEntry(key: name, value: expected)
      in expectedNamedCaptures.entries) {
    Expect.equals(expected, match.namedCaptures[name]);
  }
  Expect.equals(expectedNamedCaptures.length, match.namedCaptures.length);
}
