// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// Measures the speedup of `ParallelAnalysis` with several workers over the
/// sequential analysis, cold and hot, see `parallel_analysis_README.md`.
///
/// Uses only `dart:` libraries, so can be run with any Dart SDK, e.g.:
///
/// ```
/// tools/sdks/dart-sdk/bin/dart \
///   pkg/analyzer/tool/benchmark/parallel_analysis_run.dart \
///   --root pkg/analyzer --workers 1,2,4,8 --runs 3
/// ```
library;

import 'dart:convert';
import 'dart:ffi';
import 'dart:io';

const _usage = '''
Options:
  --root <dir>         A directory to analyze, can be repeated.
                       Default: pkg/analyzer of this SDK checkout.
  --workers <list>     Comma separated worker counts. Default: 1,2,4,8.
  --runs <n>           Runs of each configuration. Default: 3.
  --modes <list>       cold, hot, or both. Default: cold,hot.
  --runtime <path>     Default: out/Release<ARCH>/dartaotruntime, or
                       xcodebuild/Release<ARCH>/dartaotruntime on macOS.
  --snapshot <path>    Default: <work>/parallel_analysis.aot, built if
                       it does not exist.
  --rebuild            Build the snapshot even if it exists.
  --work <dir>         Directory for the snapshot, caches, and outputs.
                       Default: <system temp>/parallel_analysis.
  --vm-flags <flags>   Space separated VM flags, e.g.
                       "--old_gen_growth_space_ratio=80".
  --timeout <seconds>  Timeout of a single run. Default: 900.
  --csv <path>         Default: <work>/results.csv.
''';

Future<void> main(List<String> arguments) async {
  var options = _Options.parse(arguments);
  if (options == null) {
    stderr.write(_usage);
    exit(64);
  }

  Directory(options.work).createSync(recursive: true);
  if (options.rebuild || !File(options.snapshot).existsSync()) {
    await _buildSnapshot(options);
  }
  if (!File(options.runtime).existsSync()) {
    stderr.writeln(
      'No runtime at ${options.runtime}, see parallel_analysis_README.md.',
    );
    exit(1);
  }

  var csv = File(options.csv).openSync(mode: FileMode.write);
  csv.writeStringSync('root,mode,workers,run,wall_ms,cpu_ms,rss_mb,same\n');
  var results = <_Result>[];
  for (var (rootIndex, root) in options.roots.indexed) {
    var cache = '${options.work}/cache-$rootIndex';
    var reference = '${options.work}/reference-$rootIndex.txt';
    var dump = '${options.work}/dump.txt';
    File(reference).deleteIfExists();
    for (var run = 1; run <= options.runs; run++) {
      for (var workers in options.workers) {
        for (var mode in options.modes) {
          var result = await _runOnce(
            options,
            root: root,
            cache: cache,
            dump: dump,
            mode: mode,
            workers: workers,
            run: run,
          );
          if (result.dumped) {
            var dumpFile = File(dump);
            if (!File(reference).existsSync()) {
              dumpFile.copySync(reference);
            }
            result.same = _sameBytes(dumpFile, File(reference));
          }
          results.add(result);
          csv.writeStringSync('${result.csv}\n');
          csv.flushSync();
          stdout.writeln(result.description);
        }
      }
    }
  }
  csv.closeSync();

  stdout.writeln();
  _printSummary(options, results);
  stdout.writeln('\nRaw results: ${options.csv}');
}

String _arch() {
  return switch (Abi.current()) {
    Abi.macosArm64 || Abi.linuxArm64 => 'ARM64',
    Abi.macosX64 || Abi.linuxX64 => 'X64',
    var abi => throw UnsupportedError('Unsupported platform: $abi'),
  };
}

Future<void> _buildSnapshot(_Options options) async {
  stdout.writeln('Building ${options.snapshot}');
  var result = await Process.run(
    '${options.sdk}/pkg/vm/tool/precompiler2',
    [
      '--packages=${options.sdk}/.dart_tool/package_config.json',
      '${options.sdk}/pkg/analyzer/tool/benchmark/parallel_analysis.dart',
      options.snapshot,
    ],
    environment: {'DART_CONFIGURATION': 'Release${_arch()}'},
  );
  if (result.exitCode != 0) {
    stderr.writeln(result.stdout);
    stderr.writeln(result.stderr);
    stderr.writeln(
      'Cannot build the snapshot, see parallel_analysis_README.md.',
    );
    exit(1);
  }
}

double _median(List<num> values) {
  var sorted = values.toList()..sort();
  var middle = sorted.length ~/ 2;
  return sorted.length.isOdd
      ? sorted[middle].toDouble()
      : (sorted[middle - 1] + sorted[middle]) / 2;
}

/// Returns user and system CPU time in milliseconds, written by
/// `/usr/bin/time` into the [output].
int? _parseCpuTime(String output) {
  // GNU time, with the format from [_timeArguments].
  var gnu = RegExp(r'CPU_TIME ([\d.]+) ([\d.]+)').allMatches(output);
  // BSD time on macOS, with `-l`.
  var bsd = RegExp(r'([\d.]+) user\s+([\d.]+) sys').allMatches(output);
  var match = gnu.isNotEmpty
      ? gnu.last
      : bsd.isNotEmpty
      ? bsd.last
      : null;
  if (match == null) {
    return null;
  }
  var seconds = double.parse(match.group(1)!) + double.parse(match.group(2)!);
  return (seconds * 1000).round();
}

void _printSummary(_Options options, List<_Result> results) {
  for (var root in options.roots) {
    for (var mode in options.modes) {
      var rows = results.where((r) => r.root == root && r.mode == mode);
      double? baseline;
      stdout.writeln('## $root, $mode\n');
      stdout.writeln(
        '| workers | wall (ms) | speedup | CPU (s) | peak RSS (MB) | same |',
      );
      stdout.writeln('|---:|---:|---:|---:|---:|:---:|');
      for (var workers in options.workers) {
        var runs = rows.where((r) => r.workers == workers && r.ok).toList();
        if (runs.isEmpty) {
          stdout.writeln('| $workers | failed | | | | |');
          continue;
        }
        var wall = _median([for (var r in runs) r.wallMs]);
        baseline ??= wall;
        var cpus = [for (var r in runs) ?r.cpuMs];
        var rss = [for (var r in runs) ?r.rssMb];
        var cpu = cpus.isEmpty ? '' : (_median(cpus) / 1000).toStringAsFixed(1);
        var rssText = rss.isEmpty ? '' : _median(rss).toStringAsFixed(0);
        var same = runs.every((r) => r.same == true) ? 'yes' : 'NO';
        stdout.writeln(
          '| $workers | ${wall.toStringAsFixed(0)} '
          '| ${(baseline / wall).toStringAsFixed(2)}x | $cpu | $rssText '
          '| $same |',
        );
      }
      stdout.writeln();
    }
  }
}

Future<_Result> _runOnce(
  _Options options, {
  required String root,
  required String cache,
  required String dump,
  required String mode,
  required int workers,
  required int run,
}) async {
  File(dump).deleteIfExists();
  var benchmarkArguments = [
    ...options.vmFlags,
    options.snapshot,
    '--root',
    root,
    '--cache',
    cache,
    if (mode == 'cold') '--cold',
    '--workers',
    '$workers',
    '--dump',
    dump,
  ];

  String executable;
  List<String> arguments;
  if (File('/usr/bin/time').existsSync()) {
    executable = '/usr/bin/time';
    arguments = [
      if (Platform.isMacOS) '-l' else ...['-f', 'CPU_TIME %U %S'],
      options.runtime,
      ...benchmarkArguments,
    ];
  } else {
    executable = options.runtime;
    arguments = benchmarkArguments;
  }

  var timer = Stopwatch()..start();
  var process = await Process.start(executable, arguments);
  var stdoutFuture = process.stdout.transform(utf8.decoder).join();
  var stderrFuture = process.stderr.transform(utf8.decoder).join();
  var exitCode = await process.exitCode.timeout(
    Duration(seconds: options.timeout),
    onTimeout: () {
      process.kill(ProcessSignal.sigkill);
      return -1;
    },
  );
  var wallMs = timer.elapsedMilliseconds;
  var output = await stdoutFuture;
  var errors = await stderrFuture;

  var result = _Result(
    root: root,
    mode: mode,
    workers: workers,
    run: run,
    wallMs: wallMs,
    cpuMs: _parseCpuTime(errors),
    rssMb: switch (RegExp(r'\[peak rss: (\d+) MB\]').firstMatch(output)) {
      var match? => int.parse(match.group(1)!),
      null => null,
    },
    ok: exitCode == 0,
    dumped: exitCode == 0 && File(dump).existsSync(),
  );
  if (!result.ok) {
    var log = '${options.work}/failed-$mode-$workers-$run.log';
    File(log).writeAsStringSync('$output\n$errors');
    stderr.writeln('Run failed with exit code $exitCode, see $log');
  }
  return result;
}

bool _sameBytes(File a, File b) {
  var aBytes = a.readAsBytesSync();
  var bBytes = b.readAsBytesSync();
  if (aBytes.length != bBytes.length) {
    return false;
  }
  for (var i = 0; i < aBytes.length; i++) {
    if (aBytes[i] != bBytes[i]) {
      return false;
    }
  }
  return true;
}

class _Options {
  final String sdk;
  final List<String> roots;
  final List<int> workers;
  final int runs;
  final List<String> modes;
  final String runtime;
  final String snapshot;
  final bool rebuild;
  final String work;
  final List<String> vmFlags;
  final int timeout;
  final String csv;

  new({
    required this.sdk,
    required this.roots,
    required this.workers,
    required this.runs,
    required this.modes,
    required this.runtime,
    required this.snapshot,
    required this.rebuild,
    required this.work,
    required this.vmFlags,
    required this.timeout,
    required this.csv,
  });

  static _Options? parse(List<String> arguments) {
    // This file is in `pkg/analyzer/tool/benchmark` of the SDK checkout.
    var sdk = File.fromUri(Platform.script).parent.parent.parent.parent.parent;
    var sdkPath = sdk.absolute.path;

    var values = <String, List<String>>{};
    var rebuild = false;
    for (var i = 0; i < arguments.length; i++) {
      var argument = arguments[i];
      if (argument == '--rebuild') {
        rebuild = true;
      } else if (argument.startsWith('--') && i + 1 < arguments.length) {
        (values[argument.substring(2)] ??= []).add(arguments[++i]);
      } else {
        stderr.writeln('Unexpected argument: $argument');
        return null;
      }
    }

    String? single(String name) => values[name]?.last;
    String absolute(String path) => File(path).absolute.path;

    var work = absolute(
      single('work') ?? '${Directory.systemTemp.path}/parallel_analysis',
    );
    var modes = (single('modes') ?? 'cold,hot').split(',');
    if (modes.any((mode) => mode != 'cold' && mode != 'hot')) {
      stderr.writeln('Unknown mode in $modes');
      return null;
    }
    return _Options(
      sdk: sdkPath,
      roots: [
        for (var root in values['root'] ?? ['$sdkPath/pkg/analyzer'])
          Directory(root).absolute.path,
      ],
      workers: [
        for (var count in (single('workers') ?? '1,2,4,8').split(','))
          int.parse(count),
      ],
      runs: int.parse(single('runs') ?? '3'),
      modes: modes,
      runtime: absolute(
        single('runtime') ??
            '$sdkPath/${Platform.isMacOS ? 'xcodebuild' : 'out'}/'
                'Release${_arch()}/dartaotruntime',
      ),
      snapshot: absolute(single('snapshot') ?? '$work/parallel_analysis.aot'),
      rebuild: rebuild,
      work: work,
      vmFlags: [
        for (var flag in (single('vm-flags') ?? '').split(' '))
          if (flag.isNotEmpty) flag,
      ],
      timeout: int.parse(single('timeout') ?? '900'),
      csv: absolute(single('csv') ?? '$work/results.csv'),
    );
  }
}

class _Result {
  final String root;
  final String mode;
  final int workers;
  final int run;
  final int wallMs;
  final int? cpuMs;
  final int? rssMb;
  final bool ok;
  final bool dumped;
  bool? same;

  new({
    required this.root,
    required this.mode,
    required this.workers,
    required this.run,
    required this.wallMs,
    required this.cpuMs,
    required this.rssMb,
    required this.ok,
    required this.dumped,
  });

  String get csv {
    return '$root,$mode,$workers,$run,$wallMs,${cpuMs ?? ''},'
        '${rssMb ?? ''},${same ?? ''}';
  }

  String get description {
    var cpu = cpuMs == null ? '' : ', CPU ${cpuMs! ~/ 1000} s';
    var rss = rssMb == null ? '' : ', RSS $rssMb MB';
    var status = !ok
        ? 'FAILED'
        : same == true
        ? 'same'
        : 'DIFFERENT';
    return '[$mode] [workers: $workers] [run: $run] $wallMs ms$cpu$rss, '
        '$status';
  }
}

extension on File {
  void deleteIfExists() {
    if (existsSync()) {
      deleteSync();
    }
  }
}
