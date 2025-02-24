// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:expect/expect.dart';

void main() {
  final pattern = RegExp('(?:(?<=[^\\\\])|^){{(?<name>\\w*)}}');
  final match = pattern.firstMatch('A captured word I {{capture}}')!;
  Expect.equals((start: 18, end: 29), match.captures[0]);
  Expect.equals((start: 20, end: 27), match.captures[1]);
  Expect.equals((start: 20, end: 27), match.namedCaptures['name']);
}