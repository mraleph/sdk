import 'dart:collection';

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
  const Op();

  @override
  String toString() {
    return '${runtimeType}($operandsStr)';
  }

  String get operandsStr;

  bool isTrivial(List<Op> ops);

  void rename(List<int> renames);
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

int _renameOne(int value, List<int> renames) {
  return renames[value + 1];
}

void _rename(List<int> inputs, List<int> renames) {
  for (var i = 0; i < inputs.length; i++) {
    inputs[i] = _renameOne(inputs[i], renames);
  }
}

final class Parameter extends Op {
  final int index;
  final String? name;

  bool escapes = false;

  Parameter(this.index, {this.name});

  @override
  String get operandsStr =>
      '$index' + (name != null ? ', $name' : '') + (escapes ? ", escapes" : "");

  @override
  bool isTrivial(List<Op> ops) {
    return false;
  }

  @override
  void rename(List<int> renames) {
    // Nothing to do
  }
}

final class Merge extends Op {
  bool escaped = false;
  final List<int> inputs;

  Merge(this.inputs);

  bool isTrivial(List<Op> ops) {
    return _deref(inputs, ops);
  }

  @override
  void rename(List<int> renames) {
    _rename(inputs, renames);
  }

  @override
  String get operandsStr => inputs.join(', ');
}

final class InvocationArguments {
  final List<int> positional;
  final List<int> named;
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

  InvocationArguments(
    this.positional, {
    this.named = const [],
    this.names = const [],
  });

  bool isTrivial(List<Op> ops) {
    return _deref(positional, ops) & _deref(named, ops);
  }

  @override
  String toString() {
    return positional.join(', ') +
        (named.isNotEmpty ? ', ' : '') +
        [for (var i = 0; i < named.length; i++) '${names[i]}: ${named[i]}']
            .join(', ');
  }

  int getNamed(String name) {
    final idx = _descriptor[name];
    if (idx == null) {
      return -1;
    }
    return named[idx];
  }
}

final class Invocation extends Op {
  final tfa.Call call;
  final InvocationArguments args;
  Invocation(this.call, this.args);

  @override
  String get operandsStr => '$call; ' + args.toString();

  @override
  bool isTrivial(List<Op> ops) {
    return args.isTrivial(ops);
  }

  @override
  void rename(List<int> renames) {
    _rename(args.positional, renames);
    _rename(args.named, renames);
  }
}

final class Escape extends Op {
  /* int | List<int> */ Object value;

  Escape(this.value);

  bool isTrivial(List<Op> ops) {
    if (value is List<int>) {
      return _deref(value as List<int>, ops);
    } else {
      value = _derefArg(value as int, ops);
      return value == -1;
    }
  }

  @override
  void rename(List<int> renames) {
    if (value is List<int>) {
      _rename(value as List<int>, renames);
    } else {
      value = _renameOne(value as int, renames);
    }
  }

  @override
  String get operandsStr => '$value';
}

final class Allocation extends Op {
  TreeNode node;
  bool escapes = false;

  Allocation(this.node);

  @override
  String get operandsStr => escapes ? 'escapes' : '';

  @override
  bool isTrivial(List<Op> ops) {
    return false;
  }

  @override
  void rename(List<int> renames) {}
}

class _VariableCollector extends RecursiveVisitor {
  int depth = -1;
  bool hasAwait = false;
  bool _capturedThis = false;
  final captured = <VariableDeclaration, bool>{};

  @override
  void visitVariableDeclaration(VariableDeclaration node) {
    super.visitVariableDeclaration(node);
    if (depth == 0) {
      captured[node] = false;
    }
  }

  @override
  void visitAwaitExpression(AwaitExpression node) {
    hasAwait = true;
  }

  @override
  void visitThisExpression(ThisExpression node) {
    super.visitThisExpression(node);
    if (depth > 0) {
      _capturedThis = true;
    }
  }

  @override
  void visitVariableGet(VariableGet node) {
    super.visitVariableGet(node);
    if (depth > 0 && captured.containsKey(node.variable)) {
      captured[node.variable] = true;
    }
  }

  @override
  void visitFunctionNode(FunctionNode node) {
    depth++;
    if (node.parent case final Constructor ctor) {
      for (var initializer in ctor.initializers) {
        initializer.accept(this);
      }
    }
    super.visitFunctionNode(node);
    depth--;
  }

  bool capturesVar(VariableDeclaration node) {
    return hasAwait || captured[node]!;
  }

  bool get capturesThis => hasAwait || _capturedThis;
}

class _SummaryBuilder extends RecursiveVisitor {
  final collector = _VariableCollector();

  final tfa.SummaryCollector tfaSummaryCollector;

  int numParameters = 0;
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
    environment[node] = pushOp(Merge(<int>[]));
    if (node.initializer case final initializer?) {
      (ops[environment[node]!] as Merge).inputs.add(visitForValue(initializer));
    }
    if (collector.capturesVar(node)) {
      pushOp(Escape(environment[node]!));
    }
  }

  @override
  void visitVariableGet(VariableGet node) {
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
        environment[decl] = addOp(Merge([environment[decl]!, other[decl]!]));
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
    addOp(Escape(value));
    stack.add(-1);
  }

  @override
  void visitNot(Not node) {
    node.operand.accept(this);
  }

  @override
  void visitLogicalExpression(LogicalExpression node) {
    visitForValue(node.left);
    visitForValue(node.right);
    stack.add(-1);
  }

  @override
  void visitConstantExpression(ConstantExpression node) {
    stack.add(-1);
  }

  @override
  void visitStringConcatenation(StringConcatenation node) {
    final values = [for (var expr in node.expressions) visitForValue(expr)];
    pushOp(Escape(values));
    stack.add(-1);
  }

  @override
  void visitMapLiteral(MapLiteral node) {
    final values = <int>[];
    for (var entry in node.entries) {
      values.add(visitForValue(entry.key));
      values.add(visitForValue(entry.value));
    }
    pushOp(Escape(values));
    stack.add(-1);
  }

  @override
  void visitListLiteral(ListLiteral node) {
    final values = <int>[];
    for (var entry in node.expressions) {
      values.add(visitForValue(entry));
    }
    pushOp(Escape(values));
    stack.add(-1);
  }

  @override
  void visitStringLiteral(StringLiteral node) {
    stack.add(-1);
  }

  @override
  void visitTypeLiteral(TypeLiteral node) {
    stack.add(-1);
  }

  @override
  void visitDoubleLiteral(DoubleLiteral node) {
    stack.add(-1);
  }

  @override
  void visitIntLiteral(IntLiteral node) {
    stack.add(-1);
  }

  @override
  void visitBoolLiteral(BoolLiteral node) {
    stack.add(-1);
  }

  @override
  void visitNullLiteral(NullLiteral node) {
    stack.add(-1);
  }

  @override
  void visitStaticGet(StaticGet node) {
    stack.add(-1);
  }

  @override
  void visitStaticSet(StaticSet node) {
    final value = visitForValue(node.value);
    addOp(Escape(value));
    stack.add(value);
  }

  @override
  void visitInstanceTearOff(InstanceTearOff node) {
    final receiver = visitForValue(node.receiver);
    addOp(Escape(receiver));
    stack.add(-1);
  }

  tfa.Call? getCallFor(TreeNode callNode) {
    return tfaSummaryCollector.callSites[callNode];
  }

  int unaryCall(TreeNode callNode, TreeNode arg0) {
    final arg0Value = visitForValue(arg0);
    final call = getCallFor(callNode);
    if (call == null) {
      return pushOp(Escape(arg0Value));
    }
    return pushOp(Invocation(call, InvocationArguments([arg0Value])));
  }

  int binaryCall(TreeNode callNode, TreeNode arg0, TreeNode arg1) {
    final arg0Value = visitForValue(arg0);
    final arg1Value = visitForValue(arg1);
    final call = getCallFor(callNode);
    if (call == null) {
      return pushOp(Escape([arg0Value, arg1Value]));
    }
    return pushOp(
        Invocation(call, InvocationArguments([arg0Value, arg1Value])));
  }

  int callWithArguments(TreeNode callNode, Arguments args,
      {TreeNode? receiver, int? receiverValue}) {
    if (receiver != null) {
      receiverValue = visitForValue(receiver);
    }
    final argValues = translateArguments(args, implicitReceiver: receiverValue);
    final call = getCallFor(callNode);
    if (call == null) {
      return pushOp(Escape([...argValues.positional, ...argValues.named]));
    }
    return pushOp(Invocation(call, argValues));
  }

  @override
  void visitInstanceGet(InstanceGet node) {
    unaryCall(node, node.receiver);
  }

  @override
  void visitInstanceSet(InstanceSet node) {
    binaryCall(node, node.receiver, node.value);
  }

  @override
  void visitExpressionStatement(ExpressionStatement node) {
    visitForValue(node.expression);
  }

  @override
  void visitFunctionExpression(FunctionExpression node) {
    // TODO(XXX) just mark all captured variables as escaping.
    stack.add(-1);
  }

  @override
  void visitFunctionDeclaration(FunctionDeclaration node) {
    environment[node.variable] = -1;
  }

  InvocationArguments translateArguments(Arguments arguments,
      {int? implicitReceiver}) {
    final positional = <int>[];
    if (implicitReceiver != null) positional.add(implicitReceiver);
    for (var arg in arguments.positional) {
      positional.add(visitForValue(arg));
    }

    var named = const <int>[], names = <String>[];
    if (arguments.named.isNotEmpty) {
      named = <int>[];
      names = <String>[];
      for (var arg in arguments.named) {
        named.add(visitForValue(arg.value));
        names.add(arg.name);
      }
    }

    return InvocationArguments(positional, named: named, names: names);
  }

  @override
  void visitNullCheck(NullCheck node) {
    node.operand.accept(this);
  }

  @override
  void visitEqualsNull(EqualsNull node) {
    visitForValue(node.expression);
    stack.add(-1);
  }

  @override
  void visitEqualsCall(EqualsCall node) {
    binaryCall(node, node.left, node.right);
  }

  @override
  void visitIsExpression(IsExpression node) {
    visitForValue(node.operand);
    stack.add(-1);
  }

  @override
  void visitAsExpression(AsExpression node) {
    visitForValue(node.operand);
    stack.add(-1);
  }

  @override
  void visitConditionalExpression(ConditionalExpression node) {
    visitForValue(node.condition);
    final thenValue = visitForValue(node.then);
    final otherwiseValue = visitForValue(node.otherwise);
    pushOp(Merge([thenValue, otherwiseValue]));
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
    callWithArguments(node, node.arguments);
  }

  @override
  void visitDynamicGet(DynamicGet node) {
    unaryCall(node, node.receiver);
  }

  @override
  void visitDynamicSet(DynamicSet node) {
    binaryCall(node, node.receiver, node.value);
  }

  @override
  void visitStaticInvocation(StaticInvocation node) {
    callWithArguments(node, node.arguments);
  }

  @override
  void visitInstanceInvocation(InstanceInvocation node) {
    callWithArguments(node, node.arguments, receiver: node.receiver);
  }

  @override
  void visitSuperPropertyGet(SuperPropertyGet node) {
    unaryCall(node, ThisExpression());
  }

  @override
  void visitSuperPropertySet(SuperPropertySet node) {
    binaryCall(node, ThisExpression(), node.value);
  }

  @override
  void visitSuperMethodInvocation(SuperMethodInvocation node) {
    callWithArguments(node, node.arguments, receiver: ThisExpression());
  }

  @override
  void visitConstructorInvocation(ConstructorInvocation node) {
    final alloc = pushOp(Allocation(node));
    callWithArguments(node, node.arguments, receiverValue: alloc);
    stack.add(alloc);
  }

  @override
  void visitFunctionInvocation(FunctionInvocation node) {
    callWithArguments(node, node.arguments);
  }

  @override
  void visitLocalFunctionInvocation(LocalFunctionInvocation node) {
    callWithArguments(node, node.arguments);
  }

  @override
  void visitReturnStatement(ReturnStatement node) {
    if (node.expression case final expr?) {
      final value = visitForValue(expr);
      ops.add(Escape(value));
    }
  }

  @override
  void visitThrow(Throw node) {
    final value = visitForValue(node.expression);
    ops.add(Escape(value));
    stack.add(-1);
  }

  @override
  void visitRethrow(Rethrow node) {
    // Nothing to do.
    stack.add(-1);
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
    callWithArguments(node, node.arguments, receiver: ThisExpression());
  }

  @override
  void visitLocalInitializer(LocalInitializer node) {
    node.variable.accept(this);
  }

  @override
  void visitFieldInitializer(FieldInitializer node) {
    final value = visitForValue(node.value);
    ops.add(Escape(
        value)); // TODO(XXX): tie escape of the value with escape of the receiver.
  }

  @override
  void visitSuperInitializer(SuperInitializer node) {
    callWithArguments(node, node.arguments, receiver: ThisExpression());
  }

  @override
  void visitFunctionNode(FunctionNode node) {
    final parent = node.parent as Member;
    node.accept(collector);

    if (parent.isInstanceMember) {
      ops.add(Parameter(0)..escapes = collector.capturesThis);
    } else if (parent is Constructor) {
      ops.add(Parameter(0)..escapes = collector.capturesThis);
    }

    final params = <(VariableDeclaration, int)>[];
    for (var param in node.positionalParameters) {
      params.add((
        param,
        pushOp(Parameter(ops.length)..escapes = collector.capturesVar(param))
      ));
    }

    for (var param in node.namedParameters) {
      params.add((
        param,
        pushOp(Parameter(ops.length, name: param.name)
          ..escapes = collector.capturesVar(param))
      ));
    }

    numParameters = ops.length;

    for (var (v, idx) in params) {
      environment[v] = pushOp(Merge([idx]));
    }

    if (parent is Constructor) {
      for (var initializer in parent.initializers) {
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
      case Merge(escaped: false, :final inputs) && final m:
        m.escaped = true;
        for (var u in inputs) {
          markEscaping(u, analysis);
        }
        return;

      case Parameter(escapes: false) && final p:
        p.escapes = true;
        analysis.invalidate(callers);

      case Allocation(escapes: false) && final a:
        a.escapes = true;

      default:
        return;
    }
  }
}

final summaries = <Summary>[];

Summary summarize(tfa.SummaryCollector tfaSummaryCollector, FunctionNode node) {
  final visitor = _SummaryBuilder(tfaSummaryCollector: tfaSummaryCollector);
  node.accept(visitor);

  final nop = Merge([-1]);

  final ops = visitor.ops;

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
    }
  }

  for (var op in ops) {
    if (op case Escape(value: final v)) {
      // Mark all escaping values.
      if (v case final List<int> values) {
        for (var v in values) {
          markEscaped(v);
        }
      } else {
        markEscaped(v as int);
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

      case Invocation(:final args):
        if (!args.positional.any(isInteresting) &&
            !args.named.any(isInteresting)) {
          ops[i] = nop;
        }

      case Escape(:final value):
        if (value is List<int>) {
          if (!value.any(isInteresting)) {
            ops[i] = nop;
          }
        } else {
          if (!isInteresting(value as int)) {
            ops[i] = nop;
          }
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
        '    -> call of $target [${target.runtimeType}] [${invocation.call.selector}]');
    if (target is Field) {
      if (target.isLate) {
        handleUnknownInvocation(invocation);
      }
      if (invocation.call.selector.isSetter) {
        summary.markEscaping(invocation.args.positional[1], this);
      }
      return; // Accessing field should not do anything.
    }
    final tfaSummary = analysis.getSummary(target);
    if (tfaSummary.escapeSummary case final escapeSummary?) {
      final args = invocation.args;
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
            summary.markEscaping(args.getNamed(name), this);
          } else if (p.index < args.positional.length) {
            // Positional parameter.
            summary.markEscaping(args.positional[p.index], this);
          }
        }
      }
    } else {
      throw 'No escape summary';
    }
  }

  void _handleInvocationWithConcreteReceiver(
      tfa.ConcreteType receiver, Invocation invocation) {
    final cls = receiver.cls as dynamic;
    Member? target = cls.getDispatchTarget(invocation.call.selector);
    if (target != null) {
      handleDirectInvocation(invocation, target);
    } else {
      throw 'Failed to resolve: $invocation';
    }
  }

  void handleUnknownInvocation(Invocation op) {
    for (var v in op.args.positional) {
      summary.markEscaping(v, this);
    }
    for (var v in op.args.named) {
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
    interesting = summary.member?.name.text == '_ensureDoneFuture';

    trace('processing summary for ${summary.member}');
    for (var (i, op) in summary.ops.indexed) {
      trace('$i: $op');
      if (op case Invocation(:final call)) {
        if (call.isMonomorphic && call.monomorphicTarget != null) {
          handleDirectInvocation(op, call.monomorphicTarget!);
        } else {
          final selector = call.selector;
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

            case DynamicSelector():
            case FunctionSelector():
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
