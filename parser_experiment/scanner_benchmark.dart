// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:_fe_analyzer_shared/src/scanner/scanner.dart';
import 'package:_fe_analyzer_shared/src/scanner/token.dart';

class FileData {
  final Uint8List bytes;

  FileData(Uint8List data) : bytes = Uint8List.fromList(data);
}

final bannedFiles = {};

final fileNames = File('parser_experiment/input.list')
    .readAsLinesSync()
    .map((v) => v.substring(3))
    .toList(growable: false);
final inputData = [
  for (var path in fileNames) FileData(File(path).readAsBytesSync())
];

Token scanWithoutRecovery(Uint8List bytes,
    {ScannerConfiguration? configuration,
    bool includeComments = false,
    LanguageVersionChanged? languageVersionChanged,
    bool allowLazyStrings = true}) {
  Scanner scanner = new Utf8BytesScanner(bytes,
      configuration: configuration,
      includeComments: includeComments,
      languageVersionChanged: languageVersionChanged,
      allowLazyStrings: allowLazyStrings);
  return scanner.tokenize();
}

String pct(int i, int n) => '${(i * 100 / n).toStringAsFixed(2)} %';

void main(List<String> args) {
  if (args case ["validate"]) {
    var totalBytes = 0;
    for (var (idx, data) in inputData.indexed.take(3600)) {
      final path = fileNames[idx];
      totalBytes += data.bytes.length;
      print('[${pct(idx, 3600)}] [$totalBytes] validating $path');
      final cppResult =
          Process.runSync('out/ReleaseX64/scanner_benchmark', ['parse', path]);
      if (cppResult.exitCode != 0) {
        print(
            "FAILED: out/ReleaseX64/scanner_benchmark parse ${fileNames[idx]}");
        print(cppResult.stdout);
        print(cppResult.stderr);
        exit(1);
      }
      final cppTokens = cppResult.stdout.split('\n');
      if (cppTokens.last == '') {
        cppTokens.length--;
      }
      print('parsed with cpp (${cppTokens.length} produced)');

      final result = scanWithoutRecovery(data.bytes,
          configuration: ScannerConfiguration(enableTripleShift: true));
      final dartTokens = <String>[];
      for (Token? curr = result; curr != null; curr = curr.next) {
        dartTokens.add(formatToken(curr));
        if (curr.isEof) {
          break;
        }
      }

      if (!compareTokens(dartTokens, cppTokens)) {
        print("FAILED: ${path}");
        break;
      }
    }
  }

  inputData;
  final runForever = args.contains('forever');
  do {
    final sw = Stopwatch()..start();
    int totalBytes = 0;
    for (var (idx, data) in inputData.indexed.take(3600)) {
      totalBytes += data.bytes.length;
      scanWithoutRecovery(data.bytes,
          configuration: ScannerConfiguration(enableTripleShift: true));
    }
    sw.stop();
    print(
        'Dart took ${sw.elapsedMicroseconds} us to scan ${totalBytes} bytes: ${(sw.elapsedMicroseconds * 1000.0 / totalBytes).toStringAsFixed(2)} ns/byte');
  } while (runForever);
}

bool compareTokens(List<String> dartTokens, List<String> cppTokens) {
  if (dartTokens.length != cppTokens.length) {
    print('different length: ${dartTokens.length} != ${cppTokens.length}');
  }

  for (var i = 0; i < min(dartTokens.length, cppTokens.length); i++) {
    if (dartTokens[i] != cppTokens[i]) {
      print('mismatch at $i: (dart) ${dartTokens[i]} != (cpp) ${cppTokens[i]}');
      return false;
    }
  }

  if (dartTokens.length < cppTokens.length) {
    for (var i = dartTokens.length; i < cppTokens.length; i++) {
      print('mismatch at $i: (cpp) ${cppTokens[i]}');
    }
  }

  return true;
}

String formatToken(Token curr) {
  final sb = StringBuffer();
  sb.write('Token {');
  sb.write('offset: ${curr.charOffset}, type: ${tokenType(curr)}');
  if (curr.type == TokenType.IDENTIFIER) {
    sb.write(', content: ${curr.value()}');
  }
  sb.write('}');
  return sb.toString();
}

String tokenType(Token curr) {
  return curr.type.toString();
}
