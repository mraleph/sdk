// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library dart.metaprogramming;

const konst = pragma('konst');

final class TypeInfo<T> {
  const TypeInfo._();

  external static TypeInfo<T> of<@konst T>();

  @konst
  external Function defaultConstructor;

  @konst
  external TypeInfo<T> get nonNullable;

  @konst
  external bool isNullable;

  @konst
  external Type type;

  @konst
  external bool isSubtypeOf<@konst T>();

  @konst
  external TypeInfo<Base>? instantiationOf<@konst Base>();

  @konst
  external List<TypeInfo> get typeArguments;

  @konst
  external List<FieldInfo<T, Object?>> get fields;
}

final class FieldInfo<HostType, FieldType> {
  const FieldInfo._();

  @konst
  external String get name;

  @konst
  external bool get isStatic;

  @konst
  external TypeInfo<FieldType> get type;

  @konst
  external FieldType getFrom(HostType value);
}

external T invoke<T>(
  @konst Function f,
  List<Object?> args, {
  List<Type> types = const [],
  Map<String, Object?> named = const {},
});
