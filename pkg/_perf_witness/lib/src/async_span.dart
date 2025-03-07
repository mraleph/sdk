// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:async';
import 'dart:developer';

// With synchronous execution the nesting between spans is naturally induced
// by the callstack. Consider:
//
// ```dart
// Timeline.timeSync('a', () {
//   work();
//   Timeline.timeSync('b', () {
//     work();
//   });
//   work();
//   Timeline.timeSync('c', () {
//     work();
//   });
//   work();
// })
// ```
//
// This will created three spans `a`, `b` and `c` all properly nested. The time
// outside of `b` and `c` will be correctly attributed to `a`.
//
// However the same is not easy to achieve for async computations. Compare:
//
// void a() async {
//   work();
//   await b();
//   work();
//   await c();
//   work();
// }
//
// There is no functionality available in `dart:developer` which would allow
// to create proper span structure to automatically accurately capture the
// work done in `a`, `b` and `c`. The best you can do is to manually wrap
// synchronous parts of work into `timeSync`.
//
// This class tries to help with this by creating a `Zone` which automatically
// does this - but result is not good enough: completion of async task causes
// resumption of async task that awaits on the current task which creates
// inversely nested spans (e.g. if `b` is suspended and completes
// asynchronously you get span `a` nested inside span `b` - even though you
// would like an opposite picture or worst case you want these spans to be
// siblings).
class AsyncSpan {
  final String name;
  final Flow _flow = Flow.begin();
  bool issuedBegin = false;
  int running = 0;

  AsyncSpan(this.name);

  static AsyncSpan of(Zone zone) => zone[AsyncSpan]!;

  static final _zoneSpecification = ZoneSpecification(
    run: <R>(self, parent, zone, R Function() f) {
      final span = AsyncSpan.of(self);

      span.startSync();
      try {
        return parent.run(zone, f);
      } finally {
        span.finishSync();
      }
    },
    runUnary: <R, T1>(self, parent, zone, R Function(T1) f, T1 a1) {
      final span = AsyncSpan.of(self);
      span.startSync();
      try {
        return parent.runUnary(zone, f, a1);
      } finally {
        span.finishSync();
      }
    },
    runBinary: <R, T1, T2>(
      self,
      parent,
      zone,
      R Function(T1, T2) f,
      T1 a1,
      T2 a2,
    ) {
      final span = AsyncSpan.of(self);
      span.startSync();
      try {
        return parent.runBinary(zone, f, a1, a2);
      } finally {
        span.finishSync();
      }
    },
  );

  static Zone create(String name) {
    return Zone.current.fork(
      specification: _zoneSpecification,
      zoneValues: {AsyncSpan: AsyncSpan(name)},
    );
  }

  void startSync() {
    if (running == 0) {
      Timeline.startSync(name, flow: issuedBegin ? Flow.step(_flow.id) : _flow);
      issuedBegin = true;
    }
    running++;
  }

  void finishSync() {
    if (--running == 0) {
      Timeline.finishSync();
    }
  }
}
