// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// Analyzes a directory with `ParallelAnalysis`, and reports the time.
///
/// Compile to AOT and run, e.g.:
///
/// ```
/// pkg/vm/tool/precompiler2 --packages=sdk \
///   pkg/analyzer/tool/benchmark/parallel_analysis.dart /tmp/pa.aot
/// out/ReleaseX64/dartaotruntime /tmp/pa.aot \
///   --root pkg/analyzer --cache /tmp/pa-cache --cold --workers 8
/// ```
library;

import 'dart:io';

import 'package:analyzer/error/error.dart';
import 'package:analyzer/src/dart/analysis/analysis_context_collection.dart';
import 'package:analyzer/src/dart/analysis/driver.dart';
import 'package:analyzer/src/dart/analysis/parallel/byte_store.dart';
import 'package:analyzer/src/dart/analysis/parallel/parallel_analysis.dart';
import 'package:analyzer/src/dart/analysis/performance_logger.dart';
import 'package:args/args.dart';
import 'package:linter/src/rules.dart' as linter;
import 'package:path/path.dart' as p;

Future<void> main(List<String> args) async {
  var parser = ArgParser()
    ..addOption('root', defaultsTo: 'pkg/analyzer')
    ..addOption('cache', defaultsTo: '/tmp/parallel_analysis_cache')
    ..addFlag('cold', help: 'Delete the cache before analysis.')
    ..addOption('workers', defaultsTo: '1')
    ..addOption('dump', help: 'Write sorted diagnostics into this file.')
    ..addFlag('no-retain-asts', help: 'Parse files again for linking.')
    ..addFlag('performance', help: 'Print the performance tree.')
    ..addFlag('workers-statistics', help: 'Print statistics of workers.');
  var options = parser.parse(args);

  var root = p.normalize(p.absolute(options.option('root')!));
  var cachePath = p.normalize(p.absolute(options.option('cache')!));
  var workerCount = int.parse(options.option('workers')!);

  if (options.flag('cold')) {
    var cacheDir = Directory(cachePath);
    if (cacheDir.existsSync()) {
      cacheDir.deleteSync(recursive: true);
    }
  }

  var totalTimer = Stopwatch()..start();
  linter.registerLintRules();

  var collection = AnalysisContextCollectionImpl(
    includedPaths: [root],
    byteStore: SyncFileByteStore(cachePath),
    scheduler: _BypassedScheduler(PerformanceLog(null)),
  );

  var allDiagnostics = <FileDiagnostics>[];
  for (var context in collection.contexts) {
    var paths = context.contextRoot
        .analyzedFiles()
        .where((path) => path.endsWith('.dart'))
        .toList();
    var analysis = ParallelAnalysis(
      context.driver,
      paths,
      isolateSetup: linter.registerLintRules,
      retainParsedUnits: !options.flag('no-retain-asts'),
    );
    var timer = Stopwatch()..start();
    var diagnostics = await analysis.run(workerCount: workerCount);
    print(
      '[context: ${context.contextRoot.root.path}] '
      '[files: ${paths.length}] [time: ${timer.elapsedMilliseconds} ms]',
    );
    print('  phases: ${analysis.phaseTimes}');
    if (options.flag('workers-statistics')) {
      for (var statistics in analysis.workerStatistics) {
        print('  $statistics');
      }
    }
    if (options.flag('performance')) {
      var buffer = StringBuffer();
      analysis.performance.write(buffer: buffer);
      print(buffer);
    }
    allDiagnostics.addAll(diagnostics);
  }

  var lines = <String>[];
  for (var fileDiagnostics in allDiagnostics) {
    for (var diagnostic in fileDiagnostics.diagnostics) {
      if (diagnostic.diagnosticCode.type == DiagnosticType.TODO) continue;
      lines.add(
        '${fileDiagnostics.path}:${diagnostic.offset}:${diagnostic.length} '
        '${diagnostic.diagnosticCode.lowerCaseName} ${diagnostic.message}',
      );
    }
  }
  lines.sort();
  if (options.option('dump') case var dumpPath?) {
    File(dumpPath).writeAsStringSync(lines.map((l) => '$l\n').join());
  }

  print(
    '[workers: $workerCount] [diagnostics: ${lines.length}] '
    '[total: ${totalTimer.elapsedMilliseconds} ms]',
  );
  exit(0);
}

class _BypassedScheduler extends AnalysisDriverScheduler {
  new(super.logger);

  @override
  void start() {
    throw StateError('This scheduler should not be started.');
  }
}
