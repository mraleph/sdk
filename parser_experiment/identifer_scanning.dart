// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

final $a = 'a'.codeUnitAt(0);
final $z = 'z'.codeUnitAt(0);
final $A = 'A'.codeUnitAt(0);
final $Z = 'Z'.codeUnitAt(0);
final $0 = '0'.codeUnitAt(0);
final $9 = '9'.codeUnitAt(0);
final $_ = '_'.codeUnitAt(0);
final $$ = r'$'.codeUnitAt(0);

bool isIdentifierSlow(int i) {
  return ($a <= i && i <= $z) ||
      ($A <= i && i <= $Z) ||
      ($0 <= i && i <= $9) ||
      i == $_ ||
      i == $$;
}

void createTable() {
  print(jsonEncode(List.generate(256, (i) => isIdentifierSlow(i) ? 0x21 : 0x20)
      .map(String.fromCharCode)
      .join()));
}

@pragma('vm:prefer-inline')
bool isIdentifierCharacter(int ch) {
  const String table =
      "                                    !           !!!!!!!!!!       !!!!!!!!!!!!!!!!!!!!!!!!!!    ! !!!!!!!!!!!!!!!!!!!!!!!!!!                                                                                                                                     ";
  return table.codeUnitAt(ch) & 1 != 0;
}

@pragma('vm:unsafe:no-bounds-checks')
@pragma('vm:unsafe:no-interrupts')
@pragma('vm:prefer-inline')
int skipNonIdentifierUnrolled(Uint8List bytes, int start) {
  final bytesLengthMinusOne = bytes.length - 1;
  while ((start + 10) < bytesLengthMinusOne) {
    // Here we can access bytes without checks
    int next = bytes[++start];
    if (!isIdentifierCharacter(next) &&
        !isIdentifierCharacter(next = bytes[++start]) &&
        !isIdentifierCharacter(next = bytes[++start]) &&
        !isIdentifierCharacter(next = bytes[++start]) &&
        !isIdentifierCharacter(next = bytes[++start]) &&
        !isIdentifierCharacter(next = bytes[++start]) &&
        !isIdentifierCharacter(next = bytes[++start]) &&
        !isIdentifierCharacter(next = bytes[++start]) &&
        !isIdentifierCharacter(next = bytes[++start]) &&
        !isIdentifierCharacter(next = bytes[++start])) {
      continue;
    }
    // If we got here the latest value into next returned false.
    return start;
  }

  while (start < bytesLengthMinusOne) {
    int next = bytes[++start];
    if (isIdentifierCharacter(next)) {
      return start;
    }
  }

  return start;
}

@pragma('vm:unsafe:no-bounds-checks')
@pragma('vm:unsafe:no-interrupts')
@pragma('vm:prefer-inline')
int skipIdentifierUnrolled(Uint8List bytes, int start) {
  final bytesLengthMinusOne = bytes.length - 1;
  while ((start + 10) < bytesLengthMinusOne) {
    // Here we can access bytes without checks
    int next = bytes[++start];
    if (isIdentifierCharacter(next) &&
        isIdentifierCharacter(next = bytes[++start]) &&
        isIdentifierCharacter(next = bytes[++start]) &&
        isIdentifierCharacter(next = bytes[++start]) &&
        isIdentifierCharacter(next = bytes[++start]) &&
        isIdentifierCharacter(next = bytes[++start]) &&
        isIdentifierCharacter(next = bytes[++start]) &&
        isIdentifierCharacter(next = bytes[++start]) &&
        isIdentifierCharacter(next = bytes[++start]) &&
        isIdentifierCharacter(next = bytes[++start])) {
      continue;
    }
    // If we got here the latest value into next returned false.
    return start;
  }

  while (start < bytesLengthMinusOne) {
    int next = bytes[++start];
    if (!isIdentifierCharacter(next)) {
      return start;
    }
  }

  return start;
}

@pragma('vm:unsafe:no-bounds-checks')
@pragma('vm:unsafe:no-interrupts')
@pragma('vm:align-loops')
int justScanUsingLoop(Uint8List bytes) {
  var pos = 0;
  var totalLen = 0;
  final length = bytes.length;
  outer:
  do {
    //while (pos < length && !isIdentifierCharacter(bytes[pos])) pos++;

    do {
      if (isIdentifierCharacter(bytes[pos])) break;
      pos++;
      if (pos >= length) {
        break outer;
      }
    } while (true);

    int start = pos;
    do {
      if (!isIdentifierCharacter(bytes[pos])) break;
      pos++;
    } while (pos < length);
    totalLen += pos - start;
  } while (pos < length);
  return totalLen;
}

@pragma('vm:unsafe:no-bounds-checks')
@pragma('vm:unsafe:no-interrupts')
//@pragma('vm:align-loops')
int justScanUsingLoopUnrolled(Uint8List bytes) {
  var pos = -1;
  var totalLen = 0;
  final lengthMinusOne = bytes.length - 1;
  while (pos < lengthMinusOne) {
    pos = skipNonIdentifierUnrolled(bytes, pos);
    if (pos >= lengthMinusOne) {
      break;
    }

    int start = pos;
    pos = skipIdentifierUnrolled(bytes, pos);
    totalLen += pos - start;
  }

  if (pos == lengthMinusOne && isIdentifierCharacter(bytes[pos])) {
    totalLen++;
  }

  return totalLen;
}

class FileData {
  final Uint8List bytes;

  FileData(Uint8List data) : bytes = Uint8List.fromList(data);
}

void main() {
  // createTable();
  final fileNames = File('input.list').readAsLinesSync();
  final inputData = [
    for (var path in fileNames)
      FileData(File('../../sdk' + path.substring(2)).readAsBytesSync())
  ];

  final sw = Stopwatch()..start();
  for (var i = 0; i < 10; i++)
    for (var data in inputData) {
      justScanUsingLoop(data.bytes);
    }
  sw.stop();
  print(sw.elapsedMicroseconds);
}
