// Copyright (c) 2021, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// Rapid type analysis on kernel AST.

import 'dart:core' hide Type;

import 'package:kernel/ast.dart';
import 'package:kernel/class_hierarchy.dart' show ClassHierarchy;
import 'package:kernel/library_index.dart' show LibraryIndex;
import 'package:kernel/core_types.dart' show CoreTypes;
import 'package:kernel/target/targets.dart' show Target;

import 'calls.dart' as calls
    show Selector, DirectSelector, InterfaceSelector, VirtualSelector;
import 'native_code.dart'
    show EntryPointsListener, NativeCodeOracle, PragmaEntryPointsVisitor;
import 'protobuf_handler.dart' show ProtobufHandler;
import 'types.dart' show TFClass, Type, ConcreteType;
import '../pragma.dart' show ConstantPragmaAnnotationParser;

class Selector {
  final Name name;
  final bool setter;

  Selector(this.name, this.setter);

  @override
  int get hashCode => name.hashCode ^ setter.hashCode;

  @override
  bool operator ==(Object other) {
    if (other is! Selector) {
      return false;
    }

/*
    if (!identical(this.name.text, other.name.text)) {
      if (this.name.text == other.name.text)
        throw 'Something is wrong with canonicalization: ${this.name.text} vs ${other.name.text}';
    }

    if (!identical(this.name.library, other.name.library)) {
      if ((this.name.library == other.name.library))
        throw 'Something is wrong with canonicalization';
    }
*/

    return identical(this.name.text, other.name.text) &&
        identical(this.name.library, other.name.library) &&
        this.setter == other.setter;
  }
}

class Invocation {
  final Member interfaceTarget;
  final List arguments;

  Invocation(this.interfaceTarget, this.arguments);
}

class Summary {
  final List<Invocation> invocations;

  Summary(this.invocations);
}

// class CallersInfo {
//  final Member interfaceTarget;
//  final List
//
//  CallersInfo({required this.interfaceTarget});
//}

class ClassInfo extends TFClass {
  final ClassInfo? superclass;
  final Set<ClassInfo> supertypes; // All super-types including this.
  final Set<ClassInfo> subclasses = Set<ClassInfo>();
  final Set<ClassInfo> subtypes = Set<ClassInfo>();

  final Set<Selector>
      calledDynamicSelectors; // Selectors called with dynamic and interface calls.
  final Set<Selector> calledVirtualSelectors;

  bool isAllocated = false;

  late final Map<Name, Member> _dispatchTargetsSetters =
      _initDispatchTargets(true);
  late final Map<Name, Member> _dispatchTargetsNonSetters =
      _initDispatchTargets(false);

  late final Map<Name, Member> _allProcedures = {
    for (var p in classNode.procedures) p.name: p
  };

  ClassInfo(int id, Class classNode, this.superclass, this.supertypes,
      this.calledDynamicSelectors, this.calledVirtualSelectors)
      : super(id, classNode) {
    supertypes.add(this);
    for (var sup in supertypes) {
      sup.subtypes.add(this);
    }
    for (ClassInfo? sup = this; sup != null; sup = sup.superclass) {
      sup.subclasses.add(this);
    }
  }

  late final ConcreteType concreteType = ConcreteType(this, null);

  Map<Name, Member> _initDispatchTargets(bool setters) {
    Map<Name, Member> targets;
    final superclass = this.superclass;
    if (superclass != null) {
      targets = Map.from(setters
          ? superclass._dispatchTargetsSetters
          : superclass._dispatchTargetsNonSetters);
    } else {
      targets = {};
    }
    for (Field f in classNode.fields) {
      if (!f.isStatic && !f.isAbstract) {
        if (!setters || f.hasSetter) {
          targets[f.name] = f;
        }
      }
    }
    for (Procedure p in classNode.procedures) {
      if (!p.isStatic && !p.isAbstract) {
        if (p.isSetter == setters) {
          targets[p.name] = p;
        }
      }
    }
    return targets;
  }

  Member? getDispatchTarget(Selector selector) {
    return (selector.setter
        ? _dispatchTargetsSetters
        : _dispatchTargetsNonSetters)[selector.name];
  }
}

class _ClassHierarchyCache {
  final Map<Class, ClassInfo> classes = <Class, ClassInfo>{};
  int _classIdCounter = 0;

  _ClassHierarchyCache();

  ClassInfo getClassInfo(Class c) {
    return classes[c] ??= _createClassInfo(c);
  }

  ClassInfo _createClassInfo(Class c) {
    final supertypes = Set<ClassInfo>();
    final dynSel = Set<Selector>();
    for (var sup in c.supers) {
      final supInfo = getClassInfo(sup.classNode);
      supertypes.addAll(supInfo.supertypes);
      dynSel.addAll(supInfo.calledDynamicSelectors);
    }
    Class? superclassNode = c.superclass;
    ClassInfo? superclass;
    final virtSel = Set<Selector>();
    if (superclassNode != null) {
      superclass = getClassInfo(superclassNode);
      virtSel.addAll(superclass.calledVirtualSelectors);
    }
    return ClassInfo(
        ++_classIdCounter, c, superclass, supertypes, dynSel, virtSel);
  }

  ConcreteType addAllocatedClass(Class cl, RapidTypeAnalysis rta) {
    assert(!cl.isAbstract);
    final ClassInfo classInfo = getClassInfo(cl);
    if (!classInfo.isAllocated) {
      classInfo.isAllocated = true;
      for (var sel in classInfo.calledDynamicSelectors) {
        final member = classInfo.getDispatchTarget(sel);
        if (member != null) {
          rta.addMember(member);
        }
      }
      for (var sel in classInfo.calledVirtualSelectors) {
        final member = classInfo.getDispatchTarget(sel);
        if (member != null) {
          rta.addMember(member);
        }
      }
    }
    return classInfo.concreteType;
  }

  void addDynamicCall(Selector selector, Class cl, RapidTypeAnalysis rta) {
    final ClassInfo classInfo = getClassInfo(cl);
    for (var sub in classInfo.subtypes) {
      if (sub.calledDynamicSelectors.add(selector) && sub.isAllocated) {
        final member = sub.getDispatchTarget(selector);
        if (member != null) {
          rta.addMember(member);
        }
      }
    }
  }

  void forEachPossibleTarget(Member interfaceTarget, void Function(Member) cb) {
    final cl = interfaceTarget.enclosingClass!;
    final selector = Selector(interfaceTarget.name, false);

    final ClassInfo classInfo = getClassInfo(cl);
    for (var sub in classInfo.subclasses) {
      if (sub.isAllocated) {
        final member = sub.getDispatchTarget(selector);
        if (member != null) {
          cb(member);
        }
      }
    }
  }

  void addVirtualCall(Selector selector, Class cl, RapidTypeAnalysis rta) {
    final ClassInfo classInfo = getClassInfo(cl);
    for (var sub in classInfo.subclasses) {
      if (sub.calledVirtualSelectors.add(selector) && sub.isAllocated) {
        final member = sub.getDispatchTarget(selector);
        if (member != null) {
          rta.addMember(member);
        }
      }
    }
  }
}

class Unknown {
  const Unknown();
}

class Forward {
  const Forward();
}

class CallerInfo {
  var arg0;
  Member? unknownSource;
  Set<Member>? callers;

  void addCallFrom(Member caller, Object arg0) {
    if (identical(this.arg0, const Unknown())) {
      return;
    }

    if (identical(arg0, const Unknown())) {
      // Bad incomming argument.
      this.arg0 = const Unknown();
      this.unknownSource = caller;
      callers = null;
      return;
    }

    if (identical(arg0, const Forward())) {
      callers ??= {};
      callers!.add(caller);
      return;
    }

    if (identical(this.arg0, null)) {
      this.arg0 = arg0;
      return;
    } else {
      var s = this.arg0;
      if (s is! Set<Constant>) {
        s = Set<Constant>()..add(s);
        this.arg0 = s;
      }
      s.add(arg0 as Constant);
    }
  }
}

class RapidTypeAnalysis {
  final CoreTypes coreTypes;
  final ClassHierarchy hierarchy;
  final LibraryIndex libraryIndex;
  final _ClassHierarchyCache hierarchyCache = _ClassHierarchyCache();
  final ProtobufHandler? protobufHandler;

  final Set<Member> visited = {};
  final List<Member> workList = [];

  final Map<Member, Summary> summaries = {};

  final Map<Member, CallerInfo> callerInfo = {};

  static const injectorLibraryUri =
      'package:third_party.dart_src.angular.angular/src/di/injector.dart';

  late final tokenToProvider = libraryIndex.getMember(
      injectorLibraryUri, 'GeneratedInjector', 'tokenToProvider');
  late final Map_addAll = libraryIndex.getMember('dart:core', 'Map', 'addAll');

  late final injectorGet =
      libraryIndex.getMember(injectorLibraryUri, 'Injector', 'get');

  late final injectorProvideType =
      libraryIndex.getMember(injectorLibraryUri, 'Injector', 'provideType');

  RapidTypeAnalysis(Component component, this.coreTypes, Target target,
      this.hierarchy, this.libraryIndex, this.protobufHandler) {
    Procedure? main = component.mainMethod;
    if (main != null) {
      addMember(main);
    }
    final annotationMatcher = ConstantPragmaAnnotationParser(coreTypes, target);
    final nativeCodeOracle = NativeCodeOracle(libraryIndex, annotationMatcher);
    component.accept(PragmaEntryPointsVisitor(
        _EntryPointsListenerImpl(this), nativeCodeOracle, annotationMatcher));
    run();
  }

  List<Class> get allocatedClasses {
    return <Class>[
      for (var entry in hierarchyCache.classes.entries)
        if (entry.value.isAllocated) entry.key
    ];
  }

  bool isAllocatedClass(Class cl) =>
      hierarchyCache.classes[cl]?.isAllocated ?? false;

  ConcreteType addAllocatedClass(Class cl) =>
      hierarchyCache.addAllocatedClass(cl, this);

  void addMember(Member member) {
    if (visited.add(member)) {
      workList.add(member);
    }
  }

  Member? currentMember;

  late final Class classInject = libraryIndex.getClass(
      'package:third_party.dart_src.angular.angular/src/meta/di_arguments.dart',
      'Inject');

  late final Reference Inject_token = libraryIndex
      .getField(
          'package:third_party.dart_src.angular.angular/src/meta/di_arguments.dart',
          'Inject',
          'token')
      .fieldReference;

  late final Class classOptional = libraryIndex.getClass(
      'package:third_party.dart_src.angular.angular/src/meta/di_arguments.dart',
      'Optional');

  late final Class classSkipSelf = libraryIndex.getClass(
      'package:third_party.dart_src.angular.angular/src/meta/di_arguments.dart',
      'SkipSelf');

  late final Class classSelf = libraryIndex.getClass(
      'package:third_party.dart_src.angular.angular/src/meta/di_arguments.dart',
      'Self');

  late final Class classHost = libraryIndex.getClass(
      'package:third_party.dart_src.angular.angular/src/meta/di_arguments.dart',
      'Host');

  late final Member Reflector_registerDependencies = libraryIndex.getMember(
      'package:third_party.dart_src.angular.angular/src/reflector.dart',
      '::',
      'registerDependencies');

  late final Member RuntimeInjector_resolveArg = libraryIndex.getMember(
      'package:third_party.dart_src.angular.angular/src/di/injector/runtime.dart',
      '_RuntimeInjector',
      '_resolveArg');
  late final Member RuntimeInjector_resolveArgs = libraryIndex.getMember(
      'package:third_party.dart_src.angular.angular/src/di/injector/runtime.dart',
      '_RuntimeInjector',
      '_resolveArgs');

  late final Member RuntimeInjector_resolveMeta = libraryIndex.getMember(
      'package:third_party.dart_src.angular.angular/src/di/injector/runtime.dart',
      '_RuntimeInjector',
      '_resolveMeta');

  late final Member Provider_buildAtRuntime = libraryIndex.getMember(
      'package:third_party.dart_src.angular.angular/src/meta/di_providers.dart',
      'Provider',
      '_buildAtRuntime');

  void addLiveToken(Constant token) {
    if (!injectedTokens.contains(token)) {
      liveTokens.add(token);
      print('marking token $token live');
    }
  }

  void addCall(Class? currentClass, Member? interfaceTarget, Name name,
      bool isVirtual, bool isSetter,
      {Object arg0 = const Unknown()}) {
    final Class cl = isVirtual
        ? currentClass!
        : (interfaceTarget != null
            ? interfaceTarget.enclosingClass!
            : coreTypes.objectClass);
    final Selector selector = Selector(name, isSetter);

    if (isVirtual) {
      hierarchyCache.addVirtualCall(selector, cl, this);
    } else {
      hierarchyCache.addDynamicCall(selector, cl, this);
    }

    if (interfaceTarget != null && currentMember != null) {
      var info = callerInfo[interfaceTarget];
      if (info == null) {
        info = CallerInfo();
        callerInfo[interfaceTarget] = info;
      }
      if (identical(const Unknown(), arg0)) {
        if (interfaceTarget == injectorGet) {
          throw 'Bad call in ${currentMember!} ${currentMember!.enclosingClass} ${currentMember!.enclosingLibrary} passing $arg0';
        } else if (interfaceTarget == RuntimeInjector_resolveArg) {
          if (currentMember == RuntimeInjector_resolveArgs) {
            return; // Ignore for now.
          }
        } else if (currentMember == RuntimeInjector_resolveMeta) {
          return;
        } else if (currentMember == Provider_buildAtRuntime) {
          return;
        } else if (currentMember == runAction) {
          return;
        }
      }
      info.addCallFrom(currentMember!, arg0);
    }
  }

  final Set<Constant> injectedTokens = {};

  final Set<Constant> liveTokens = {};

  void run() {
    //incomingState[injectorGet] = initialState(injectorGet);

    final memberVisitor = _MemberVisitor(this);

    do {
      while (workList.isNotEmpty || invalidateProtobufFields()) {
        final member = workList.removeLast();
        protobufHandler?.beforeSummaryCreation(member);
        member.accept(memberVisitor);
      }
      print('total members live ${visited.length}');

      final Map<Member, Set<Member>> cache = {};
      Set<Member> possibleTargets(Member target) {
        return cache.putIfAbsent(target, () {
          final result = <Member>{};
          if (target.enclosingClass == null) {
            result.add(target);
            return result; // Static call.
          }

          final cls = hierarchyCache.getClassInfo(target.enclosingClass!);

          for (var t in cls.supertypes) {
            final p = t._allProcedures[target.name];
            if (p != null) {
              result.add(p);
            }
          }

          for (var t in cls.subclasses) {
            final p = t._allProcedures[target.name];
            if (p != null && p.isAbstract) {
              result.add(p);
            }
          }
          return result;
        });
      }

      final tokensWorklist = Set<Constant>();

      for (var token in liveTokens) {
        if (injectedTokens.add(token)) {
          tokensWorklist.add(token);
        }
      }
      liveTokens.clear();

      bool unknown = false;
      final Set<Member> toAnalyze = {};
      toAnalyze.add(injectorGet);
      workList.add(injectorGet);
      while (workList.isNotEmpty) {
        final member = workList.removeLast();
        for (var target in possibleTargets(member)) {
          final callers = callerInfo[target];
          if (callers == null) continue;

          if (identical(callers.arg0, const Unknown())) {
            print(
                'Hit UKNOWN at $target originating from ${callers.unknownSource} ${callers.unknownSource!.enclosingClass} - ${callers.unknownSource!.enclosingLibrary}');
            unknown = true;
            continue;
          }

          if (callers.arg0 != null) {
            if (callers.arg0 is Set) {
              for (var c in callers.arg0 as Set<Constant>) {
                if (injectedTokens.add(c)) {
                  tokensWorklist.add(c);
                }
              }
            } else {
              final c = callers.arg0 as Constant;
              if (injectedTokens.add(c)) {
                tokensWorklist.add(c);
              }
            }
          }

          if (callers.callers != null) {
            for (var caller in callers.callers!) {
              if (toAnalyze.add(caller)) {
                workList.add(caller);
              }
            }
          }
        }
      }

      if (unknown) {
        throw 'Can not continue due to UNKNOWN input to Injector';
      }

      print('discovered new tokens: ${tokensWorklist.length}');

      final toExpand = tokensWorklist.toList();
      while (toExpand.isNotEmpty) {
        final tok = toExpand.removeLast();
        final deps = _dependencies[tok];
        if (deps != null) {
          for (var arg in deps.entries) {
            late Constant token;
            if (arg is ListConstant) {
              for (var meta in arg.entries) {
                if (meta is InstanceConstant) {
                  if (meta.classNode == classInject) {
                    token = meta.fieldValues[Inject_token]!;
                  } else if (meta.classNode == classOptional ||
                      meta.classNode == classSelf ||
                      meta.classNode == classSkipSelf ||
                      meta.classNode == classHost) {
                    continue;
                  } else {
                    throw 'Unknown meta: ${meta}';
                  }
                } else {
                  token = meta;
                }
              }
            } else {
              token = arg;
            }
            if (injectedTokens.add(token)) {
              tokensWorklist.add(token);
              toExpand.add(token);
            }
          }
        }
      }

      print('after expanding dependencies: ${tokensWorklist.length}');

      for (var map in tokenToProviderMaps) {
        TreeNode member = map;
        while (member is! Member) {
          member = member.parent!;
        }

        for (var entry in map.entries) {
          if (tokensWorklist
              .contains((entry.key as ConstantExpression).constant)) {
            // print('visiting ${entry.key} -> ${entry.value}');
            memberVisitor.inContext(member, entry.value);
          }
        }
      }

      print('have ${workList.length} new members to visit');
    } while (workList.isNotEmpty);
  }

/*
  void propagateArguments() {
    while (workList.isNotEmpty) {
      final member = workList.removeLast();
      for (var target in possibleTargets(member)) {
        final callers = callerInfo[target];
        if (callers == null) continue;
        for (var caller in callers.callers) {
          if (toAnalyze.add(caller)) {
            workList.add(caller);
          }
        }
      }
    }

  }*/

  bool invalidateProtobufFields() {
    final protobufHandler = this.protobufHandler;
    if (protobufHandler == null) {
      return false;
    }
    final fields = protobufHandler.getInvalidatedFields();
    if (fields.isEmpty) {
      return false;
    }
    // Protobuf handler replaced contents of static field initializers.
    bool invalidated = false;
    for (var field in fields) {
      assert(field.isStatic);
      if (visited.contains(field)) {
        workList.add(field);
        invalidated = true;
      }
    }
    return invalidated;
  }

  final List<MapLiteral> tokenToProviderMaps = [];

  void addTokenToProviderMap(MapLiteral map) {
    print('... registered map with ${map.entries.length} entries');
    tokenToProviderMaps.add(map);
  }

  final _dependencies = Map<Constant, ListConstant>.identity();

  void registerDependencies(Constant token, ListConstant deps) {
    if (token is TypeLiteralConstant) {
      if (_dependencies.containsKey(token)) {
        throw 'Duplicated dependencies for $token';
      }
      _dependencies[token] = deps;
    }
  }
}

class _MemberVisitor extends RecursiveVisitor {
  final RapidTypeAnalysis rta;
  final _ConstantVisitor _constantVisitor;

  Member? _currentMember;
  Map<VariableDeclaration, int>? _currentParams;
  Class? _currentClass;
  ClassInfo? _superclassInfo;
  List<Invocation> _invocations = [];

  _MemberVisitor(this.rta) : _constantVisitor = _ConstantVisitor(rta);

  ClassInfo get superclassInfo => _superclassInfo ??=
      rta.hierarchyCache.getClassInfo(_currentClass!.superclass!);

  void inContext(Member member, Node node) {
    _superclassInfo = null;
    _currentMember = rta.currentMember = member;
    _currentClass = member.enclosingClass;
    _currentParams = <VariableDeclaration, int>{};
    if (member is Procedure) {
      for (int i = 0; i < member.function.positionalParameters.length; i++) {
        _currentParams![member.function.positionalParameters[i]] = i;
      }
    }
    node.visitChildren(this);
    if (member is Constructor) {
      // Make sure instance field initializers are visited.
      for (var f in _currentClass!.members) {
        if (f is Field && !f.isStatic) {
          f.initializer?.accept(this);
        }
      }
    }
    _currentParams = null;
    _superclassInfo = null;
    _currentMember = rta.currentMember = null;
    _currentClass = null;
  }

  @override
  void defaultMember(Member node) {
    inContext(node, node);
  }

  @override
  void visitConstructorInvocation(ConstructorInvocation node) {
    rta.addAllocatedClass(node.constructedType.classNode);
    rta.addMember(node.target);
    node.visitChildren(this);
  }

  @override
  void visitInstanceInvocation(InstanceInvocation node) {
    final arg0 = node.arguments.positional.length > 0
        ? node.arguments.positional.first
        : null;
    Object arg0Info = const Unknown();
    if (arg0 is ConstantExpression) {
      arg0Info = arg0.constant;
    } else if (arg0 is VariableGet && _currentParams![arg0.variable] == 0) {
      arg0Info = const Forward(); // Direct forward of the argument
    }

    rta.addCall(_currentClass, node.interfaceTarget, node.name,
        node.receiver is ThisExpression, false,
        arg0: arg0Info);

    if (node.interfaceTarget == rta.Map_addAll) {
      final receiver = node.receiver;
      if (receiver is InstanceGet &&
          receiver.receiver is ThisExpression &&
          receiver.interfaceTarget.name.text == 'tokenToProvider' &&
          node.arguments.positional.first is MapLiteral) {
        print(
            'Found tokenToProvider call in ${_currentMember} ${_currentClass} ${_currentClass?.enclosingLibrary}');
        rta.addTokenToProviderMap(
            node.arguments.positional.first as MapLiteral);
        return;
      }
    }

    node.visitChildren(this);
  }

  @override
  void visitDynamicInvocation(DynamicInvocation node) {
    rta.addCall(null, null, node.name, false, false);
    node.visitChildren(this);
  }

  @override
  void visitEqualsCall(EqualsCall node) {
    rta.addCall(_currentClass, node.interfaceTarget, node.interfaceTarget.name,
        node.left is ThisExpression, false);
    node.visitChildren(this);
  }

  @override
  void visitInstanceGet(InstanceGet node) {
    rta.addCall(_currentClass, node.interfaceTarget, node.name,
        node.receiver is ThisExpression, false);
    node.visitChildren(this);
  }

  @override
  void visitInstanceTearOff(InstanceTearOff node) {
    rta.addCall(_currentClass, node.interfaceTarget, node.name,
        node.receiver is ThisExpression, false);
    node.visitChildren(this);
  }

  @override
  void visitDynamicGet(DynamicGet node) {
    rta.addCall(null, null, node.name, false, false);
    node.visitChildren(this);
  }

  @override
  void visitInstanceSet(InstanceSet node) {
    rta.addCall(_currentClass, node.interfaceTarget, node.name,
        node.receiver is ThisExpression, true);
    node.visitChildren(this);
  }

  @override
  void visitDynamicSet(DynamicSet node) {
    rta.addCall(null, null, node.name, false, true);
    node.visitChildren(this);
  }

  @override
  void visitSuperMethodInvocation(SuperMethodInvocation node) {
    final target = superclassInfo.getDispatchTarget(Selector(node.name, false));
    if (target != null) {
      rta.addMember(target);
    }
    node.visitChildren(this);
  }

  @override
  void visitSuperPropertyGet(SuperPropertyGet node) {
    final target = superclassInfo.getDispatchTarget(Selector(node.name, false));
    if (target != null) {
      rta.addMember(target);
    }
    node.visitChildren(this);
  }

  @override
  void visitSuperPropertySet(SuperPropertySet node) {
    final target = superclassInfo.getDispatchTarget(Selector(node.name, true));
    if (target != null) {
      rta.addMember(target);
    }
    node.visitChildren(this);
  }

  @override
  void visitStaticGet(StaticGet node) {
    rta.addMember(node.target);
    node.visitChildren(this);
  }

  @override
  void visitStaticInvocation(StaticInvocation node) {
    if (node.target == rta.shell_fromMap) {
      final arg0 = node.arguments.positional.first as StaticGet;
      final fieldInit = (arg0.target as Field).initializer as MapLiteral;
      for (var e in fieldInit.entries) {
        final token =
            (e.value as ConstantExpression).constant as TypeLiteralConstant;
        rta.addLiveToken(token);
      }
    } else if (node.target == rta.Reflector_registerDependencies) {
      final token =
          (node.arguments.positional[0] as ConstantExpression).constant;
      final deps = (node.arguments.positional[1] as ConstantExpression).constant
          as ListConstant;
      rta.registerDependencies(token, deps);
    }
    rta.addMember(node.target);
    node.visitChildren(this);
  }

  @override
  void visitStaticSet(StaticSet node) {
    rta.addMember(node.target);
    node.visitChildren(this);
  }

  @override
  void visitRedirectingInitializer(RedirectingInitializer node) {
    rta.addMember(node.target);
    node.visitChildren(this);
  }

  @override
  void visitSuperInitializer(SuperInitializer node) {
    // Re-resolve target due to partial mixin resolution.
    for (var replacement in _currentClass!.superclass!.constructors) {
      if (node.target.name == replacement.name) {
        rta.addMember(replacement);
        break;
      }
    }
    node.visitChildren(this);
  }

  @override
  void visitConstantExpression(ConstantExpression node) {
    _constantVisitor.visit(node.constant);
  }
}

class _ConstantVisitor extends ConstantVisitor<void> {
  final RapidTypeAnalysis rta;
  final Set<Constant> visited = {};

  _ConstantVisitor(this.rta);

  void visit(Constant constant) {
    if (visited.add(constant)) {
      constant.accept(this);
    }
  }

  @override
  void defaultConstant(Constant node) {}

  @override
  void visitListConstant(ListConstant constant) {
    for (final entry in constant.entries) {
      visit(entry);
    }
  }

  @override
  void visitMapConstant(MapConstant constant) {
    for (final entry in constant.entries) {
      visit(entry.key);
      visit(entry.value);
    }
  }

  @override
  void visitSetConstant(SetConstant constant) {
    for (final entry in constant.entries) {
      visit(entry);
    }
  }

  @override
  void visitInstanceConstant(InstanceConstant constant) {
    rta.addAllocatedClass(constant.classNode);
    for (var value in constant.fieldValues.values) {
      visit(value);
    }
  }

  void _visitTearOffConstant(TearOffConstant constant) {
    final Member member = constant.target;
    rta.addMember(member);
    if (member is Constructor) {
      rta.addAllocatedClass(member.enclosingClass);
    }
  }

  @override
  void visitStaticTearOffConstant(StaticTearOffConstant constant) =>
      _visitTearOffConstant(constant);

  @override
  void visitConstructorTearOffConstant(ConstructorTearOffConstant constant) =>
      _visitTearOffConstant(constant);

  @override
  void visitRedirectingFactoryTearOffConstant(
          RedirectingFactoryTearOffConstant constant) =>
      _visitTearOffConstant(constant);

  @override
  void visitInstantiationConstant(InstantiationConstant constant) {
    visit(constant.tearOffConstant);
  }
}

class _EntryPointsListenerImpl implements EntryPointsListener {
  final RapidTypeAnalysis rta;

  _EntryPointsListenerImpl(this.rta);

  @override
  void addFieldUsedInConstant(Field field, Type instance, Type value) {}

  @override
  void addRawCall(calls.Selector selector) {
    if (selector is calls.DirectSelector) {
      rta.addMember(selector.member);
    } else if (selector is calls.InterfaceSelector) {
      rta.addCall(selector.member.enclosingClass!, selector.member,
          selector.name, selector is calls.VirtualSelector, selector.isSetter);
    } else {
      throw 'Unexpected selector ${selector.runtimeType} $selector';
    }
  }

  @override
  ConcreteType addAllocatedClass(Class c) => rta.addAllocatedClass(c);

  @override
  void recordMemberCalledViaInterfaceSelector(Member target) =>
      throw 'Unsupported operation';

  @override
  void recordMemberCalledViaThis(Member target) =>
      throw 'Unsupported operation';

  @override
  void recordTearOff(Member target) => throw 'Unsupported operation';
}
