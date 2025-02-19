import 'dart:convert';
import 'dart:mirrors';

class A {
  final String f0;
  final int f1;
  final double f2;
  final List<B> nested;

  const A({
    required this.f0,
    required this.f1,
    required this.f2,
    required this.nested,
  });
}

class B {
  final String f0;
  final int? f1;
  final double f2;

  const B({
    required this.f0,
    required this.f1,
    required this.f2,
  });
}

String propertyName(Symbol name) {
  return MirrorSystem.getName(name);
}

class Redirect<T> {
  Object? convert(T value) => toJson(value);
}

Object? invokeToJson(Type valueType, Object? value) {
  return ((reflectType(Redirect, [valueType]) as ClassMirror)
          .newInstance(const Symbol(''), []).reflectee as Redirect)
      .convert(value);
}

Object? toJson<T>(T value) {
  print('here with $T');
  final typeMirror = reflectType(T);
  if (typeMirror is! ClassMirror) {
    throw 'Expected to get a class!';
  }

  if (value is String ||
      value is int ||
      value is double ||
      value is bool ||
      value == null) {
    return value;
  }

  if (value is List) {
    final elementType = typeMirror.typeArguments.first.reflectedType;
    return value.map((v) => invokeToJson(elementType, v)).toList();
  }

  final instanceMirror =
      reflect(value); // should not be able to do this in KMirrors.
  return {
    for (var decl in typeMirror.declarations.values)
      if (decl is VariableMirror && !decl.isStatic)
        propertyName(decl.simpleName): invokeToJson(
          decl.type.reflectedType,
          instanceMirror.getField(decl.simpleName).reflectee,
        ),
  };
}

void main() {
  print(const JsonEncoder.withIndent('  ').convert(toJson(A(
    f0: 'f0 value',
    f1: 42,
    f2: 3.14,
    nested: [
      B(
        f0: 'B.f0',
        f1: 24,
        f2: 1.1,
      ),
    ],
  ))));
}
