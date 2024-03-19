import 'dart:collection';

import 'package:kernel/ast.dart';
import 'package:kernel/external_name.dart';
import 'package:kernel/kernel.dart';
import 'package:vm/transformations/type_flow/calls.dart';
import 'package:vm/transformations/type_flow/summary.dart' as tfa;
import 'package:vm/transformations/type_flow/summary_collector.dart' as tfa;
import 'package:vm/transformations/type_flow/analysis.dart' as tfa;
import 'package:vm/transformations/type_flow/types.dart' as tfa;

//
// Parameter()
// Allocation()
// Merge()
// Invocation(args...)
// Escape()
//

sealed class Op {
  final List<int> inputs;

  const Op({required this.inputs});

  @override
  String toString() {
    final sb = StringBuffer();
    sb.write(runtimeType);
    final operands = operandsStr;
    if (operands.isNotEmpty) {
      sb.write('[');
      sb.write(operands);
      sb.write(']');
    }
    final inputsStr = this.inputsStr;
    if (inputsStr.isNotEmpty) {
      sb.write('(');
      sb.write(inputsStr);
      sb.write(')');
    }
    return sb.toString();
  }

  String get operandsStr => '';

  String get inputsStr => inputs.join(', ');

  bool isTrivial(List<Op> ops) {
    return _deref(inputs, ops);
  }

  void rename(List<int> renames) {
    for (var i = 0; i < inputs.length; i++) {
      inputs[i] = renames[inputs[i] + 1];
    }
  }
}

int _derefArg(int arg, List<Op> ops) {
  while (arg != -1) {
    if (ops[arg] case Merge(inputs: [final v])) {
      arg = v;
    } else if (ops[arg] case Parameter(escapes: true)) {
      arg = -1; // We are not interested in parameters that escape.
    } else if (ops[arg] case Allocation(escapes: true)) {
      arg = -1;
    } else if (ops[arg] case Invocation()) {
      arg = -1;
    } else {
      break;
    }
  }
  return arg;
}

bool _deref(List<int> inputs, List<Op> ops) {
  bool trivial = true;
  for (var i = 0; i < inputs.length; i++) {
    inputs[i] = _derefArg(inputs[i], ops);
    if (inputs[i] != -1) {
      trivial = false;
    }
  }
  return trivial;
}

final class Parameter extends Op {
  final int index;
  final String? name;

  bool escapes = false;

  Parameter(this.index, {this.name}) : super(inputs: const <int>[]);

  @override
  String get operandsStr =>
      '$index' + (name != null ? ', $name' : '') + (escapes ? ", escapes" : "");

  @override
  bool isTrivial(List<Op> ops) {
    return false;
  }
}

final class Merge extends Op {
  bool escaped = false;

  Merge({required super.inputs});
}

final class InvocationSelector {
  final int numPositional;
  final List<String> names;
  late final Map<String, int> _descriptor = toDescriptor(names);

  static final _descriptorCache = LinkedHashMap<List<String>, Map<String, int>>(
    equals: (a, b) {
      if (a.length != b.length) {
        return false;
      }

      for (var i = 0; i < a.length; i++) {
        if (a[i] != b[i]) return false;
      }

      return true;
    },
    hashCode: (a) => Object.hashAll(a),
  );

  static Map<String, int> toDescriptor(List<String> names) => _descriptorCache
      .putIfAbsent(names, () => {for (var (i, name) in names.indexed) name: i});

  InvocationSelector(
    this.numPositional, {
    this.names = const [],
  });

  String formatArgs(List<int> args) {
    final result = [
      ...args.take(numPositional),
      for (var i = 0; i < names.length; i++)
        '${names[i]}: ${args[numPositional + i]}',
    ];
    return result.join(', ');
  }

  int getNamed(List<int> inputs, String name) {
    final idx = _descriptor[name];
    if (idx == null) {
      return -1;
    }
    return inputs[numPositional + idx];
  }
}

final class Invocation extends Op {
  final TreeNode? callNode;
  final Selector callSelector;

  final InvocationSelector selector;

  Invocation(
    this.callSelector, {
    required super.inputs,
    required this.selector,
    required this.callNode,
  });

  @override
  String get operandsStr => '$callSelector';

  @override
  String get inputsStr => selector.formatArgs(inputs);
}

final class Escape extends Op {
  final String where;

  Escape({required super.inputs}) : where = StackTrace.current.toString();

  String get operandsStr => where;
}

final class Allocation extends Op {
  TreeNode node;
  bool escapes = false;

  Allocation(this.node, {required super.inputs});

  @override
  String get operandsStr {
    final sb = StringBuffer();
    sb.write(_constructedType);
    if (escapes) {
      sb.write(', escapes');
    }
    return sb.toString();
  }

  String? get _constructedType => switch (node) {
        final ConstructorInvocation ctor => ctor.constructedType.toString(),
        StaticInvocation(:final target) =>
          target.enclosingClass!.name.toString(),
        FunctionNode() => '<closure>',
        _ => '?',
      };

  @override
  bool isTrivial(List<Op> ops) {
    return false;
  }
}

class VariableInfo {
  final Scope owner;

  bool captured = false;
  bool alwaysEscapes = false;

  VariableInfo(this.owner);
}

class Scope {
  final Scope? parent;
  final Map<VariableDeclaration, VariableInfo> variables = {};

  bool hasAwait = false;

  Scope({this.parent});

  void declare(VariableDeclaration v) {
    variables[v] = VariableInfo(this);
  }

  VariableInfo use(VariableDeclaration v) {
    VariableInfo? info = variables[v];
    if (info == null) {
      // Variable is not declare in the current scope. We need to find
      // it in the parent scope.
      if (parent == null) {
        throw 'Use of undeclared variable: $v';
      }

      variables[v] = info = parent!.use(v)..captured = true;
    }
    return info;
  }

  void finishScope() {
    if (hasAwait) {
      // If the scope contains await then we mark all variables escaping.
      for (var v in variables.values) {
        v.alwaysEscapes = true;
      }
    }
  }

  bool alwaysEscapes(VariableDeclaration v) {
    return variables[v]!.alwaysEscapes;
  }

  Iterable<VariableDeclaration> get captured sync* {
    for (var MapEntry(key: v, value: info) in variables.entries) {
      if (info.owner != this) {
        yield v;
      }
    }
  }
}

class _ScopeBuilder extends RecursiveVisitor {
  final this$ = VariableDeclaration('%this%');

  Scope scope = Scope(parent: null);

  final Map<FunctionNode, Scope> scopes = {};

  @override
  void visitVariableDeclaration(VariableDeclaration node) {
    super.visitVariableDeclaration(node);
    scope.declare(node);
  }

  @override
  void visitAwaitExpression(AwaitExpression node) {
    super.visitAwaitExpression(node);
    scope.hasAwait = true;
  }

  @override
  void visitYieldStatement(YieldStatement node) {
    super.visitYieldStatement(node);
    scope.hasAwait = true;
  }

  @override
  void visitThisExpression(ThisExpression node) {
    super.visitThisExpression(node);
    scope.use(this$);
  }

  @override
  void visitVariableGet(VariableGet node) {
    super.visitVariableGet(node);
    scope.use(node.variable);
  }

  @override
  void visitFunctionNode(FunctionNode node) {
    scope = Scope(parent: scope);
    scopes[node] = scope;
    super.visitFunctionNode(node);
    scope.finishScope();
    scope = scope.parent!;
  }

  void buildScopes(FunctionNode node) {
    scopes[node] = scope;
    node.positionalParameters.forEach(visitVariableDeclaration);
    node.namedParameters.forEach(visitVariableDeclaration);
    if (node.parent case final Constructor ctor) {
      for (var initializer in ctor.initializers) {
        initializer.accept(this);
      }
      scope.declare(this$);
    } else if (node.parent case final Member member) {
      if (member.isInstanceMember) {
        scope.declare(this$);
      }
    }
    node.body?.accept(this);
    if (node.parent case Member(isExternal: true)) {
      scope.hasAwait = true;
    }
    scope.finishScope();
  }
}

class _SummaryBuilder extends RecursiveVisitor {
  final scopeBuilder = _ScopeBuilder();

  final tfa.SummaryCollector tfaSummaryCollector;

  late final int numParameters;
  final List<Op> ops = [];

  Map<VariableDeclaration, int> environment = {};
  List<int> stack = [];

  _SummaryBuilder({required this.tfaSummaryCollector});

  @override
  void visitThisExpression(ThisExpression node) {
    stack.add(0);
  }

  @override
  void visitVariableDeclaration(VariableDeclaration node) {
    environment[node] = pushOp(Merge(inputs: <int>[]));
    if (node.initializer case final initializer?) {
      (ops[environment[node]!] as Merge).inputs.add(visitForValue(initializer));
    }
  }

  @override
  void visitVariableGet(VariableGet node) {
    if (!environment.containsKey(node.variable)) {
      throw 'Unable to find ${node.variable}';
    }
    stack.add(environment[node.variable]!);
  }

  @override
  void visitVariableSet(VariableSet node) {
    final value = visitForValue(node.value);
    (ops[environment[node.variable]!] as Merge).inputs.add(value);
    stack.add(value);
  }

  @override
  void visitBlock(Block node) {
    for (var stmt in node.statements) {
      stmt.accept(this);
    }
  }

  void merge(Map<VariableDeclaration, int> other) {
    for (var decl in other.keys) {
      if (environment.containsKey(decl) && environment[decl] != other[decl]) {
        environment[decl] =
            addOp(Merge(inputs: [environment[decl]!, other[decl]!]));
      }
    }
  }

  @override
  void visitIfStatement(IfStatement node) {
    visitForValue(node.condition);

    node.then.accept(this);
    node.otherwise?.accept(this);
  }

  @override
  void visitSwitchStatement(SwitchStatement node) {
    visitForValue(node.expression);
    for (var c in node.cases) {
      for (var e in c.expressions) {
        visitForValue(e);
      }
      c.body.accept(this);
    }
  }

  @override
  void visitWhileStatement(WhileStatement node) {
    visitForValue(node.condition);
    node.body.accept(this);
  }

  @override
  void visitDoStatement(DoStatement node) {
    node.body.accept(this);
    visitForValue(node.condition);
  }

  @override
  void visitForStatement(ForStatement node) {
    for (var decl in node.variables) {
      decl.accept(this);
    }

    if (node.condition case final cond?) {
      visitForValue(cond);
    }

    node.body.accept(this);

    for (var upd in node.updates) {
      visitForValue(upd);
    }
  }

  @override
  void visitTryCatch(TryCatch node) {
    node.body.accept(this);
    node.catches.forEach((katch) {
      katch.exception?.accept(this);
      katch.stackTrace?.accept(this);
      katch.body.accept(this);
      if (katch.exception case final v?) environment.remove(v);
      if (katch.stackTrace case final v?) environment.remove(v);
    });
  }

  @override
  void visitTryFinally(TryFinally node) {
    node.body.accept(this);
    node.finalizer.accept(this);
  }

  @override
  void visitLet(Let let) {
    let.variable.accept(this);
    let.body.accept(this);
    environment.remove(let.variable);
  }

  @override
  void visitBlockExpression(BlockExpression node) {
    node.body.accept(this);
    node.value.accept(this);
  }

  @override
  void visitLabeledStatement(LabeledStatement node) {
    node.body.accept(this);
  }

  @override
  void visitBreakStatement(BreakStatement node) {}

  @override
  void visitAwaitExpression(AwaitExpression node) {
    final value = visitForValue(node.operand);
    addOp(Escape(inputs: [value]));
    pushUnknown();
  }

  @override
  void visitYieldStatement(YieldStatement node) {
    final value = visitForValue(node.expression);
    addOp(Escape(inputs: [value]));
  }

  @override
  void visitNot(Not node) {
    node.operand.accept(this);
  }

  @override
  void visitLogicalExpression(LogicalExpression node) {
    visitForValue(node.left);
    visitForValue(node.right);
    pushUnknown();
  }

  @override
  void visitConstantExpression(ConstantExpression node) {
    pushUnknown();
  }

  @override
  void visitStringConcatenation(StringConcatenation node) {
    pushOp(Escape(
      inputs: [
        for (var expr in node.expressions) visitForValue(expr),
      ],
    ));
    pushUnknown();
  }

  @override
  void visitMapLiteral(MapLiteral node) {
    final values = <int>[];
    for (var entry in node.entries) {
      values.add(visitForValue(entry.key));
      values.add(visitForValue(entry.value));
    }
    pushOp(Escape(inputs: values));
    pushUnknown();
  }

  @override
  void visitSetLiteral(SetLiteral node) {
    final values = <int>[];
    for (var entry in node.expressions) {
      values.add(visitForValue(entry));
    }
    pushOp(Escape(inputs: values));
    pushUnknown();
  }

  @override
  void visitRecordLiteral(RecordLiteral node) {
    final values = <int>[];
    for (var entry in node.positional) {
      values.add(visitForValue(entry));
    }
    for (var entry in node.named) {
      values.add(visitForValue(entry.value));
    }
    pushOp(Escape(inputs: values));
    pushUnknown();
  }

  @override
  void visitListLiteral(ListLiteral node) {
    final values = <int>[];
    for (var entry in node.expressions) {
      values.add(visitForValue(entry));
    }
    pushOp(Escape(inputs: values));
    pushUnknown();
  }

  @override
  void visitStringLiteral(StringLiteral node) {
    pushUnknown();
  }

  @override
  void visitTypeLiteral(TypeLiteral node) {
    pushUnknown();
  }

  @override
  void visitDoubleLiteral(DoubleLiteral node) {
    pushUnknown();
  }

  @override
  void visitIntLiteral(IntLiteral node) {
    pushUnknown();
  }

  @override
  void visitBoolLiteral(BoolLiteral node) {
    pushUnknown();
  }

  @override
  void visitNullLiteral(NullLiteral node) {
    pushUnknown();
  }

  @override
  void visitStaticGet(StaticGet node) {
    pushUnknown();
  }

  @override
  void visitStaticSet(StaticSet node) {
    final value = visitForValue(node.value);
    addOp(Escape(inputs: [value]));
    stack.add(value);
  }

  @override
  void visitInstanceTearOff(InstanceTearOff node) {
    final receiver = visitForValue(node.receiver);
    addOp(Escape(inputs: [receiver]));
    pushUnknown();
  }

  tfa.Call? getCallFor(TreeNode callNode) {
    return tfaSummaryCollector.callSites[callNode];
  }

  int unaryCall(
    Selector callSelector,
    TreeNode arg0, {
    TreeNode? callNode,
  }) {
    final arg0Value = visitForValue(arg0);
    return pushOp(Invocation(callSelector,
        inputs: [arg0Value],
        selector: InvocationSelector(1),
        callNode: callNode));
  }

  int binaryCall(
    Selector callSelector,
    TreeNode arg0,
    TreeNode arg1, {
    TreeNode? callNode,
  }) {
    final arg0Value = visitForValue(arg0);
    final arg1Value = visitForValue(arg1);
    return pushOp(Invocation(
      callSelector,
      inputs: [arg0Value, arg1Value],
      selector: InvocationSelector(2),
      callNode: callNode,
    ));
  }

  bool interesting = false;
  void trace(String v) {
    if (interesting) {
      print(v);
    }
  }

  int callWithArguments(
    Selector callSelector,
    Arguments args, {
    TreeNode? receiver,
    int? receiverValue,
    TreeNode? callNode,
  }) {
    if (receiver != null) {
      receiverValue = visitForValue(receiver);
    }
    final (inputs, selector) =
        translateArguments(args, implicitReceiver: receiverValue);
    return pushOp(Invocation(
      callSelector,
      inputs: inputs,
      selector: selector,
      callNode: callNode,
    ));
  }

  @override
  void visitRecordIndexGet(RecordIndexGet node) {
    visitForValue(node.receiver);
    pushUnknown();
  }

  @override
  void visitRecordNameGet(RecordNameGet node) {
    visitForValue(node.receiver);
    pushUnknown();
  }

  @override
  void visitInstanceGet(InstanceGet node) {
    unaryCall(
        InterfaceSelector(node.interfaceTarget, callKind: CallKind.PropertyGet),
        node.receiver,
        callNode: node);
  }

  @override
  void visitInstanceSet(InstanceSet node) {
    binaryCall(
        InterfaceSelector(node.interfaceTarget, callKind: CallKind.PropertySet),
        node.receiver,
        node.value,
        callNode: node);
  }

  @override
  void visitExpressionStatement(ExpressionStatement node) {
    visitForValue(node.expression);
  }

  void pushUnknown() {
    stack.add(-1);
  }

  @override
  void visitFunctionExpression(FunctionExpression node) {
    // We take the body of the closure and simply inline it into the current
    // summary.
    visitFunctionNode(node.function);

    final inputs = [
      for (var v in scopeBuilder.scopes[node.function]!.captured)
        environment[v]!
    ];
    pushOp(Allocation(node.function, inputs: inputs));
  }

  @override
  void visitFunctionDeclaration(FunctionDeclaration node) {
    final inputs = [
      for (var v in scopeBuilder.scopes[node.function]!.captured)
        if (v == node.variable) -1 else environment[v]!
    ];
    pushOp(Allocation(node.function, inputs: inputs));
    environment[node.variable] = stack.removeLast();
    visitFunctionNode(node.function);
  }

  (List<int>, InvocationSelector) translateArguments(Arguments arguments,
      {int? implicitReceiver}) {
    final inputs = <int>[];
    if (implicitReceiver != null) inputs.add(implicitReceiver);
    for (var arg in arguments.positional) {
      inputs.add(visitForValue(arg));
    }

    final numPositional = inputs.length;

    var names = const <String>[];
    if (arguments.named.isNotEmpty) {
      names = <String>[];
      for (var arg in arguments.named) {
        inputs.add(visitForValue(arg.value));
        names.add(arg.name);
      }
    }

    return (inputs, InvocationSelector(numPositional, names: names));
  }

  @override
  void visitNullCheck(NullCheck node) {
    node.operand.accept(this);
  }

  @override
  void visitEqualsNull(EqualsNull node) {
    visitForValue(node.expression);
    pushUnknown();
  }

  @override
  void visitEqualsCall(EqualsCall node) {
    binaryCall(InterfaceSelector(node.interfaceTarget), node.left, node.right,
        callNode: node);
  }

  @override
  void visitIsExpression(IsExpression node) {
    visitForValue(node.operand);
    pushUnknown();
  }

  @override
  void visitAsExpression(AsExpression node) {
    visitForValue(node.operand);
    pushUnknown();
  }

  @override
  void visitConditionalExpression(ConditionalExpression node) {
    visitForValue(node.condition);
    final thenValue = visitForValue(node.then);
    final otherwiseValue = visitForValue(node.otherwise);
    pushOp(Merge(inputs: [thenValue, otherwiseValue]));
  }

  int addOp(Op op) {
    final id = ops.length;
    ops.add(op);
    return id;
  }

  int pushOp(Op op) {
    final id = addOp(op);
    stack.add(id);
    return id;
  }

  @override
  void visitDynamicInvocation(DynamicInvocation node) {
    callWithArguments(
      DynamicSelector(CallKind.Method, node.name),
      node.arguments,
    );
  }

  @override
  void visitDynamicGet(DynamicGet node) {
    unaryCall(
      DynamicSelector(CallKind.PropertyGet, node.name),
      node.receiver,
    );
  }

  @override
  void visitDynamicSet(DynamicSet node) {
    binaryCall(DynamicSelector(CallKind.PropertySet, node.name), node.receiver,
        node.value);
  }

  @override
  void visitStaticInvocation(StaticInvocation node) {
    callWithArguments(DirectSelector(node.target), node.arguments);
    if (node.target.isFactory) {
      stack.removeLast();
      pushOp(Allocation(node, inputs: const <int>[]));
    }
  }

  @override
  void visitInstanceInvocation(InstanceInvocation node) {
    callWithArguments(
      InterfaceSelector(node.interfaceTarget),
      node.arguments,
      receiver: node.receiver,
      callNode: node,
    );
  }

  @override
  void visitSuperPropertyGet(SuperPropertyGet node) {
    unaryCall(
      DirectSelector(node.interfaceTarget, callKind: CallKind.PropertyGet),
      ThisExpression(),
    );
  }

  @override
  void visitSuperPropertySet(SuperPropertySet node) {
    binaryCall(
      DirectSelector(node.interfaceTarget, callKind: CallKind.PropertySet),
      ThisExpression(),
      node.value,
    );
  }

  @override
  void visitSuperMethodInvocation(SuperMethodInvocation node) {
    callWithArguments(
      DirectSelector(node.interfaceTarget),
      node.arguments,
      receiver: ThisExpression(),
    );
  }

  @override
  void visitConstructorInvocation(ConstructorInvocation node) {
    final alloc = pushOp(Allocation(node, inputs: const <int>[]));
    callWithArguments(
      DirectSelector(node.target),
      node.arguments,
      receiverValue: alloc,
    );
    stack.add(alloc);
  }

  @override
  void visitFunctionInvocation(FunctionInvocation node) {
    final functionValue = visitForValue(node.receiver);
    callWithArguments(
      FunctionSelector(tfa.emptyType),
      node.arguments,
      receiverValue: functionValue,
    );
  }

  @override
  void visitLocalFunctionInvocation(LocalFunctionInvocation node) {
    final functionValue = environment[node.variable]!;
    callWithArguments(
      FunctionSelector(tfa.emptyType),
      node.arguments,
      receiverValue: functionValue,
    );
  }

  @override
  void visitReturnStatement(ReturnStatement node) {
    if (node.expression case final expr?) {
      final value = visitForValue(expr);
      ops.add(Escape(inputs: [value]));
    }
  }

  @override
  void visitThrow(Throw node) {
    final value = visitForValue(node.expression);
    ops.add(Escape(inputs: [value]));
    pushUnknown();
  }

  @override
  void visitRethrow(Rethrow node) {
    // Nothing to do.
    pushUnknown();
  }

  @override
  void visitEmptyStatement(EmptyStatement node) {}

  int visitForValue(Node node) {
    node.accept(this);
    if (stack.isEmpty) {
      throw 'Expression: $node (${node.runtimeType}) did not produce any value';
    }
    return stack.removeLast();
  }

  @override
  void visitRedirectingInitializer(RedirectingInitializer node) {
    callWithArguments(
      DirectSelector(node.target),
      node.arguments,
      receiver: ThisExpression(),
    );
  }

  @override
  void visitLocalInitializer(LocalInitializer node) {
    node.variable.accept(this);
  }

  @override
  void visitFieldInitializer(FieldInitializer node) {
    final value = visitForValue(node.value);
    ops.add(Escape(inputs: [value]));
  }

  @override
  void visitSuperInitializer(SuperInitializer node) {
    callWithArguments(
      DirectSelector(node.target),
      node.arguments,
      receiver: ThisExpression(),
    );
  }

  @override
  void visitFunctionNode(FunctionNode node) {
    if (scopeBuilder.scopes.isEmpty) {
      interesting = isInterestingMember(node.parent);
      scopeBuilder.buildScopes(node);
    }

    // We are entering the given node. We assume the scopes are already built.
    final currentScope = scopeBuilder.scopes[node]!;

    // Handle parameters.

    final params = <(VariableDeclaration, int)>[];
    void createParam(VariableDeclaration param, {String? name}) {
      int val = -1;
      if (currentScope.parent == null) {
        val = pushOp(Parameter(ops.length, name: name)
          ..escapes = currentScope.alwaysEscapes(param));
      }
      params.add((param, val));
    }

    if (currentScope.parent == null) {
      final parent = node.parent as Member;
      if (parent.isInstanceMember || parent is Constructor) {
        createParam(scopeBuilder.this$);
      }
    }

    for (var param in node.positionalParameters) {
      createParam(param);
    }

    for (var param in node.namedParameters) {
      createParam(param, name: param.name);
    }

    if (currentScope.parent == null) {
      numParameters = ops.length;
    }

    for (var (v, idx) in params) {
      environment[v] = pushOp(Merge(inputs: [
        if (idx != -1) idx,
      ]));
    }

    if (node.parent case Constructor(:final initializers)) {
      for (var initializer in initializers) {
        initializer.accept(this);
      }
    }

    node.body?.accept(this);
  }

  void defaultNode(Node node) {
    throw 'Unsupported node: ${node.runtimeType}';
  }
}

class Summary {
  final Member? member;
  final List<Op> ops;
  final int numParameters;

  final Set<Summary> callers = {};

  bool processed = false;
  bool inWorklist = false;

  Summary._(this.member, this.ops, this.numParameters);

  @override
  String toString() {
    return ops.indexed.map((v) => '${v.$1}: ${v.$2}').join('\n');
  }

  void markEscaping(int v, EscapeAnalysis analysis) {
    analysis.trace('        $v escapes');
    if (v == -1) {
      return;
    }

    final op = ops[v];
    switch (op) {
      case Merge(escaped: false) && final m:
        m.escaped = true;
        for (var u in m.inputs) {
          markEscaping(u, analysis);
        }

      case Parameter(escapes: false) && final p:
        p.escapes = true;
        analysis.invalidate(callers);

      case Allocation(escapes: false) && final a:
        a.escapes = true;
        for (var u in a.inputs) {
          markEscaping(u, analysis);
        }

      default:
        return;
    }
  }
}

final summaries = <Summary>[];

final interestingMembers = <String>{
  'SkwasmPaint.isAntiAlias',
  'TwentyThree._drawBox',
  'Canvas.drawRect',
  'SkwasmCanvas.drawRect',
};

bool isInterestingMember(TreeNode? m) {
  return m is Member &&
      (m.enclosingLibrary.importUri.path.endsWith('test.dart') ||
          interestingMembers
              .contains('${m.enclosingClass?.name}.${m.name.text}'));
}

Summary summarize(tfa.SummaryCollector tfaSummaryCollector, FunctionNode node) {
  final visitor = _SummaryBuilder(tfaSummaryCollector: tfaSummaryCollector);
  node.accept(visitor);

  final bool xxx = isInterestingMember(node.parent);

  final nop = Merge(inputs: [-1]);

  final ops = visitor.ops;
  if (xxx) {
    print("${node.parent}:");
    for (var (i, op) in ops.indexed) {
      print("$i: $op");
    }
    print("");
  }

  final escaped = List<bool>.filled(ops.length, false);
  final interesting = List<bool>.filled(ops.length + 1, false);
  void markEscaped(int v) {
    if (v == -1) {
      return;
    }
    if (escaped[v]) {
      return;
    }
    escaped[v] = true;
    if (ops[v] case Merge(:final inputs)) {
      for (var u in inputs) {
        markEscaped(u);
      }
    } else if (ops[v] case final Parameter p) {
      p.escapes = true;
    } else if (ops[v] case final Allocation a) {
      a.escapes = true;
      for (var u in a.inputs) {
        markEscaped(u);
      }
    }
  }

  for (var op in ops) {
    if (op case Escape(:final inputs)) {
      for (var v in inputs) {
        markEscaped(v);
      }
    }
  }

  bool isInteresting(int v) => interesting[v + 1];
  void markInteresting(int v) {
    interesting[v + 1] = true;
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (var i = 0; i < ops.length; i++) {
      final op = ops[i];
      if (isInteresting(i)) continue;

      switch (op) {
        case Merge(:final inputs):
          for (var v in inputs) {
            if (isInteresting(v)) {
              markInteresting(i);
              changed = true;
              break;
            }
          }

        case Parameter(escapes: false):
          markInteresting(i);
          changed = true;
          break;

        case Allocation(escapes: false):
          markInteresting(i);
          changed = true;
          break;

        default:
      }
    }
  }

  for (var i = 0; i < ops.length; i++) {
    if (isInteresting(i)) continue;

    final op = ops[i];
    switch (op) {
      case Merge():
        ops[i] = nop;

      case Invocation(:final inputs):
        if (!inputs.any(isInteresting)) {
          ops[i] = nop;
        }

      case Escape(:final inputs):
        if (!inputs.any(isInteresting)) {
          ops[i] = nop;
        }

      case Allocation(escapes: true):
        ops[i] = nop;

      default:
    }
  }

  changed = true;
  while (changed) {
    changed = false;
    for (var i = 0; i < ops.length; i++) {
      final op = ops[i];
      if (op.isTrivial(ops)) {
        ops[i] = nop;
        changed = true;
      } else if (op case Merge(:final inputs)) {
        var j = 0;
        for (var i = 0; i < inputs.length; i++) {
          if (inputs[i] == -1) {
            continue;
          }
          inputs[j++] = inputs[i];
        }
        if (j != inputs.length) {
          changed = true;
        }
        inputs.length = j;
      }
    }

    // Now compact the ops.
    final renaming = List<int>.generate(ops.length + 1, (index) => index - 1);
    var j = 0;
    for (var i = 0; i < ops.length; i++) {
      final op = ops[i];
      if (op == nop) {
        renaming[i + 1] = -1;
        continue;
      }
      if (op case Merge(inputs: [final v])) {
        renaming[i + 1] = -v - 1;
        continue;
      }
      if (i != j) {
        renaming[i + 1] = j;
      }
      ops[j++] = op;
    }
    for (var i = 1; i < renaming.length; i++) {
      while (renaming[i] < -1) {
        renaming[i] = renaming[-renaming[i]];
      }
    }
    if (j < ops.length) {
      for (var i = 0; i < j; i++) {
        ops[i].rename(renaming);
      }
      ops.length = j;
      changed = true;
    }
  }

  final summary = Summary._(node.parent as Member, ops, visitor.numParameters);

  if (ops.any((v) => v is Allocation && !v.escapes)) {
    summaries.add(summary);
  }

  if (xxx) {
    print("COMPACTED ${node.parent}:");
    for (var (i, op) in ops.indexed) {
      print("$i: $op");
    }
    print("");
  }

  return summary;
}

class EscapeAnalysis {
  final tfa.TypeFlowAnalysis analysis;

  EscapeAnalysis(this.analysis);

  final worklist = <Summary>[];

  void invalidate(Iterable<Summary> summaries) {
    for (var summary in summaries) {
      if (!summary.inWorklist) {
        worklist.add(summary);
        summary.inWorklist = true;
      }
    }
  }

  void handleDirectInvocation(Invocation invocation, Member target) {
    trace(
        '    -> call of $target [${target.runtimeType}] [${invocation.callSelector}]');
    if (target is Field) {
      if (target.isLate) {
        handleUnknownInvocation(invocation);
      }
      if (invocation.callSelector.isSetter) {
        summary.markEscaping(invocation.inputs[1], this);
      }
      return; // Accessing field should not do anything.
    }
    final tfaSummary = analysis.tryGetSummary(target);
    if (tfaSummary == null) {
      // Not reachable member.
      return;
    }
    if (tfaSummary.escapeSummary case final escapeSummary?) {
      final inputs = invocation.inputs;
      final selector = invocation.selector;
      escapeSummary.callers.add(summary);
      if (!escapeSummary.processed && !escapeSummary.inWorklist) {
        worklist.add(escapeSummary);
        escapeSummary.inWorklist = true;
      }

      for (var i = 0; i < escapeSummary.numParameters; i++) {
        final p = escapeSummary.ops[i] as Parameter;
        trace('      | $p');
        if (p.escapes) {
          if (p.name case final name?) {
            summary.markEscaping(selector.getNamed(inputs, name), this);
          } else if (p.index < selector.numPositional) {
            // Positional parameter.
            summary.markEscaping(inputs[p.index], this);
          }
        }
      }
    } else {
      handleUnknownInvocation(invocation);
      //throw 'No escape summary for $target';
    }
  }

  void _handleInvocationWithConcreteReceiver(
      tfa.ConcreteType receiver, Invocation invocation) {
    final cls = receiver.cls as dynamic;
    Member? target = cls.getDispatchTarget(invocation.callSelector);
    if (target != null) {
      handleDirectInvocation(invocation, target);
    } else {
      throw 'Failed to resolve: $invocation';
    }
  }

  void handleUnknownInvocation(Invocation op, {bool ignoreReceiver = false}) {
    final inputs = op.inputs;
    for (var i = ignoreReceiver ? 1 : 0; i < inputs.length; i++) {
      final v = inputs[i];
      summary.markEscaping(v, this);
    }
  }

  Summary summary = Summary._(null, [], 0);

  bool interesting = false;

  void trace(String v) {
    if (interesting) {
      print(v);
    }
  }

  void process() {
    interesting = isInterestingMember(summary.member);

    trace('processing summary for ${summary.member}');
    for (var (i, op) in summary.ops.indexed) {
      trace('$i: $op');
      if (op case Invocation(:final callSelector, :final callNode)) {
        final call = callNode != null ? analysis.callSite(callNode) : null;
        if (call != null &&
            call.isMonomorphic &&
            call.monomorphicTarget != null) {
          if (callSelector is FunctionSelector) throw 'what?';
          handleDirectInvocation(op, call.monomorphicTarget!);
        } else {
          final selector = callSelector;
          switch (selector) {
            case DirectSelector(:final member):
              handleDirectInvocation(op, member);

            case InterfaceSelector(:final member):
              final receiverCls =
                  analysis.hierarchyCache.getTFClass(member.enclosingClass!);
              final receiver = analysis.hierarchyCache
                  .specializeTypeCone(receiverCls, allowWideCone: false);

              if (receiver is tfa.ConcreteType) {
                _handleInvocationWithConcreteReceiver(receiver, op);
              } else if (receiver is tfa.SetType) {
                for (var type in receiver.types) {
                  _handleInvocationWithConcreteReceiver(type, op);
                }
              } else if (receiver is tfa.AnyInstanceType) {
                handleUnknownInvocation(op);
              } else {
                assert(receiver is tfa.EmptyType);
              }

            case FunctionSelector():
              handleUnknownInvocation(op, ignoreReceiver: true);

            case DynamicSelector():
              handleUnknownInvocation(op);
          }
        }
      }
    }
  }

  void processSummaries() {
    invalidate(summaries);

    while (worklist.isNotEmpty) {
      summary = worklist.removeLast();
      summary.inWorklist = false;
      process();
      summary.processed = true;
    }

    for (var summary in summaries) {
      bool interesting = false;

      for (var op in summary.ops) {
        if (op case Allocation(escapes: false)) {
          interesting = true;
          break;
        }
      }

      interesting = isInterestingMember(summary.member);

      if (interesting) {
        print('${summary.member}');
        for (var (i, op) in summary.ops.indexed) {
          print('   $i: $op');
        }
        print('');
      }
    }
  }
}

void analyse(tfa.TypeFlowAnalysis analysis) {
  EscapeAnalysis(analysis).processSummaries();
}
