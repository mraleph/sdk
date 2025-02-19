// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:metaprogramming';

Map<String, Object?> toJsonImpl<@konst T>(T value) {
  final typeInfo = TypeInfo.of<T>();
  return {
    for (@konst final field in typeInfo.fields)
      if (!field.isStatic) field.name: field.getFrom(value),
  };
}

List<E> listFromJson<@konst E>(List<Object?> list) {
  return <E>[for (var v in list) valueFromJson<E>(v)];
}

FieldType valueFromJson<@konst FieldType>(Object? value) {
  var fieldType = TypeInfo.of<FieldType>();
  if (fieldType.isNullable) {
    if (value == null) {
      return null as FieldType;
    }
    fieldType = fieldType.nonNullable;
  } else {
    if (value == null) {
      throw ArgumentError('Field not found in incoming json');
    }
  }

  // Primitive values are mapped directly.
  if (fieldType.isSubtypeOf<String>() ||
      fieldType.isSubtypeOf<num>() ||
      fieldType.isSubtypeOf<bool>()) {
    return value as FieldType;
  }

  // Lists are unpacked element by element.
  if (fieldType.instantiationOf<List>() case final instantiation?) {
    final elementType = instantiation.typeArguments.first.type;
    return invoke(listFromJson, [value as List<Object?>], types: [elementType]);
  } else {
    // We assume that this is Map -> class conversion then.
    return fromJson<FieldType>(value as Map<String, Object?>);
  }
}

T fromJson<@konst T>(Map<String, Object?> json) {
  final typeInfo = TypeInfo.of<T>();
  return invoke(
    typeInfo.defaultConstructor,
    [],
    named: {
      for (@konst final field in typeInfo.fields)
        if (!field.isStatic)
          field.name: invoke(
            valueFromJson,
            [json[field.name]],
            types: [field.type.type],
          ),
    },
  );
}

class HashHelpers {
  static final _seed = identityHashCode(Object());

  static int combine(int hash, int value) {
    hash = 0x1fffffff & (hash + value);
    hash = 0x1fffffff & (hash + ((0x0007ffff & hash) << 10));
    return hash ^ (hash >> 6);
  }

  static int finish(int hash) {
    hash = 0x1fffffff & (hash + ((0x03ffffff & hash) << 3));
    hash = hash ^ (hash >> 11);
    return 0x1fffffff & (hash + ((0x00003fff & hash) << 15));
  }
}

mixin DataClass<@konst T> {
  @override
  operator ==(Object? other) {
    if (other is! T) {
      return false;
    }

    final typeInfo = TypeInfo.of<T>();
    for (@konst final field in typeInfo.fields) {
      final value1 = field.getFrom(this as T);
      final value2 = field.getFrom(other);
      if (field.type.isSubtypeOf<List>()) {
        if ((value1 as List).length != (value2 as List).length) {
          return false;
        }
        for (var i = 0; i < value1.length; i++) {
          if (value1[i] != value2[i]) {
            return false;
          }
        }
      } else if (value1 != value2) {
        return false;
      }
    }
    return true;
  }

  @override
  int get hashCode {
    final typeInfo = TypeInfo.of<T>();
    var hash = HashHelpers._seed;
    for (@konst final field in typeInfo.fields) {
      hash = HashHelpers.combine(hash, field.getFrom(this as T).hashCode);
    }
    return HashHelpers.finish(hash);
  }

  Map<String, Object?> toJson() => toJsonImpl<T>(this as T);
}
