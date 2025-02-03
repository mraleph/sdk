import 'dart:io';
import 'dart:math' as math;

Iterable<List<T>> splitIntoSublists<T>(List<T> l, int len) sync* {
  for (var start = 0; start < l.length; start += len) {
    yield l.sublist(start, math.min(start + len, l.length));
  }
}

List<List<String>> loadInputSets() {
  final lines = File('input.list').readAsLinesSync();
  return splitIntoSublists(lines, 1000).toList();
}
