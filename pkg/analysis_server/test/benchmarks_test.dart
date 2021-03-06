// Copyright (c) 2014, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.=> defineTests();

/// This tests the benchmarks in benchmark/benchmark.test, and ensures that our
/// benchmarks can run.
import 'dart:convert';
import 'dart:io';

import 'package:path/path.dart' as path;
import 'package:test/test.dart';

void main() => defineTests();

String get _serverSourcePath {
  String script = Platform.script.toFilePath(windows: Platform.isWindows);
  String pkgPath = path.normalize(path.join(path.dirname(script), '..', '..'));
  return path.join(pkgPath, 'analysis_server');
}

void defineTests() {
  group('benchmarks', () {
    final List<String> benchmarks = _listBenchmarks();

    test('can list', () {
      expect(benchmarks, isNotEmpty);
    });

    for (String benchmarkId in benchmarks) {
      if (benchmarkId == 'analysis-flutter-analyze') {
        continue;
      }

      test(benchmarkId, () {
        ProcessResult r = Process.runSync(
          Platform.resolvedExecutable,
          [
            path.join('benchmark', 'benchmarks.dart'),
            'run',
            '--repeat=1',
            '--quick',
            benchmarkId
          ],
          workingDirectory: _serverSourcePath,
        );
        expect(r.exitCode, 0,
            reason: 'exit: ${r.exitCode}\n${r.stdout}\n${r.stderr}');
      });

      // TODO(scheglov): Restore similar test coverage when the front-end API
      // allows it.  See https://github.com/dart-lang/sdk/issues/33520.
      // test('$benchmarkId-use-cfe', () {
      //   ProcessResult r = Process.runSync(
      //     Platform.resolvedExecutable,
      //     [
      //       path.join('benchmark', 'benchmarks.dart'),
      //       'run',
      //       '--repeat=1',
      //       '--quick',
      //       '--use-cfe',
      //       benchmarkId
      //     ],
      //     workingDirectory: _serverSourcePath,
      //   );
      //   expect(r.exitCode, 0,
      //       reason: 'exit: ${r.exitCode}\n${r.stdout}\n${r.stderr}');
      // });
    }
  });
}

List<String> _listBenchmarks() {
  ProcessResult result = Process.runSync(
    Platform.resolvedExecutable,
    [path.join('benchmark', 'benchmarks.dart'), 'list', '--machine'],
    workingDirectory: _serverSourcePath,
  );
  Map m = json.decode(result.stdout);
  List benchmarks = m['benchmarks'];
  return benchmarks.map((b) => b['id']).cast<String>().toList();
}
