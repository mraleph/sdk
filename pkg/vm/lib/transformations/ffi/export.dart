// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';

import 'package:front_end/src/api_unstable/vm.dart'
    show
        messageFfiNativeMustBeExternal,
        messageFfiNativeOnlyNativeFieldWrapperClassCanBePointer,
        templateCantHaveNamedParameters,
        templateCantHaveOptionalParameters,
        templateFfiNativeUnexpectedNumberOfParameters,
        templateFfiNativeUnexpectedNumberOfParametersWithReceiver;

import 'package:kernel/ast.dart';
import 'package:kernel/core_types.dart';
import 'package:kernel/class_hierarchy.dart' show ClassHierarchy;
import 'package:kernel/library_index.dart' show LibraryIndex;
import 'package:kernel/reference_from_index.dart' show ReferenceFromIndex;
import 'package:kernel/target/targets.dart' show DiagnosticReporter;
import 'package:kernel/text/ast_to_text.dart';
import 'package:kernel/type_environment.dart';
import 'package:vm/transformations/type_flow/utils.dart';

import 'common.dart' show FfiStaticTypeError, FfiTransformer;

/// Transform @Export annotated functions into FFI native function pointer
/// functions.
void transformLibraries(
    Component component,
    CoreTypes coreTypes,
    ClassHierarchy hierarchy,
    List<Library> libraries,
    DiagnosticReporter diagnosticReporter,
    ReferenceFromIndex? referenceFromIndex) {
  final index = LibraryIndex(component, [
    'dart:core',
    'dart:ffi',
    'dart:_internal',
    'dart:typed_data',
    'dart:nativewrappers',
    'dart:isolate',
    'package:ffi/src/utf8.dart',
    'package:ffi/src/allocation.dart',
  ]);
  // Skip if dart:ffi isn't loaded (e.g. during incremental compile).
  if (index.tryGetClass('dart:ffi', 'Export') == null) {
    return;
  }

  final transformer = FfiExportTransformer(
      index, coreTypes, hierarchy, diagnosticReporter, referenceFromIndex);
  libraries.forEach(transformer.visitLibrary);

  if (transformer.exported.isEmpty) {
    return;
  }

  final utf8Class = index.tryGetClass('package:ffi/src/utf8.dart', 'Utf8')!;
  final toDartStringMethod = index.getProcedure(
      'package:ffi/src/utf8.dart', 'Utf8Pointer', 'toDartString');
  final toNativeUtf8Method = index.getProcedure(
      'package:ffi/src/utf8.dart', 'StringUtf8Pointer', 'toNativeUtf8');
  final allocateExports =
      index.getTopLevelProcedure('dart:ffi', '_allocateExports');
  final callocField =
      index.getTopLevelField('package:ffi/src/allocation.dart', 'malloc');
  final ffiInt64Class = index.getClass('dart:ffi', 'Int64');
  final ffiDoubleClass = index.getClass('dart:ffi', 'Double');
  final pragmaClass = index.getClass('dart:core', 'pragma');
  final pragmaNameField = index.getField('dart:core', 'pragma', 'name');
  final pragmaOptionsField = index.getField('dart:core', 'pragma', 'options');

  bool requiresMarshalling(DartType type) {
    if (type is! InterfaceType) {
      return true;
    }
    final cls = type.classNode;
    return cls != transformer.intClass &&
        cls != transformer.doubleClass &&
        cls != transformer.pointerClass;
  }

  DartType incommingType(DartType type) {
    if (type is InterfaceType) {
      final cls = type.classNode;
      if (cls == transformer.intClass ||
          cls == transformer.doubleClass ||
          cls == transformer.pointerClass) {
        return type;
      } else if (cls == coreTypes.stringClass) {
        return InterfaceType(transformer.pointerClass, Nullability.nonNullable,
            [InterfaceType(utf8Class, Nullability.nonNullable)]);
      }
    }
    throw ArgumentError('Expected int, double, Pointer or String, got $type');
  }

  DartType outgoingType(DartType type) {
    if (type is InterfaceType) {
      final cls = type.classNode;
      if (cls == transformer.intClass ||
          cls == transformer.doubleClass ||
          cls == transformer.pointerClass) {
        return type;
      } else if (cls == coreTypes.stringClass) {
        return InterfaceType(transformer.pointerClass, Nullability.nonNullable,
            [InterfaceType(utf8Class, Nullability.nonNullable)]);
      }
    }
    throw ArgumentError('Expected int, double, Pointer or String, got $type');
  }

  Expression convertValue(Expression e, DartType from, DartType to) {
    final fromCls = (from as InterfaceType).classNode;
    final toCls = (to as InterfaceType).classNode;
    if (toCls == coreTypes.stringClass) {
      return StaticInvocation(toDartStringMethod, Arguments([e]));
    } else if (fromCls == coreTypes.stringClass) {
      return StaticInvocation(
          toNativeUtf8Method,
          Arguments([
            e
          ], named: [
            for (var named in toNativeUtf8Method.function.namedParameters)
              NamedExpression(named.name!, named.initializer!)
          ]));
    }
    throw ArgumentError('Unsupported conversion of $e $from -> $to');
  }

  Procedure generateWrapper(Procedure p) {
    final params = [
      for (var p in p.function.positionalParameters)
        VariableDeclaration(p.name, type: incommingType(p.type))
    ];
    final returnType = outgoingType(p.function.returnType);

    final statements = <Statement>[];
    final vars = <VariableDeclaration>[];

    for (var i = 0; i < params.length; i++) {
      final incomming = params[i];
      final outgoing = p.function.positionalParameters[i];
      if (incomming.type != outgoing.type) {
        final v = VariableDeclaration(
          'coverted#${incomming.name}',
          isFinal: true,
          type: outgoing.type,
          initializer: convertValue(
            VariableGet(incomming),
            incomming.type,
            outgoing.type,
          ),
        );
        statements.add(v);
        vars.add(v);
      } else {
        vars.add(incomming);
      }
    }

    Expression result =
        StaticInvocation(p, Arguments(vars.map(VariableGet.new).toList()));

    if (returnType != p.function.returnType) {
      result = convertValue(result, p.function.returnType, returnType);
    }

    statements.add(ReturnStatement(result));
    final function = FunctionNode(
      Block(statements),
      positionalParameters: params,
      returnType: returnType,
      requiredParameterCount: params.length,
    );

    return Procedure(Name('exported#${p.name.text}'), p.kind, function,
        fileUri: p.fileUri, isStatic: true)
      ..isNonNullableByDefault = true;
  }

  DartType toFfiType(DartType type) {
    final cls = (type as InterfaceType).classNode;
    if (cls == transformer.intClass) {
      return InterfaceType(ffiInt64Class, Nullability.nonNullable);
    } else if (cls == transformer.doubleClass) {
      return InterfaceType(ffiDoubleClass, Nullability.nonNullable);
    } else if (cls == transformer.pointerClass) {
      return type;
    }
    throw ArgumentError('unexpected type $type');
  }

  String toCType(DartType type, {required bool isReturn}) {
    final cls = (type as InterfaceType).classNode;
    if (cls == transformer.intClass) {
      return "int64_t";
    } else if (cls == transformer.doubleClass) {
      return "double";
    } else if (cls == transformer.pointerClass) {
      final arg = type.typeArguments.first;
      if ((arg as InterfaceType).classNode == utf8Class) {
        return isReturn ? "char*" : "const char*";
      }
      return "void*";
    }
    throw ArgumentError('unexpected type $type');
  }

  FunctionType makeFfiSignature(FunctionNode function) {
    return FunctionType(
        function.positionalParameters.map((v) => toFfiType(v.type)).toList(),
        toFfiType(function.returnType),
        Nullability.nonNullable);
  }

  final mainLibrary = (component.mainMethod?.enclosingLibrary)!;

  final exportsList = [];

  final elements = <Expression>[];

  for (var node in transformer.exported) {
    final name = node.name.text;
    if (node.function.positionalParameters
            .any((p) => requiresMarshalling(p.type)) ||
        requiresMarshalling(node.function.returnType)) {
      final library = node.enclosingLibrary;
      node = generateWrapper(node);
      library.addProcedure(node);
    }
    elements.add(StaticInvocation(
      transformer.fromFunctionMethod,
      Arguments(
        [
          ConstantExpression(
            StaticTearOffConstant(node),
            node.function.computeFunctionType(Nullability.nonNullable),
          ),
        ],
        types: <DartType>[makeFfiSignature(node.function)],
      ),
    ));
    exportsList.add({
      'n': name,
      'r': toCType(node.function.returnType, isReturn: true),
      'p': node.function.positionalParameters
          .map((v) => toCType(v.type, isReturn: false))
          .toList(),
      'pn': node.function.positionalParameters.map((v) => v.name).toList(),
    });
  }

  mainLibrary.addProcedure(Procedure(
    Name('#ffiExports'),
    ProcedureKind.Method,
    FunctionNode(
      ReturnStatement(StaticInvocation(
        allocateExports,
        Arguments(
          [
            ListLiteral(elements, typeArgument: transformer.pointerVoidType),
            ConstantExpression(
                (callocField.initializer as ConstantExpression).constant),
          ],
        ),
      )),
      returnType: coreTypes.intNonNullableRawType,
    ),
    isStatic: true,
    fileUri: mainLibrary.fileUri,
  )
    ..isNonNullableByDefault = true
    ..addAnnotation(
      ConstantExpression(
        InstanceConstant(
          pragmaClass.reference,
          [],
          {
            pragmaNameField.fieldReference: StringConstant('vm:entry-point'),
            pragmaOptionsField.fieldReference: NullConstant(),
          },
        ),
      ),
    )
    ..addAnnotation(
      ConstantExpression(
        InstanceConstant(
          pragmaClass.reference,
          [],
          {
            pragmaNameField.fieldReference:
                StringConstant('vm:ffi:exports-list'),
            pragmaOptionsField.fieldReference:
                StringConstant(jsonEncode(exportsList)),
          },
        ),
      ),
    ));
}

class FfiExportTransformer extends FfiTransformer {
  final DiagnosticReporter diagnosticReporter;
  final ReferenceFromIndex? referenceFromIndex;
  final Class ffiExportClass;

  final List<Procedure> exported = [];

  FfiExportTransformer(
      LibraryIndex index,
      CoreTypes coreTypes,
      ClassHierarchy hierarchy,
      this.diagnosticReporter,
      this.referenceFromIndex)
      : ffiExportClass = index.getClass('dart:ffi', 'Export'),
        super(index, coreTypes, hierarchy, diagnosticReporter,
            referenceFromIndex);

  ConstantExpression? tryGetAnnotation(Annotatable node, Class instanceOf) {
    for (final Expression annotation in node.annotations) {
      if (annotation is! ConstantExpression) {
        continue;
      }
      final annotationConstant = annotation.constant;
      if (annotationConstant is! InstanceConstant) {
        continue;
      }
      if (instanceOf == annotationConstant.classNode) {
        return annotation;
      }
    }
    return null;
  }

  @override
  TreeNode visitProcedure(Procedure node) {
    if (node.isStatic &&
        node.enclosingClass == null &&
        tryGetAnnotation(node, ffiExportClass) != null) {
      exported.add(node);
    }

    return super.visitProcedure(node);
  }
}
