import 'dart:io';

import 'common.dart';

void main(List<String> arguments) {
  final inputSets = loadInputSets();

  for (final (i, files) in inputSets.indexed) {
    Stopwatch sw = new Stopwatch()..start();
    var totalBytes = 0;
    for (String file in files) {
      totalBytes += File(file).readAsBytesSync().length;
    }
    sw.stop();
    print(
        '[$i],${sw.elapsedMilliseconds},$totalBytes,${(sw.elapsedMicroseconds * 1024 / totalBytes).toStringAsFixed(2)} us/kb');
  }
}
