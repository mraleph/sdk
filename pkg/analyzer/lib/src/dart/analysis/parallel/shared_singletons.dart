// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:analyzer/src/dart/element/element.dart';
import 'package:analyzer/src/dart/element/type.dart';

/// Values of static fields that are compared by identity, or otherwise must
/// be the same for all isolates that share the element model.
///
/// Static fields are per isolate, so a new isolate must [adopt] the values
/// of the isolate that created the element model, before it accesses any of
/// these static fields.
class SharedSingletons {
  final List<Object> _elementStatics;
  final List<Object> _types;

  /// Captures the values of the current isolate.
  new capture()
    : _elementStatics = captureElementStatics(),
      _types = List.unmodifiable(<Object>[
        DynamicTypeImpl.instance,
        InvalidTypeImpl.instance,
        NeverTypeImpl.instance,
        NeverTypeImpl.instanceNullable,
        VoidTypeImpl.instance,
      ]);

  /// Sets the captured values into the static fields of the current isolate.
  void adopt() {
    adoptElementStatics(_elementStatics);
    var i = 0;
    T next<T>() => _types[i++] as T;
    DynamicTypeImpl.instance = next();
    InvalidTypeImpl.instance = next();
    NeverTypeImpl.instance = next();
    NeverTypeImpl.instanceNullable = next();
    VoidTypeImpl.instance = next();
    assert(i == _types.length);
  }
}
