// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// Analyzes a directory with `ParallelAnalysis`, and reports the time and
/// the peak RSS of the process.
///
/// Compile to AOT and run, e.g.:
///
/// ```
/// pkg/vm/tool/precompiler2 --packages=$PWD/.dart_tool/package_config.json \
///   pkg/analyzer/tool/benchmark/parallel_analysis.dart /tmp/pa.aot
/// out/ReleaseX64/dartaotruntime /tmp/pa.aot \
///   --root pkg/analyzer --cache /tmp/pa-cache --cold --workers 8
/// ```
library;

import 'dart:developer';
import 'dart:io';

import 'package:analyzer/diagnostic/diagnostic.dart';
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
    ..addFlag('workers-statistics', help: 'Print statistics of workers.')
    ..addOption(
      'timeline',
      help: 'Write the timeline of tasks and GC in the Chrome format.',
    );
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

  var timelinePath = options.option('timeline');
  if (timelinePath != null) {
    NativeRuntime.streamTimelineTo(
      TimelineRecorder.chrome,
      path: timelinePath,
      streams: [TimelineStream.dart, TimelineStream.gc],
    );
  }

  var totalTimer = Stopwatch()..start();
  var lines = await _run(options, root, cachePath, workerCount);
  if (timelinePath != null) {
    NativeRuntime.stopStreamingTimeline();
  }

  lines.sort();
  if (options.option('dump') case var dumpPath?) {
    File(dumpPath).writeAsStringSync(lines.map((l) => '$l\n').join());
  }

  print(
    '[workers: $workerCount] [diagnostics: ${lines.length}] '
    '[total: ${totalTimer.elapsedMilliseconds} ms] '
    '[peak rss: ${_peakRssMb()} MB]',
  );
  exit(0);
}

AnalysisContextCollectionImpl _createCollection(String root, String cache) {
  return AnalysisContextCollectionImpl(
    includedPaths: [root],
    byteStore: SyncFileByteStore(cache),
    scheduler: _BypassedScheduler(PerformanceLog(null)),
  );
}

String _format(String path, Diagnostic diagnostic) {
  return '$path:${diagnostic.offset}:${diagnostic.length} '
      '${diagnostic.diagnosticCode.lowerCaseName} ${diagnostic.message}';
}

int _peakRssMb() {
  var status = File('/proc/self/status').readAsStringSync();
  var match = RegExp(r'VmHWM:\s+(\d+) kB').firstMatch(status);
  return match == null ? -1 : int.parse(match.group(1)!) ~/ 1024;
}

Future<List<String>> _run(
  ArgResults options,
  String root,
  String cachePath,
  int workerCount,
) async {
  linter.registerLintRules();
  var collection = _createCollection(root, cachePath);

  var lines = <String>[];
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
      traceTasks: options.option('timeline') != null,
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
    for (var fileDiagnostics in diagnostics) {
      for (var diagnostic in fileDiagnostics.diagnostics) {
        if (diagnostic.diagnosticCode.type == DiagnosticType.TODO) continue;
        lines.add(_format(fileDiagnostics.path, diagnostic));
      }
    }
  }
  return lines;
}

class _BypassedScheduler extends AnalysisDriverScheduler {
  new(super.logger);

  @override
  void start() {
    throw StateError('This scheduler should not be started.');
  }
}
