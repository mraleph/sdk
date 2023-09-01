import 'dart:async';
import 'dart:io';
import 'dart:convert';

import 'package:path/path.dart' as p;

import 'package:vm_service/vm_service.dart';
import 'package:vm_service/vm_service_io.dart';
import 'package:vm_service/src/vm_service.dart' show extensionCallHelper;

final sdkRoot = p.dirname(Platform.script.toFilePath());

Stream<String> startCompiler() async* {
  final process = await Process.start(Platform.executable, [
    p.join(sdkRoot, 'pkg', 'frontend_server/bin/frontend_server_starter.dart'),
    '--sdk-root',
    'bazel-bin/swiftui_app/App_archive-root/Payload/App.app',
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
      print('current deps:\n  ${deps.join('\n  ')}');
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
  process.stdin.writeln('compile root:///hello.dart');
  await process.stdin.flush();

  Directory('./dart')
      .watch(events: FileSystemEvent.modify, recursive: true)
      .listen((event) {
    final uri = File(event.path).absolute.uri.toString();
    // print(uri);
    if (deps.contains(uri) && !compilationPending) {
      compilationPending = true;
      process.stdin.writeln('recompile changed-files');
      process.stdin.writeln('root:///hello.dart');
      process.stdin.writeln('changed-files');
    }
  });

  // final initialDillPath = await initialDill.future;
  // print('got initial dill');

  // await process.stdin.flush();

  yield* dillFiles.stream;
}

void main() async {
  final serviceClient =
      await vmServiceConnectUri('ws://[::]:8787/ws', log: StdoutLog());
  try {
    VM vm = await serviceClient.getVM();
    print('hostCPU=${vm.hostCPU}');
    print(await serviceClient.getVersion());
    List<IsolateRef> isolates = vm.isolates!;
    print(isolates);

    final createResult = await extensionCallHelper(
        serviceClient, '_createDevFS', {'fsName': 'reload'});
    final uri = createResult.json['uri'];

    await for (var dillPath in startCompiler()) {
      print('got $dillPath');
      await extensionCallHelper(serviceClient, '_writeDevFSFile', {
        'fsName': 'reload',
        'path': 'main.dill',
        'fileContents': base64.encode(File(dillPath).readAsBytesSync())
      });
      final dillPathTarget = Uri.parse(uri).resolve('main.dill');
      final result = await serviceClient.reloadSources(isolates.first.id!,
          rootLibUri: dillPathTarget.toString());
      print(result);
    }
  } finally {
    try {
      final deleteResult =
          await extensionCallHelper(serviceClient, '_deleteDevFS', {
        'fsName': 'reload',
      });
      print(deleteResult);
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
