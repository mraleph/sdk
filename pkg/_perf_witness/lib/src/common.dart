// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io' as io;
import 'package:path/path.dart' as p;

final String? _dartToolDirectoryPath = () {
  final env = io.Platform.environment;
  final homeDir = io.Platform.isWindows ? env['LOCALAPPDATA'] : env['HOME'];
  if (homeDir == null) {
    return null;
  }

  final dartToolPath = p.join(homeDir, '.dart-tool');
  try {
    // Ensure that directory exists.
    io.Directory(dartToolPath).createSync();
    return dartToolPath;
  } catch (_) {
    // Ignore any sort of exceptions.
    return null;
  }
}();

const String _pidFileSuffix = '.pw';

List<({int pid, io.File pidFile, String wsUri})> getAllConnectionUrls() {
  if (_dartToolDirectoryPath == null) {
    return const [];
  }

  try {
    final allPidFiles =
        io.Directory(_dartToolDirectoryPath!).listSync().whereType<io.File>();
    return [
      for (var file in allPidFiles)
        if (file.path.endsWith(_pidFileSuffix))
          (
            pid: int.parse(p.basenameWithoutExtension(file.path)),
            pidFile: file,
            wsUri: file.readAsStringSync(),
          ),
    ];
  } catch (_) {
    // Ignore
    return [];
  }
}

final String? pidFilePath = () {
  final dirPath = _dartToolDirectoryPath;
  if (dirPath == null) {
    return null;
  }

  // TODO(vegorov) figure out if we want to support multi-isolate situation.
  return p.join(dirPath, '${io.pid}$_pidFileSuffix');
}();
