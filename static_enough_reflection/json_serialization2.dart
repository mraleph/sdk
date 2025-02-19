// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';
import 'to_json2.dart';

class A with DataClass<A> {
  final String f0;
  final int f1;
  final double f2;
  final List<B> nested;

  const A({
    required this.f0,
    required this.f1,
    required this.f2,
    required this.nested,
  });
}

class B with DataClass<B> {
  final String f0;
  final int? f1;
  final double f2;

  const B({required this.f0, required this.f1, required this.f2});
}

void main() {
  final obj = A(
    f0: 'f0 value',
    f1: 42,
    f2: 3.14,
    nested: [B(f0: 'B.f0', f1: 24, f2: 1.1)],
  );
  final json = const JsonEncoder.withIndent('  ').convert(obj);
  print(json);
  final m = jsonDecode(json);
  final obj2 = fromJson<A>(m);
  print(const JsonEncoder.withIndent('  ').convert(obj2));
  print(obj == obj2);
}
