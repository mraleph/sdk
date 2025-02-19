// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';
import 'dart:math';

sealed class JsonType {
  String get spelling;

  String createInitializer(Random random);
}

extension on Random {
  String nextString(int length) {
    return String.fromCharCodes(List.generate(length, (_) => nextInt(26) + 97));
  }
}

final class PrimitiveType extends JsonType {
  @override
  final String spelling;

  PrimitiveType({required this.spelling});

  @override
  String createInitializer(Random random) => switch (spelling) {
    'int' => '${random.nextInt(1024)}',
    'double' => '${random.nextDouble()}',
    'bool' => '${random.nextBool()}',
    'String' => '"${random.nextString(10 + random.nextInt(10))}"',
    _ => throw UnimplementedError(),
  };
}

final class ListType extends JsonType {
  final JsonType elementType;

  ListType({required this.elementType});

  @override
  String get spelling => 'List<${elementType.spelling}>';

  @override
  String createInitializer(Random random) {
    return '[' +
        List.generate(
          5 + random.nextInt(5),
          (_) => elementType.createInitializer(random),
        ).join(', ') +
        ']';
  }
}

final class ClassType extends JsonType {
  final int id;
  final List<JsonType> fields;

  ClassType({required this.id, required this.fields});

  String get spelling => 'C$id';

  @override
  String createInitializer(Random random) {
    final out = StringBuffer();
    out.writeln('C$id(');
    for (var (idx, fieldType) in fields.indexed) {
      final fieldName = 'f$idx';
      out.writeln('  $fieldName: ${fieldType.createInitializer(random)},');
    }
    out.writeln(')');
    return out.toString();
  }
}

final class NullableType extends JsonType {
  final JsonType type;

  NullableType(this.type);

  String get spelling => '${type.spelling}?';

  @override
  String createInitializer(Random random) {
    if (random.nextInt(10) < 3) {
      return 'null';
    }
    return type.createInitializer(random);
  }
}

final primitiveTypes = [
  PrimitiveType(spelling: 'int'),
  PrimitiveType(spelling: 'double'),
  PrimitiveType(spelling: 'bool'),
  PrimitiveType(spelling: 'String'),
];

enum Variant { metaprogramming, manual, none }

class Generator {
  final classTypes = <ClassType>[];
  final random = Random(42);

  Generator(int numClasses) {
    for (var i = 0; i < numClasses; i++) {
      generateClass();
    }
  }

  JsonType generateRandomType(
    int classId, {
    bool allowList = true,
    bool allowNullable = true,
  }) {
    final allowClass = classId > 0;
    if (allowNullable && random.nextInt(10) < 3) {
      return NullableType(
        generateRandomType(classId, allowList: allowList, allowNullable: false),
      );
    }
    final fieldTypeId = random.nextInt(
      primitiveTypes.length + (allowClass ? 2 : 0) + (allowList ? 2 : 0),
    );
    if (fieldTypeId < primitiveTypes.length) {
      return primitiveTypes[fieldTypeId];
    } else if (allowClass && fieldTypeId < primitiveTypes.length + 2) {
      final classIndex = classId == 1 ? 0 : random.nextInt(classId);
      return classTypes[classIndex];
    } else {
      return ListType(
        elementType: generateRandomType(classId, allowList: false),
      );
    }
  }

  void generateClass() {
    final id = classTypes.length;

    final cls = ClassType(
      id: id,
      fields: List<JsonType>.generate(
        5 + random.nextInt(10),
        (_) => generateRandomType(id),
      ),
    );
    classTypes.add(cls);
  }

  String serialize(String value, JsonType type) {
    return switch (type) {
      PrimitiveType() ||
      NullableType(type: PrimitiveType()) ||
      ListType(elementType: PrimitiveType()) ||
      ListType(elementType: NullableType(type: PrimitiveType())) => value,
      ListType(elementType: final elementType) =>
        '[for (var el in $value) ${serialize('el', elementType)}]',
      NullableType(type: ClassType()) => '$value?.toJson()',
      NullableType(type: final nonNullableType) => switch (serialize(
        value,
        nonNullableType,
      )) {
        final serializer when serializer == value => value,
        final serializer => '$value == null ? null : $serializer',
      },
      ClassType() => '$value.toJson()',
      _ => throw UnimplementedError(),
    };
  }

  String deserialize(String value, JsonType type) {
    return switch (type) {
      PrimitiveType(:final spelling) => '$value as $spelling',
      NullableType(type: PrimitiveType(:final spelling)) =>
        '$value as $spelling?',
      ListType(elementType: PrimitiveType(:final spelling)) =>
        '($value as List<Object?>).cast<$spelling>()',
      ListType(
        elementType: NullableType(type: PrimitiveType(:final spelling)),
      ) =>
        '($value as List<Object?>).cast<$spelling?>()',
      ListType(elementType: final elementType) =>
        '[for (var el in ($value as List<Object?>)) ${deserialize('el', elementType)}]',
      NullableType(type: final nonNullable) =>
        '$value == null ? null : ${deserialize(value, nonNullable)}',
      ClassType(:final id) => 'C$id.fromJson($value as Map<String, Object?>)',
    };
  }

  void writeClass(
    StringBuffer out,
    ClassType cls, {
    required Variant toJson,
    required Variant fromJson,
  }) {
    out.writeln('class C${cls.id} {');
    for (var (idx, field) in cls.fields.indexed) {
      out.writeln('  final ${field.spelling} f$idx;');
    }
    out.writeln('  C${cls.id}({');
    for (var (idx, _) in cls.fields.indexed) {
      out.writeln('    required this.f$idx,');
    }
    out.writeln('  });');
    switch (toJson) {
      case Variant.none:
        break;
      case Variant.manual:
        out.writeln('  Map<String, Object?> toJson() => {');
        for (var (idx, _) in cls.fields.indexed) {
          final fieldName = 'f$idx';
          // ${serialize(fieldName, fieldType)}
          out.writeln('    "$fieldName": $fieldName,');
        }
        out.writeln('  };');
      case Variant.metaprogramming:
        out.writeln(
          '  Map<String, Object?> toJson() => to_json.toJsonImpl(this);',
        );
    }
    switch (fromJson) {
      case Variant.none:
        break;
      case Variant.metaprogramming:
        out.writeln(
          'factory C${cls.id}.fromJson(Map<String, Object?> m) => to_json.fromJson(m);',
        );
      case Variant.manual:
        out.writeln('  C${cls.id}.fromJson(Map<String, Object?> m) : ');
        for (var (idx, fieldType) in cls.fields.indexed) {
          final fieldName = 'f$idx';
          final value = 'm["$fieldName"]';
          out.writeln(
            '  ${idx > 0 ? ',' : ''}$fieldName = ${deserialize(value, fieldType)}',
          );
        }
        out.writeln('  ;');
    }
    out.writeln('}');
  }

  String generateSource({required bool useReflection}) {
    final out = StringBuffer();

    if (useReflection) {
      out.writeln(
        'import "/usr/local/google/home/vegorov/src/dart/sdk/static_enough_reflection/to_json2.dart" as to_json;',
      );
    }

    for (var cls in classTypes) {
      writeClass(
        out,
        cls,
        toJson: useReflection ? Variant.metaprogramming : Variant.manual,
        fromJson: useReflection ? Variant.metaprogramming : Variant.manual,
      );
    }

    for (var cls in classTypes) {
      // useReflection ? 'toJson(v)' : 'v.toJson()';
      final toJson = 'v.toJson()';

      out.writeln("""
  @pragma('vm:entry-point')
  Object? serialize${cls.id}(C${cls.id} v) => $toJson;

  @pragma('vm:entry-point')
  C${cls.id} deserialize${cls.id}(Map<String, Object?> v) => C${cls.id}.fromJson(v);
  """);
    }

    out.writeln('void main() {');
    // for (var cls in classTypes) {
    //  out.writeln('final v${cls.id} = ${cls.createInitializer()};');
    // }
    out.writeln('}');

    return out.toString();
  }
}

void generateTo(
  int numClasses,
  String outputPath, {
  bool useReflection = false,
}) {
  final generator = Generator(numClasses);
  /*
  out.writeln('void main() {');
  for (var cls in classTypes) {
    out.writeln('final v${cls.id} = ${cls.createInitializer()};');
  }
  out.writeln('}');
  */
  File(
    outputPath,
  ).writeAsStringSync(generator.generateSource(useReflection: useReflection));
  final fmtResult = Process.runSync('dart', ['format', outputPath]);
  if (fmtResult.exitCode != 0) {
    print('ERROR: failed to format ${outputPath}');
    print(fmtResult.stdout);
    print(fmtResult.stderr);
    exit(1);
  }
  //  print('generated ${outputPath}');
}

/*
void main(List<String> args) {
  if (args.length != 2) {
    print('Usage: gen_stress_test.dart <num> output.dart');
    exit(1);
  }

  final [numClassesStr, outputPath] = args;

  final numClasses = int.parse(numClassesStr);
  for (var i = 0; i < numClasses; i++) {
    generateClass();
  }

}*/
