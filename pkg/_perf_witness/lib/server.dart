// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io' as io;
import 'dart:developer' as developer;

import 'src/common.dart';

class PerfWitnessServer {
  static Future<void> start() async {
    if (pidFilePath == null) {
      return;
    }

    final info = await developer.Service.controlWebServer(
      enable: true,
      silenceOutput: true,
    );

    // TODO(vegorov) we would like to delete this when process is exiting, but
    // currently we don't have good enough tools for that in Dart.
    io.File(
      pidFilePath!,
    ).writeAsStringSync(info.serverWebSocketUri!.toString());
  }

  static void shutdown() {
    if (pidFilePath case final path?) {
      try {
        io.File(path).deleteSync();
      } catch (_) {
        // Ignore.
      }
    }
  }
}
