// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// VMOptions=--async-debugger --verbose-debug

import 'dart:developer';
import 'service_test_common.dart';
import 'test_helper.dart';

// LINE* values can be updated by running: test_helper.dart update-lines $path.
const int LINE_A = 22;
const int LINE_B = 23;
const int LINE_C = 24;
const int LINE_G = 29;
const int LINE_D = 36;
const int LINE_E = 37;
const int LINE_F = 38;
const int LINE_H = 39;

Future<int> helper() async {
  await null; // LINE_A.
  print('[helper] after await 1'); // LINE_B.
  print('[helper] after await 2 at ${StackTrace.current}'); // LINE_C.
  return 42;
}

testMain() async {
  int handleValue(int v) /* LINE_G */ {
    print('handleValue($v) at ${StackTrace.current}');
    return v + 4200;
  }

  print('[testMain] before break');
  debugger();
  print('[testMain] after break, before await'); // LINE_D.
  final v = await helper().then(handleValue); // LINE_E
  final u = handleValue(v); // LINE_F.
  print('[testMain] v=$v u=$u'); // LINE_H.
}

var tests = <IsolateTest>[
  hasStoppedAtBreakpoint,
  stoppedAtLine(LINE_D),
  stepOver, // print.

  hasStoppedAtBreakpoint,
  stoppedAtLine(LINE_E),
  stepInto,

  hasStoppedAtBreakpoint,
  stoppedAtLine(LINE_A),
  asyncNext,

  hasStoppedAtBreakpoint,
  stoppedAtLine(LINE_B),
  stepOver, // print.

  hasStoppedAtBreakpoint,
  stoppedAtLine(LINE_C),
  stepOut, // out of helper to awaiter testMain.

  hasStoppedAtBreakpoint,
  stoppedAtLine(LINE_G),
  stepOut, // out of helper to awaiter testMain.

  hasStoppedAtBreakpoint,
  stoppedAtLine(LINE_F),
  stepOver,

  hasStoppedAtBreakpoint,
  stoppedAtLine(LINE_H),
];

main(args) => runIsolateTests(args, tests,
    testeeConcurrent: testMain, extraArgs: extraDebuggingArgs);
