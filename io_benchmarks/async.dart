import 'dart:async';
import 'dart:io';

import 'common.dart';

void main(List<String> arguments) async {
  final inputSets = loadInputSets();

  for (final (i, files) in inputSets.indexed) {
    final sw = new Stopwatch()..start();
    var totalBytes = 0;
    await Future.wait([
      for (var file in files)
        File(file).readAsBytes().then((bytes) {
          totalBytes += bytes.length;
        }),
    ]);
    sw.stop();
    print(
        '[$i],${sw.elapsedMilliseconds},$totalBytes,${(sw.elapsedMicroseconds * 1024 / totalBytes).toStringAsFixed(2)} us/kb');
  }
}
