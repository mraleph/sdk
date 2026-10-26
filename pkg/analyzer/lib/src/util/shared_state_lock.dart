// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// A recursive lock that protects lazily initialized state, such as the
/// element model read from summaries, when it is shared by multiple isolates
/// of the same isolate group, see `ParallelAnalysis`.
abstract class SharedStateLock {
  /// The lock used by the current isolate, `null` if the state is not shared.
  ///
  /// Static fields are per isolate, so each isolate sets its own instance.
  static SharedStateLock? current;

  void acquire();

  void release();
}

/// Runs [operation] while holding [SharedStateLock.current], if any.
@pragma('vm:prefer-inline')
T withSharedStateLock<T>(T Function() operation) {
  var lock = SharedStateLock.current;
  if (lock == null) {
    return operation();
  }
  lock.acquire();
  try {
    return operation();
  } finally {
    lock.release();
  }
}
