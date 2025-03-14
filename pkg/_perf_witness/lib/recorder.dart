// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';
import 'dart:io' as io;

import 'package:args/args.dart';
import 'package:path/path.dart' as p;
import 'package:vm_service/vm_service.dart';
import 'package:vm_service/vm_service_io.dart';

import 'src/common.dart';

class Connection {
  final int pid;
  final VmService vmService;
  final int initialTimeStamp;

  Connection._(this.pid, this.vmService, this.initialTimeStamp);

  Future<void> pullDataTo(String outputDir) async {
    final currentTimeStamp = (await vmService.getVMTimelineMicros()).timestamp!;
    final vm = await vmService.getVM();
    final timeline = await vmService.getPerfettoVMTimeline(
      timeOriginMicros: initialTimeStamp,
      timeExtentMicros: currentTimeStamp - initialTimeStamp,
    );

    print(
      'time extent in seconds: ${(currentTimeStamp - initialTimeStamp) / (1000 * 1000)}',
    );

    final outputPath = p.join(outputDir, '$pid.timeline');
    final output = io.File(outputPath).openWrite();

    output.add(base64.decode(timeline.trace!));
    for (var isolateRef in vm.isolates ?? const <IsolateRef>[]) {
      final cpuSamples = await vmService.getPerfettoCpuSamples(
        isolateRef.id!,
        timeOriginMicros: initialTimeStamp,
        timeExtentMicros: currentTimeStamp - initialTimeStamp,
      );
      output.add(base64.decode(cpuSamples.samples!));
    }
    await output.flush();
    await output.close();
  }

  Future<void> disconnect() async {
    // Stop profiler and timeline recording.
    await vmService.setVMTimelineFlags([]);
    await vmService.setFlag('profiler', 'false');
    await vmService.dispose();
  }

  static Future<Connection> connectTo(int pid, String wsUri) async {
    final vmService = await vmServiceConnectUri(wsUri);
    print(await vmService.setFlag('timeline_recorder', 'endless'));
    await vmService.setVMTimelineFlags(["all"]);
    print(await vmService.setFlag('profile_period', '250'));
    print(await vmService.setFlag('profiler', 'true'));
    final timeStamp = await vmService.getVMTimelineMicros();
    return Connection._(pid, vmService, timeStamp.timestamp!);
  }
}

Future<Connection?> tryConnect(int pid, io.File pidFile, String wsUri) async {
  try {
    return await Connection.connectTo(pid, wsUri);
  } catch (_) {
    try {
      pidFile.deleteSync(); // Likely stale file. Purge it.
    } catch (_) {}
    return null;
  }
}

final argParser = ArgParser()..addOption('output-dir', abbr: 'o');

void main(List<String> args) async {
  final parsedArgs = argParser.parse(args);

  final activeProcesses = getAllConnectionUrls();
  final connections = (await Future.wait([
    for (var p in activeProcesses) tryConnect(p.pid, p.pidFile, p.wsUri),
  ])).nonNulls.toList(growable: false);

  print('Connected to ${connections.length} processes.');
  final sw = Stopwatch()..start();
  await io.ProcessSignal.sigint.watch().first;
  print('Pulling data for ${sw.elapsed}');
  final io.Directory outputDir;
  if (parsedArgs['output-dir'] case final String outputDirPath) {
    outputDir = io.Directory(outputDirPath);
  } else {
    outputDir = io.Directory.systemTemp.createTempSync('recording');
  }
  print('... data will be written to $outputDir');
  await Future.wait([
    for (var conn in connections) conn.pullDataTo(outputDir.path),
  ]);
  print('done');
  await Future.wait([for (var conn in connections) conn.disconnect()]);
}
