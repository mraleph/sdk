// Copyright (c) 2020, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Helper script for extracting CFG and generated code dumps for specific
// functions from the full code dumps which might be too large to view in
// the editor comfortably.

import 'dart:io';
import 'dart:convert';

enum State {
  nothing,
  phase,
  functionName,
  dumping,
  dumpingCode,
}

void main(List<String> args) async {
  final pattern = RegExp(args[1]);

  final input = args[0];
  final file = File(input);
  int cnt = 0;

  var state = State.nothing;
  String phase;

  await for (var line in file
      .openRead()
      .transform(utf8.decoder)
      .transform(const LineSplitter())) {
    switch (state) {
      case State.nothing:
        if (line == '*** BEGIN CFG') {
          state = State.phase;
        } else if (line.startsWith('Code for') && line.contains(pattern)) {
          print(line);
          state = State.dumpingCode;
        }
        break;
      case State.phase:
        state = State.functionName;
        phase = line;
        break;
      case State.functionName:
        if (line.contains(pattern)) {
          print('*** BEGIN CFG');
          print(phase);
          print(line);
          state = State.dumping;
        } else {
          state = State.nothing;
        }
        break;
      case State.dumping:
        print(line);
        if (line == '*** END CFG') {
          state = State.nothing;
        }
        break;
      case State.dumpingCode:
        print(line);
        if (line == '}') {
          state = State.nothing;
        }
    }
    cnt++;
  }
}
