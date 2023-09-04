import 'dart:async';
import 'dart:io';
import 'dart:convert';

import 'package:path/path.dart' as p;

import 'package:vm_service/vm_service.dart';
import 'package:vm_service/vm_service_io.dart';
import 'package:vm_service/src/vm_service.dart' show extensionCallHelper;

final sdkRoot = p.dirname(Platform.script.toFilePath());

Stream<String> startCompiler() async* {
  final process = await Process.start(p.join(sdkRoot, 'frontend_server'), [
    //p.join(sdkRoot, 'pkg', 'frontend_server/bin/frontend_server_starter.dart'),
    '--sdk-root',
    'bazel-out/darwin_arm64-opt/bin/external/dart-sdk/',
    '--platform',
    'platform_product.dill',
    '--source',
    'package:ffi/ffi.dart',
    '--filesystem-root',
    'dart',
    '--filesystem-scheme',
    'root',
    '--output-dill',
    p.join(Directory.systemTemp.path, 'output.dill'),
    '--output-incremental-dill',
    p.join(Directory.systemTemp.path, 'output.incremental.dill'),
    '--packages',
    p.join(sdkRoot, '.dart_tool/package_config.json'),
    '--incremental',
  ]);

  String? boundaryKey;
  Set<String> deps = {};

  bool compilationPending = false;
  bool firstCompilation = true;
  final sw = Stopwatch()..start();

  final dillFiles = StreamController<String>();

  process.stdout
      .transform(utf8.decoder)
      .transform(const LineSplitter())
      .listen((line) {
    if (line == boundaryKey) {
      return;
    }

    if (boundaryKey != null && line.startsWith(boundaryKey!)) {
      final results = line.split(' ');
      compilationPending = false;
      if (!firstCompilation) {
        print("recompiled in ${sw.elapsedMilliseconds} ms");
      }
      firstCompilation = false;
      dillFiles.add(results[1]);
      boundaryKey = null;
    } else if (line.startsWith('+')) {
      deps.add(line.substring(1));
    } else if (line.startsWith('-')) {
      deps.remove(line.substring(1));
    } else if (line.startsWith('result ')) {
      boundaryKey = line.split(' ')[1];
    } else {
      print('stdout> $line');
    }
  });
  process.stderr
      .transform(utf8.decoder)
      .transform(const LineSplitter())
      .listen((line) {
    print('stderr> $line');
  });

  compilationPending = true;
  sw.reset();
  process.stdin.writeln('compile root:///hello.dart');
  await process.stdin.flush();

  Directory('./dart')
      .watch(events: FileSystemEvent.modify, recursive: true)
      .listen((event) {
    final uri = File(event.path).absolute.uri.toString();
    if (deps.contains(uri) && !compilationPending) {
      compilationPending = true;
      sw.reset();
      process.stdin.writeln('recompile changed-files');
      process.stdin.writeln('root:///' + p.basename(event.path));
      process.stdin.writeln('changed-files');
    }
  });

  yield* dillFiles.stream;
}

void main() async {
  final serviceClient =
      await vmServiceConnectUri('ws://localhost:8787/ws', log: StdoutLog());
  try {
    VM vm = await serviceClient.getVM();
    print("Connected to the VM");
    List<IsolateRef> isolates = vm.isolates!;

    final listResult =
        await extensionCallHelper(serviceClient, '_listDevFS', {});

    if (listResult.json['fsNames'].contains('reload')) {
      await extensionCallHelper(serviceClient, '_deleteDevFS', {
        'fsName': 'reload',
      });
    }

    final createResult = await extensionCallHelper(
        serviceClient, '_createDevFS', {'fsName': 'reload'});
    final uri = createResult.json['uri'];

    var firstReload = true;
    await for (var dillPath in startCompiler()) {
      final sw = Stopwatch()..start();
      await extensionCallHelper(serviceClient, '_writeDevFSFile', {
        'fsName': 'reload',
        'path': 'main.dill',
        'fileContents': base64.encode(File(dillPath).readAsBytesSync())
      });
      final dillPathTarget = Uri.parse(uri).resolve('main.dill');
      final result = await serviceClient.reloadSources(isolates.first.id!,
          rootLibUri: dillPathTarget.toString());
      if (firstReload) {
        print("Battlecruiser operational 🚀");
        firstReload = false;
      } else {
        print('reloaded in ${sw.elapsedMilliseconds}');
      }
    }
  } finally {
    try {
      final deleteResult =
          await extensionCallHelper(serviceClient, '_deleteDevFS', {
        'fsName': 'reload',
      });
    } catch (_) {}

    await serviceClient.dispose();
  }
}

class StdoutLog extends Log {
  @override
  void warning(String message) => print(message);

  @override
  void severe(String message) => print(message);
}
