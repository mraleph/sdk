// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';
import 'dart:io' as io;

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
    final vm = await vmService.getVM();
    final timeline = await vmService.getPerfettoVMTimeline(
      timeOriginMicros: initialTimeStamp,
    );

    final outputPath = p.join(outputDir, '$pid.timeline');
    final output = io.File(outputPath).openWrite();

    output.add(base64.decode(timeline.trace!));
    for (var isolateRef in vm.isolates ?? []) {
      final cpuSamples = await vmService.getPerfettoCpuSamples(
        isolateRef.id!,
        timeOriginMicros: initialTimeStamp,
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
    final timeStamp = await vmService.getVMTimelineMicros();
    await vmService.setVMTimelineFlags(["all"]);
    await vmService.setFlag('profile-period', '250');
    await vmService.setFlag('profiler', 'true');
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

void main() async {
  final activeProcesses = getAllConnectionUrls();
  final connections = (await Future.wait([
    for (var p in activeProcesses) tryConnect(p.pid, p.pidFile, p.wsUri),
  ])).nonNulls.toList(growable: false);

  print('Connected to ${connections.length} processes.');
  await io.ProcessSignal.sigint.watch().first;
  print('Pulling data');
  final outputDir = io.Directory.systemTemp.createTempSync('recording');
  print('... data will be written to $outputDir');
  await Future.wait([
    for (var conn in connections) conn.pullDataTo(outputDir.path),
  ]);
  print('done');
  await Future.wait([for (var conn in connections) conn.disconnect()]);
}
