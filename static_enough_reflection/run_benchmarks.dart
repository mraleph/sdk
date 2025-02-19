// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';
import 'gen_stress_test.dart' as gen;

int measureProcess(String cmd, List<String> args) {
  final sw = Stopwatch()..start();
  final result = Process.runSync(cmd, args);
  sw.stop();
  if (result.exitCode != 0) {
    print('failed to compile $cmd ${args.join(' ')}');
    print(result.stdout);
    print(result.stderr);
    exit(1);
  }
  return sw.elapsedMilliseconds;
}

// --no-include-inlining-info-in-disassembly --disassemble-optimized --code-comments --print-flow-graph-optimized --print-flow-graph-filter=toJson /tmp/json.aot.dill

void main() {
  final numRuns = 10;
  final dartBuildMode = 'ProductX64';

  for (var mode in ['jit', 'aot'])
    for (var useReflection in [true, false]) {
      for (var n = 2; n <= 1024; n *= 2) {
        final variant = useReflection ? 'kmirror' : 'manual';
        final benchmarkSource = '/tmp/x_${variant}_$n.dart';
        final benchmarkDill = '$benchmarkSource.$mode.dill';
        final benchmarkSnapshot = '$benchmarkSource.elf';
        gen.generateTo(n, benchmarkSource, useReflection: useReflection);
        for (var i = 0; i < numRuns; i++) {
          final result = measureProcess('out/$dartBuildMode/gen_kernel.exe', [
            '--platform',
            'out/$dartBuildMode/vm_platform_strong.dill',
            if (mode == 'aot') '--aot',
            '-o',
            benchmarkDill,
            benchmarkSource,
          ]);
          print('$n,gen_kernel,$mode,$variant,$result');
        }
        if (mode == 'aot') {
          for (var i = 0; i < numRuns; i++) {
            final result = measureProcess('out/$dartBuildMode/gen_snapshot', [
              '--snapshot-kind=app-aot-elf',
              '--elf=$benchmarkSnapshot',
              benchmarkDill,
            ]);
            print('$n,gen_snapshot,$mode,$variant,$result');
          }
          print(
            '$n,size,$mode,$variant,${File(benchmarkSnapshot).lengthSync()}',
          );
        }
      }
    }
}
