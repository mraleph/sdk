// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:collection/collection.dart';
import 'package:front_end/src/api_prototype/constant_evaluator.dart'
    show ConstantEvaluator;
import 'package:front_end/src/codes/cfe_codes.dart'
    show messageKonstExpressionDoesNotEvaluateToACompileTimeConstant;
import 'package:kernel/ast.dart';
import 'package:kernel/binary/ast_from_binary.dart';
import 'package:kernel/class_hierarchy.dart';
import 'package:kernel/clone.dart';
import 'package:kernel/core_types.dart';
import 'package:kernel/library_index.dart';
import 'package:kernel/reference_from_index.dart';
import 'package:kernel/target/targets.dart' show DiagnosticReporter, Target;
import 'package:kernel/text/ast_to_text.dart';
import 'package:kernel/type_algebra.dart';
import 'package:kernel/type_environment.dart';
import 'package:vm/transformations/pragma.dart';
import 'package:vm/transformations/vm_constant_evaluator.dart';

void transformLibraries({
  required Target targetInfo,
  required DiagnosticReporter diagnosticReporter,
  required Component component,
  required CoreTypes coreTypes,
  required ClassHierarchy hierarchy,
  required List<Library> libraries,
  required ReferenceFromIndex? referenceFromIndex,
  //required ConstantEvaluator constantEvaluator,
}) {
  try {
    final compTime = _Konst(
      targetInfo: targetInfo,
      diagnosticReporter: diagnosticReporter,
      component: component,
      coreTypes: coreTypes,
      hierarchy: hierarchy,
      constantEvaluator:
          VMConstantEvaluator.create(targetInfo, component, null),
    );
    libraries.forEach(compTime.visitLibrary);
    compTime.emitPending();
  } on _AbortTransformation catch (_) {
  } on UnimplementedError catch (_) {
    print(debugComponentToString(component));
    rethrow;
  }
}

class KonstSignature {
  final bool hasReceiver;
  final int typeParameters;
  final int positionalParameters;
  final List<String> namedParameters;

  const KonstSignature({
    this.hasReceiver = false,
    this.typeParameters = 0,
    this.positionalParameters = 0,
    this.namedParameters = const [],
  });

  @override
  String toString() {
    return 'KonstSignature($hasReceiver, $typeParameters, $positionalParameters, $namedParameters)';
  }

  bool get isEmpty => identical(this, const KonstSignature());

  bool hasTypeParameter(int i) => typeParameters & (1 << i) != 0;
  bool hasPositionalParameter(int i) => positionalParameters & (1 << i) != 0;
  bool hasNamedParameter(String name) => namedParameters.contains(name);
}

final class KonstArguments {
  late final hashCode = _computeHash();

  final Constant? receiver;
  final Map<VariableDeclaration, Constant> arguments;
  final Map<TypeParameter, DartType> typeArguments;

  KonstArguments(
    this.receiver,
    this.arguments,
    this.typeArguments,
  );

  int _computeHash() {
    return Object.hashAll([
      receiver,
      ...arguments.values,
      ...typeArguments.values,
    ]);
  }

  @override
  String toString() {
    return 'KonstArguments(receiver: $receiver, arguments: $arguments, typeArguments: $typeArguments)';
  }

  @override
  bool operator ==(Object other) {
    if (other is! KonstArguments) {
      return false;
    }

    if (this.receiver != other.receiver) {
      return false;
    }

    for (var key in typeArguments.keys) {
      if (typeArguments[key] != other.typeArguments[key]) {
        return false;
      }
    }

    for (var key in arguments.keys) {
      if (arguments[key] != other.arguments[key]) {
        return false;
      }
    }

    return true;
  }
}

class InvocationArguments {
  final Expression? receiver;
  final Arguments arguments;
  InvocationArguments(this.receiver, this.arguments);

  @override
  String toString() {
    return 'InvocationArguments(receiver: $receiver, arguments: $arguments)';
  }
}

class _Konst extends Transformer {
  final Target targetInfo;
  final DiagnosticReporter diagnosticReporter;
  final Component component;
  final CoreTypes coreTypes;
  final ClassHierarchy hierarchy;
  final LibraryIndex index;
  final ConstantPragmaAnnotationParser pragmaParser;

  final ConstantEvaluator constantEvaluator;
  final TypeEnvironment typeEnvironment;

  late final classTypeInfo = index.getClass('dart:metaprogramming', 'TypeInfo');

  late final classFieldInfo =
      index.getClass('dart:metaprogramming', 'FieldInfo');

  static bool Function(Statement stmt) isIndexedStoreTo(
      VariableDeclaration map) {
    return (stmt) {
      if (stmt is! ExpressionStatement) {
        return false;
      }

      final expression = stmt.expression;
      if (expression is! InstanceInvocationExpression) {
        return false;
      }

      if (expression.name.text != '[]=') {
        return false;
      }

      if (expression.receiver case VariableGet(:final variable)
          when variable == map) {
        return true;
      }
      return false;
    };
  }

  Map<String, Expression>? tryReconstructMapLiteral(Expression expr) {
    if (expr case BlockExpression(:final body, :final value)) {
      if (body.statements
          case [
            VariableDeclaration(isFinal: true, initializer: MapLiteral()) &&
                final decl,
            final Block block
          ] when block.statements.every(isIndexedStoreTo(decl))) {
        if (value case VariableGet(variable: final resultVar)
            when resultVar == decl) {
          final result = <String, Expression>{};
          for (var stmt in block.statements) {
            final [key, value] = ((stmt as ExpressionStatement).expression
                    as InstanceInvocationExpression)
                .arguments
                .positional;
            result[((key as ConstantExpression).constant as StringConstant)
                .value] = value;
          }
          return result;
        }
      } else {
        return null;
      }
    }
    return null;
  }

  _Konst({
    required this.targetInfo,
    required this.diagnosticReporter,
    required this.component,
    required this.coreTypes,
    required this.hierarchy,
    required this.constantEvaluator,
  })  : index = LibraryIndex.fromLibraries(
            component.libraries, ['dart:metaprogramming']),
        pragmaParser = ConstantPragmaAnnotationParser(coreTypes, targetInfo),
        typeEnvironment = TypeEnvironment(coreTypes, hierarchy) {
    registerHandler(
        index.getProcedure('dart:metaprogramming', 'TypeInfo', 'of'),
        (member, args, iargs) {
      final targetType =
          args.typeArguments[member.function.typeParameters.first]!;
      return ConstantExpression(_createTypeInfo(targetType));
    });

    registerHandler(
        index.getProcedure(
            'dart:metaprogramming', 'TypeInfo', 'get:isNullable'),
        (member, args, iargs) {
      final kmirror = args.receiver as InstanceConstant;

      final result = switch (kmirror.typeArguments.first.nullability) {
        Nullability.nonNullable => false,
        Nullability.nullable => true,
        _ => throw UnimplementedError(),
      };

      return ConstantExpression(BoolConstant(result));
    });

    registerHandler(
        index.getProcedure(
            'dart:metaprogramming', 'TypeInfo', 'get:typeArguments'),
        (member, args, iargs) {
      final kmirror = args.receiver as InstanceConstant;
      final type = kmirror.typeArguments.first as InterfaceType;
      return ConstantExpression(ListConstant(
        InterfaceType(classTypeInfo, Nullability.nonNullable),
        [
          for (var typeArg in type.typeArguments) _createTypeInfo(typeArg),
        ],
      ));
    });

    registerHandler(
        index.getProcedure(
            'dart:metaprogramming', 'TypeInfo', 'get:defaultConstructor'),
        (member, args, iargs) {
      final kmirror = args.receiver as InstanceConstant;
      final type = kmirror.typeArguments.first as InterfaceType;

      return ConstantExpression(
          ConstructorTearOffConstant(type.classNode.constructors.first));
    });

    registerHandler(
        index.getProcedure('dart:metaprogramming', 'TypeInfo', 'isSubtypeOf'),
        (member, args, iargs) {
      final kmirror = args.receiver as InstanceConstant;
      final type = kmirror.typeArguments.first;
      final other = args.typeArguments.values.first;

      return ConstantExpression(BoolConstant(typeEnvironment.isSubtypeOf(
          type, other, SubtypeCheckMode.withNullabilities)));
    });

    registerHandler(
        index.getProcedure(
            'dart:metaprogramming', 'TypeInfo', 'instantiationOf'),
        (member, args, iargs) {
      final kmirror = args.receiver as InstanceConstant;
      final type = kmirror.typeArguments.first as InterfaceType;
      final other =
          (args.typeArguments.values.first as InterfaceType).classNode;

      final instantiation =
          hierarchy.getInterfaceTypeAsInstanceOfClass(type, other);

      if (instantiation == null) {
        return ConstantExpression(NullConstant());
      }

      return ConstantExpression(_createTypeInfo(instantiation));
    });

    registerHandler(
        index.getProcedure('dart:metaprogramming', 'TypeInfo', 'get:type'),
        (member, args, iargs) {
      final kmirror = args.receiver as InstanceConstant;
      final type = kmirror.typeArguments.first;

      return ConstantExpression(TypeLiteralConstant(type));
    });

    registerHandler(
        index.getProcedure(
            'dart:metaprogramming', 'TypeInfo', 'get:nonNullable'),
        (member, args, iargs) {
      final kmirror = args.receiver as InstanceConstant;
      final type = kmirror.typeArguments.first.toNonNull();

      return ConstantExpression(_createTypeInfo(type));
    });

    registerHandler(
        index.getProcedure('dart:metaprogramming', 'TypeInfo', 'get:fields'),
        (member, args, iargs) {
      final kmirror = args.receiver as InstanceConstant;
      // TODO: probably want to instantiate types of fields using
      // type parameters?
      final hostType = (kmirror.typeArguments.first as InterfaceType);
      final cls = hostType.classNode;

      final result = fieldsOf[hostType] ??= ListConstant(
          InterfaceType(classFieldInfo, Nullability.nonNullable, [
            hostType,
            coreTypes.objectNullableRawType,
          ]),
          <Constant>[
            for (var member in cls.members)
              if (member is Field) _createFieldInfo(member, hostType),
          ]);

      return ConstantExpression(result);
    });

    registerHandler(
        index.getProcedure('dart:metaprogramming', 'FieldInfo', 'get:isStatic'),
        (member, args, iargs) {
      final kfield = args.receiver as InstanceConstant;
      final field = fieldInv[kfield]!;

      return ConstantExpression(BoolConstant(field.isStatic));
    });

    registerHandler(
        index.getProcedure('dart:metaprogramming', 'FieldInfo', 'get:name'),
        (member, args, iargs) {
      final kfield = args.receiver as InstanceConstant;
      final field = fieldInv[kfield]!;

      return ConstantExpression(StringConstant(field.name.text));
    });

    registerHandler(
        index.getProcedure('dart:metaprogramming', 'FieldInfo', 'get:type'),
        (member, args, iargs) {
      final kfield = args.receiver as InstanceConstant;
      return ConstantExpression(_createTypeInfo(kfield.typeArguments[1]));
    });

    registerHandler(
        index.getProcedure('dart:metaprogramming', 'FieldInfo', 'getFrom'),
        (member, args, iargs) {
      final kfield = args.receiver as InstanceConstant;
      final field = fieldInv[kfield]!;
      return InstanceGet.byReference(
        InstanceAccessKind.Instance,
        iargs.arguments.positional[0],
        field.name,
        interfaceTargetReference: field.getterReference,
        resultType: field.getterType,
      );
    });

    registerHandler(
        index.getTopLevelProcedure('dart:metaprogramming', 'invoke'),
        (member, args, iargs) {
      // Decide if we can interpret the arguments as compile time values.
      final [target, positionalArguments] = iargs.arguments.positional;
      final namedArguments = iargs.arguments.named
          .firstWhereOrNull((a) => a.name == 'named')
          ?.value;
      final typeArguments = iargs.arguments.named
          .firstWhereOrNull((a) => a.name == 'types')
          ?.value;

      if (target is! ConstantExpression) {
        throw UnimplementedError("For now can only invoke() constants");
      }

      final positionalArgumentsExpressions = <Expression>[];
      if (positionalArguments case ListLiteral(:final expressions)) {
        positionalArgumentsExpressions.addAll(expressions);
      } else {
        throw UnimplementedError(
            "Invoke should have a list literal as an argument");
      }

      final typeArgumentsTypes = <DartType>[];
      if (typeArguments case ListLiteral(:final expressions)) {
        for (var e in expressions) {
          if (e
              case ConstantExpression(
                constant: TypeLiteralConstant(:final type)
              )) {
            typeArgumentsTypes.add(type);
          } else {
            throw UnimplementedError(
                "Invoke typeArguments should be list of type literals");
          }
        }
      }

      final namedArgumentsExpressions = <NamedExpression>[];
      if (namedArguments != null) {
        final map = tryReconstructMapLiteral(namedArguments);
        if (map == null) {
          throw UnimplementedError(
              'Unable to translate namedArguments for invoke()');
        }
        for (var MapEntry(:key, :value) in map.entries) {
          namedArgumentsExpressions.add(NamedExpression(key, value));
        }
      }

      final args = Arguments(
        positionalArgumentsExpressions,
        types: typeArgumentsTypes.isNotEmpty ? typeArgumentsTypes : null,
        named: namedArgumentsExpressions.isNotEmpty
            ? namedArgumentsExpressions
            : null,
      );
      if (target.constant
          case StaticTearOffConstant(target: final targetProcedure)) {
        // TODO: validate that targetProcedure can actually accept the parameters
        // we are giving to it.
        return StaticInvocation(targetProcedure, args);
      } else if (target.constant
          case ConstructorTearOffConstant(target: final targetConstructor)) {
        return ConstructorInvocation(targetConstructor as Constructor, args);
      } else {
        throw UnimplementedError(
            "For now can only invoke() static tear-offs and constructors");
      }
    });
  }

  Constant _createTypeInfo(DartType targetType) {
    return mirrors[targetType] ??=
        InstanceConstant(classTypeInfo.reference, [targetType], {});
  }

  final fieldsOf = <DartType, Constant>{};
  final fields = <Field, Constant>{};
  final fieldInv = Map<Constant, Field>.identity();

  Constant _createFieldInfo(Field field, DartType hostType) {
    var kfield = fields[field];
    if (kfield != null) {
      return kfield;
    }
    kfield =
        InstanceConstant(classFieldInfo.reference, [hostType, field.type], {});
    fields[field] = kfield;
    fieldInv[kfield] = field;
    return kfield;
  }

  void registerHandler(
      Procedure member,
      TreeNode Function(Procedure, KonstArguments, InvocationArguments)
          handler) {
    handlers[member] = (kargs, iargs) => handler(member, kargs, iargs);
  }

  bool folding = false;

  // static final notConstant = NullConstant();

  Map<VariableDeclaration, Constant> variableValues = {};

  @override
  TreeNode visitBlock(Block node) {
    if (folding) {
      final statements = node.statements;
      for (int i = 0; i < statements.length; ++i) {
        final result = transform(statements[i]);
        result.parent = node;
        statements[i] = result;
        if (result is ReturnStatement ||
            (result is Block &&
                result.statements.lastOrNull is ReturnStatement)) {
          statements.length = i + 1;
          break;
        }
      }
      return node;
    }
    return super.visitBlock(node);
  }

  @override
  TreeNode visitVariableDeclaration(VariableDeclaration node) {
    node.transformChildren(this);
    if (folding) {
      if (node.initializer case final ConstantExpression constExpr) {
        variableValues[node] = constExpr.constant;
      } else {
        variableValues.remove(node);
      }
    }
    return node;
  }

  @override
  TreeNode visitVariableSet(VariableSet node) {
    node.transformChildren(this);

    if (folding) {
      if (node.value case final ConstantExpression constExpr) {
        variableValues[node.variable] = constExpr.constant;
      } else {
        variableValues.remove(node.variable);
      }
    }

    return node;
  }

  @override
  TreeNode visitVariableGet(VariableGet node) {
    if (folding) {
      final value = variableValues[node.variable];
      if (value != null) {
        return ConstantExpression(value);
      }
    }
    return super.visitVariableGet(node);
  }

  @override
  TreeNode visitEqualsNull(EqualsNull node) {
    // TODO: implement visitEqualsNull
    if (folding) {
      node.transformChildren(this);
      if (node.expression case ConstantExpression(constant: final constant)) {
        if (constant is NullConstant) {
          return ConstantExpression(BoolConstant(true));
        } else {
          return ConstantExpression(BoolConstant(false));
        }
      }
      return node;
    }
    return super.visitEqualsNull(node);
  }

  void mergeState(Map<VariableDeclaration, Constant> otherState) {
    for (var k in variableValues.keys.toList()) {
      final v = variableValues[k]!;
      final otherV = otherState[k];
      if (otherV != v) {
        variableValues.remove(k);
      }
    }
  }

  @override
  TreeNode visitLogicalExpression(LogicalExpression node) {
    if (folding) {
      node.transformChildren(this);
      if (node.left
          case ConstantExpression(constant: BoolConstant(:final value))) {
        if (value) {
          if (node.operatorEnum == LogicalExpressionOperator.OR) {
            return node.left;
          } else {
            return node.right;
          }
        } else {
          if (node.operatorEnum == LogicalExpressionOperator.AND) {
            return node.left;
          } else {
            return node.right;
          }
        }
      }

      if (node.right
          case ConstantExpression(constant: BoolConstant(:final value))) {
        if (value) {
          if (node.operatorEnum == LogicalExpressionOperator.OR) {
            return node.right;
          } else {
            return node.left;
          }
        } else {
          if (node.operatorEnum == LogicalExpressionOperator.AND) {
            return node.right;
          } else {
            return node.left;
          }
        }
      }

      return node;
    }
    return super.visitLogicalExpression(node);
  }

  @override
  TreeNode visitNot(Not node) {
    if (folding) {
      node.transformChildren(this);
      if (node.operand
          case ConstantExpression(constant: BoolConstant(:final value))) {
        return ConstantExpression(BoolConstant(!value));
      }
    }
    return super.visitNot(node);
  }

  @override
  TreeNode visitForInStatement(ForInStatement node) {
    if (folding) {
      if (isAnnotatedWithKonst(node.variable)) {
        final iterable = transform(node.iterable);
        if (iterable is! ConstantExpression ||
            iterable.constant is! ListConstant) {
          diagnosticReporter.report(
              messageKonstExpressionDoesNotEvaluateToACompileTimeConstant,
              node.iterable.fileOffset,
              1,
              node.iterable.location!.file);
          throw _AbortTransformation();
        }
        final list = (iterable.constant as ListConstant).entries;
        return Block([
          for (var value in list)
            transform(CloneVisitorNotMembers(variableSubstitutions: {
              node.variable: ReplaceWithConstant(value)
            }).clone(node.body))
        ]);
      }
    }
    return super.visitForInStatement(node);
  }

  @override
  TreeNode visitIfStatement(IfStatement node) {
    if (folding) {
      node.condition = transform(node.condition);
      node.condition.parent = node;

      if (node.condition
          case ConstantExpression(constant: BoolConstant(:final value))) {
        if (value) {
          // We only need to recurse into true branch
          return transform(node.then);
        } else {
          if (node.otherwise case final otherwise?) {
            // We only need to recurse into false branch.
            return transform(otherwise);
          } else {
            return EmptyStatement();
          }
        }
      }

      // Need to go both ways.
      final entryState = Map.of(variableValues);
      node.then = transform(node.then);
      node.then.parent = node;
      final afterThenState = variableValues;
      variableValues = entryState;
      if (node.otherwise case final otherwise?) {
        node.otherwise = transform(otherwise)..parent = node;
      }
      mergeState(afterThenState);
    }

    // TODO: implement visitIfStatement
    return super.visitIfStatement(node);
  }

  final hasKonst = <Procedure, KonstSignature>{};

  final mirrors = <DartType, Constant>{};

  final List<(Procedure, TreeNode)> pending = [];

  KonstSignature lookupSignature(Procedure node) {
    return (hasKonst[node] ??= computeKonstSignature(node));
  }

  bool isKonst(Procedure node) {
    return !lookupSignature(node).isEmpty;
  }

  bool isAnnotatedWithKonst(Annotatable node) {
    return pragmaParser
        .parsedPragmas(node.annotations)
        .contains(const ParsedKonstPragma());
  }

  KonstSignature computeKonstSignature(Procedure node) {
    final hasReceiver = node.isInstanceMember && isAnnotatedWithKonst(node);
    int typeParameters = 0;
    int positionalParameters = 0;
    var namedParameters = const <String>[];

    for (var (idx, param) in node.function.typeParameters.indexed) {
      if (isAnnotatedWithKonst(param)) {
        typeParameters |= 1 << idx;
      }
    }

    for (var (idx, param) in node.function.positionalParameters.indexed) {
      if (isAnnotatedWithKonst(param)) {
        positionalParameters |= 1 << idx;
      }
    }

    for (var param in node.function.namedParameters) {
      if (isAnnotatedWithKonst(param)) {
        if (identical(namedParameters, const <String>[])) {
          namedParameters = <String>[];
        } else {
          namedParameters.add(param.name!);
        }
      }
    }

    if (typeParameters == 0 &&
        positionalParameters == 0 &&
        identical(namedParameters, const <String>[]) &&
        !hasReceiver) {
      return const KonstSignature();
    }

    return KonstSignature(
      hasReceiver: hasReceiver,
      typeParameters: typeParameters,
      positionalParameters: positionalParameters,
      namedParameters: namedParameters,
    );
  }

  StaticTypeContext? staticTypeContext;

  @override
  TreeNode defaultMember(Member node) {
    try {
      staticTypeContext = StaticTypeContext(node, typeEnvironment);
      return super.defaultMember(node);
    } finally {
      staticTypeContext = null;
    }
  }

  @override
  TreeNode visitClass(Class node) {
    if (node.isMixinDeclaration &&
        node.typeParameters.any(isAnnotatedWithKonst)) {
      return node;
    }
    return super.visitClass(node);
  }

  @override
  TreeNode visitProcedure(Procedure node) {
    if (isKonst(node)) {
      return node;
    }

    final oldFolding = folding;
    try {
      if (!folding && node.enclosingClass?.isEliminatedMixin == true) {
        for (var type in node.enclosingClass!.implementedTypes) {
          if (type.classNode.isMixinDeclaration &&
              type.classNode.typeParameters.any(isAnnotatedWithKonst)) {
            startFolding();
            break;
          }
        }
      }
      return super.visitProcedure(node);
    } finally {
      folding = oldFolding;
    }
  }

  Constant? evaluate(Expression node) {
    final result = constantEvaluator.evaluate(staticTypeContext!, node);
    if (result is UnevaluatedConstant) {
      return null;
    }
    return result;
  }

  TreeNode? handleKonstInvocation(
    Procedure target,
    Arguments arguments, {
    Expression? receiver,
    required TreeNode errorLocation,
  }) {
    final signature = lookupSignature(target);
    if (signature.isEmpty) {
      return null;
    }

    final Constant? receiverSubstitution;
    final typeSubstitution = <TypeParameter, DartType>{};
    final varSubstitution = <VariableDeclaration, Constant>{};

    if (signature.hasReceiver) {
      final value = evaluate(receiver!);
      if (value == null) {
        diagnosticReporter.report(
            messageKonstExpressionDoesNotEvaluateToACompileTimeConstant,
            receiver.fileOffset,
            1,
            receiver.location!.file);
        throw _AbortTransformation();
      }
      receiverSubstitution = value;
    } else {
      receiverSubstitution = null;
    }

    for (var i = 0; i < arguments.types.length; i++) {
      if (signature.hasTypeParameter(i)) {
        final type = arguments.types[i];
        if (!type.accept(_IsInstantiatedType())) {
          diagnosticReporter.report(
              messageKonstExpressionDoesNotEvaluateToACompileTimeConstant,
              errorLocation.fileOffset,
              1,
              errorLocation.location!.file);
          throw _AbortTransformation();
        }
        typeSubstitution[target.function.typeParameters[i]] = type;
      }
    }

    for (var i = 0; i < arguments.positional.length; i++) {
      if (signature.hasPositionalParameter(i)) {
        final arg = arguments.positional[i];
        final value = evaluate(arg);
        if (value == null) {
          diagnosticReporter.report(
            messageKonstExpressionDoesNotEvaluateToACompileTimeConstant,
            arg.fileOffset,
            1,
            arg.location!.file,
          );
          throw _AbortTransformation();
        }
        varSubstitution[target.function.positionalParameters[i]] = value;
      }
    }

    for (var i = 0; i < arguments.named.length; i++) {
      final param = arguments.named[i];
      if (signature.hasNamedParameter(param.name)) {
        final value = evaluate(param.value);
        if (value == null) {
          diagnosticReporter.report(
            messageKonstExpressionDoesNotEvaluateToACompileTimeConstant,
            param.fileOffset,
            1,
            param.location!.file,
          );
          throw _AbortTransformation();
        }
        varSubstitution[target.function.namedParameters[i]] = value;
      }
    }

    final konstArgs = KonstArguments(
      receiverSubstitution,
      varSubstitution,
      typeSubstitution,
    );

    final result =
        apply(target, konstArgs, InvocationArguments(receiver, arguments));
    if (result is Procedure) {
      // Prune arguments.
      if (signature.hasReceiver) {
        throw new StateError('Unexpected @konst receiver');
      }

      {
        var j = 0;
        for (var i = 0; i < arguments.types.length; i++) {
          if (!signature.hasTypeParameter(i)) {
            arguments.types[j++] = arguments.types[i];
          }
        }
        arguments.types.length = j;
      }

      {
        var j = 0;
        for (var i = 0; i < arguments.positional.length; i++) {
          if (!signature.hasPositionalParameter(i)) {
            arguments.positional[j++] = arguments.positional[i];
          }
        }
        arguments.positional.length = j;
      }

      {
        var j = 0;
        for (var i = 0; i < arguments.named.length; i++) {
          final param = arguments.named[i];
          if (!signature.hasNamedParameter(param.name)) {
            arguments.named[j++] = param;
          }
        }
        arguments.named.length = j;
      }
    }
    return result;
  }

  static final emptyArguments = Arguments.empty();

  @override
  TreeNode visitInstanceGet(InstanceGet node) {
    final result = super.visitInstanceGet(node);
    if (result is! InstanceGet) {
      return result;
    }
    node = result;

    if (node.interfaceTarget is! Procedure) {
      return node;
    }

    if (handleKonstInvocation(
      node.interfaceTarget as Procedure,
      emptyArguments,
      receiver: node.receiver,
      errorLocation: node,
    )
        case final konstResult?) {
      if (konstResult is! Procedure) {
        return konstResult;
      }
      node.interfaceTarget = konstResult;
      node.name = konstResult.name;
    }
    return node;
  }

  @override
  TreeNode visitInstanceInvocation(InstanceInvocation node) {
    final result = super.visitInstanceInvocation(node);
    if (result is! InstanceInvocation) {
      return result;
    }
    node = result;

    if (handleKonstInvocation(
      node.interfaceTarget,
      node.arguments,
      receiver: node.receiver,
      errorLocation: node,
    )
        case final konstResult?) {
      if (konstResult is! Procedure) {
        return konstResult;
      }
      node.interfaceTarget = konstResult;
      node.name = konstResult.name;
    }
    return node;
  }

  final handlers =
      <Procedure, TreeNode Function(KonstArguments, InvocationArguments)>{};

  int id = 0;

  Procedure instantiate(Procedure target, KonstArguments konstArgs) {
    if (target.isExternal) {
      throw UnimplementedError('Unable apply ${target} to ${konstArgs}');
    }

    final clone = CloneVisitorWithMembers(
      typeSubstitution: konstArgs.typeArguments,
      shouldDrop: konstArgs.typeArguments.keys.toSet(),
    ).cloneProcedure(target, null);
    clone.name = Name(target.name.text + '#${id++}', target.name.library);
    pending.add((clone, target.enclosingLibrary));
    return clone;
  }

  TreeNode Function(KonstArguments, InvocationArguments) instantiationHandler(
      Procedure target) {
    final cache = <KonstArguments, Procedure>{};
    return (konstArgs, _) {
      return cache[konstArgs] ??= instantiate(target, konstArgs);
    };
  }

  TreeNode apply(
      Procedure target, KonstArguments konstArgs, InvocationArguments args) {
    final handler = (handlers[target] ??= instantiationHandler(target));
    return handler(konstArgs, args);
  }

  @override
  TreeNode visitStaticInvocation(StaticInvocation node) {
    final result = super.visitStaticInvocation(node);
    if (result is! StaticInvocation) {
      return result;
    }
    node = result;

    if (handleKonstInvocation(node.target, node.arguments, errorLocation: node)
        case final konstResult?) {
      if (konstResult is! Procedure) {
        return transform(konstResult);
      }
      node.target = konstResult;
    }

    return node;
  }

  void startFolding() {
    folding = true;
    variableValues.clear();
  }

  void emitPending() {
    var failed = false;
    while (pending.isNotEmpty) {
      final (n, parent) = pending.removeLast();
      if (parent is Library) {
        parent.addProcedure(n);
      } else {
        throw UnimplementedError();
      }

      try {
        startFolding();
        n.accept(this);
        folding = false;
      } catch (e, st) {
        print('Crashed when translating ${n}');
        print(e);
        print(st);
        failed = true;
      }
    }
    if (failed) {
      throw UnimplementedError('failed');
    }
  }
}

class _IsInstantiatedType implements DartTypeVisitor<bool> {
  @override
  bool visitAuxiliaryType(AuxiliaryType node) => throw UnimplementedError();

  @override
  bool visitDynamicType(DynamicType node) => true;

  @override
  bool visitExtensionType(ExtensionType node) {
    return visitTypes(node.typeArguments);
  }

  @override
  bool visitFunctionType(FunctionType node) {
    return visitTypes(node.typeParameters.map((p) => p.bound)) &&
        node.returnType.accept(this) &&
        visitTypes(node.positionalParameters) &&
        visitTypes(node.namedParameters.map((p) => p.type));
  }

  @override
  bool visitFutureOrType(FutureOrType node) {
    return node.typeArgument.accept(this);
  }

  @override
  bool visitInterfaceType(InterfaceType node) {
    return visitTypes(node.typeArguments);
  }

  @override
  bool visitIntersectionType(IntersectionType node) {
    return node.left.accept(this) && node.right.accept(this);
  }

  @override
  bool visitInvalidType(InvalidType node) => false;

  @override
  bool visitNeverType(NeverType node) => true;

  @override
  bool visitNullType(NullType node) => true;

  @override
  bool visitRecordType(RecordType node) {
    return visitTypes(node.positional) &&
        visitTypes(node.named.map((t) => t.type));
  }

  @override
  bool visitStructuralParameterType(StructuralParameterType node) {
    // TODO: implement visitStructuralParameterType
    throw UnimplementedError();
  }

  @override
  bool visitTypeParameterType(TypeParameterType node) => false;

  @override
  bool visitTypedefType(TypedefType node) => true;

  @override
  bool visitVoidType(VoidType node) {
    // TODO: implement visitVoidType
    throw UnimplementedError();
  }

  bool visitTypes(Iterable<DartType> types) {
    for (var type in types) {
      if (!type.accept(this)) {
        return false;
      }
    }
    return true;
  }
}

class _AbortTransformation {
  const _AbortTransformation();
}

class ReplaceWithConstant implements VariableSubstitution {
  final Constant value;
  ReplaceWithConstant(this.value);

  @override
  TreeNode replaceVariableGet(VariableGet node) {
    return ConstantExpression(value);
  }

  @override
  TreeNode replaceVariableSet(VariableSet node) {
    // TODO: implement replaceVariableSet
    throw UnimplementedError();
  }
}
