// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:metaprogramming';

Object? toJson<@konst T>(T value) {
  var typeMirror = TypeInfo.of<T>();

  if (typeMirror.isNullable) {
    if (value == null) {
      return null;
    }

    typeMirror = typeMirror.nonNullable;
  }

  if (typeMirror.isSubtypeOf<String>() ||
      typeMirror.isSubtypeOf<num>() ||
      typeMirror.isSubtypeOf<bool>() ||
      typeMirror.isSubtypeOf<Null>()) {
    return value;
  }

  if (typeMirror.instantiationOf<List>() case final instantiation?) {
    final elementTypeMirror = instantiation.typeArguments.first.type;
    return [
      for (var v in (value as List))
        invoke(toJson, [v], types: [elementTypeMirror]),
    ];
  }

  return {
    for (@konst final field in typeMirror.fields)
      if (!field.isStatic)
        field.name: invoke(
          toJson,
          [field.getFrom(value)],
          types: [field.staticType],
        ),
  };
}
