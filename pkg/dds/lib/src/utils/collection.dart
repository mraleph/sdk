// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

extension Pairs<K, V> on Map<K, V> {
  /// Returns an iterable of key-value pairs for each entry in this map.
  Iterable<(K, V)> get pairs => entries.map((e) => (e.key, e.value));
}

extension Zip2<T1, T2> on (Iterable<T1>, Iterable<T2>) {
  /// Zips the pair of iterables into an iterable of pairs.
  ///
  /// Throws [StateError] if given iterables have different lengths.
  Iterable<(T1, T2)> get zipped sync* {
    final it1 = this.$1.iterator;
    final it2 = this.$2.iterator;
    while (true) {
      final it1HasNext = it1.moveNext();
      final it2HasNext = it2.moveNext();
      if (it1HasNext != it2HasNext) {
        throw StateError('Iterables have different lengths');
      }
      if (!it1HasNext) {
        break;
      }

      yield (it1.current, it2.current);
    }
  }
}
