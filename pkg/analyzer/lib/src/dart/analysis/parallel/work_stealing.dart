// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:ffi';
import 'dart:math';

import 'package:analyzer/src/dart/analysis/parallel/native.dart';

/// Work stealing queues of task indices, in native memory.
///
/// Each worker owns a deque, and takes tasks from its head. When the deque
/// of a worker is empty, it steals tasks from the tails of other deques.
/// Tasks are not added after creation, so when all deques are empty, the
/// work is done.
///
/// Each deque is protected by its own mutex, so the owner and thieves
/// contend only when they access the same deque.
class WorkStealingQueues {
  /// The size of the header of each deque: the mutex, `head` and `tail`.
  /// Deque headers are on separate cache lines.
  static const int _headerSize = 128;
  static const int _headOffset = mutexSize;
  static const int _tailOffset = mutexSize + 8;

  final int workerCount;
  final int address;

  new _(this.workerCount, this.address);

  /// Creates queues, the [tasks] of the worker `i` are `tasks[i]`, ordered
  /// by priority, from the highest.
  factory allocate(List<List<int>> tasks) {
    var workerCount = tasks.length;
    var taskCount = tasks.fold(0, (sum, list) => sum + list.length);
    var headersSize = workerCount * _headerSize;
    var memory = allocateZeroed(headersSize + taskCount * 4);
    var queues = WorkStealingQueues._(workerCount, memory.address);
    for (var worker = 0; worker < workerCount; worker++) {
      queues._mutex(worker).init();
    }

    var itemsOffset = 0;
    for (var worker = 0; worker < workerCount; worker++) {
      var workerTasks = tasks[worker];
      var items = queues._items;
      for (var i = 0; i < workerTasks.length; i++) {
        items[itemsOffset + i] = workerTasks[i];
      }
      queues._setHead(worker, itemsOffset);
      queues._setTail(worker, itemsOffset + workerTasks.length);
      itemsOffset += workerTasks.length;
    }
    return queues;
  }

  Pointer<Int32> get _items {
    return Pointer<Int32>.fromAddress(address + workerCount * _headerSize);
  }

  void free() {
    freeNative(Pointer<Uint8>.fromAddress(address));
  }

  /// Returns the next task of the [worker], or a task stolen from another
  /// worker, or `null` if there are no tasks left.
  ///
  /// Invokes [onSteal] when the task is stolen.
  int? take(int worker, {void Function(int victim)? onSteal}) {
    if (_takeHead(worker) case var task?) {
      return task;
    }
    for (var i = 1; i < workerCount; i++) {
      var victim = (worker + i) % workerCount;
      if (_takeTail(victim) case var task?) {
        onSteal?.call(victim);
        return task;
      }
    }
    return null;
  }

  int _head(int worker) => _int64At(worker, _headOffset).value;

  Pointer<Int64> _int64At(int worker, int offset) {
    return Pointer<Int64>.fromAddress(address + worker * _headerSize + offset);
  }

  NativeMutex _mutex(int worker) {
    return NativeMutex(Pointer.fromAddress(address + worker * _headerSize));
  }

  void _setHead(int worker, int value) {
    _int64At(worker, _headOffset).value = value;
  }

  void _setTail(int worker, int value) {
    _int64At(worker, _tailOffset).value = value;
  }

  int _tail(int worker) => _int64At(worker, _tailOffset).value;

  int? _takeHead(int worker) {
    var mutex = _mutex(worker);
    mutex.lock();
    try {
      var head = _head(worker);
      if (head < _tail(worker)) {
        _setHead(worker, head + 1);
        return _items[head];
      }
      return null;
    } finally {
      mutex.unlock();
    }
  }

  int? _takeTail(int victim) {
    // Racy check, avoids locking deques that are already empty.
    if (_head(victim) >= _tail(victim)) {
      return null;
    }
    var mutex = _mutex(victim);
    mutex.lock();
    try {
      var tail = _tail(victim);
      if (_head(victim) < tail) {
        _setTail(victim, tail - 1);
        return _items[tail - 1];
      }
      return null;
    } finally {
      mutex.unlock();
    }
  }
}

/// Work stealing queues of tasks with dependencies, in native memory.
///
/// A task becomes ready when all tasks that it depends on are completed.
/// Each worker owns a heap of ready tasks ordered by priority. Tasks that
/// become ready when a worker completes a task are added to the heap of this
/// worker. A worker takes the task with the highest priority from its heap,
/// and when its heap is empty, steals the task with the highest priority from
/// heaps of other workers. When there are no ready tasks, but not all tasks
/// are completed yet, workers wait.
///
/// Each heap is protected by its own mutex. The global mutex protects the
/// counters of pending dependencies, and the condition to wait on.
class DependencyQueues {
  static const int _conditionOffset = mutexSize;
  static const int _completedCountOffset = mutexSize + conditionSize;
  static const int _isAbortedOffset = _completedCountOffset + 8;
  static const int _versionOffset = _isAbortedOffset + 8;
  static const int _completedLeadingCountOffset = _versionOffset + 8;
  static const int _globalSize = 256;

  /// The size of the header of each heap: the mutex, and the count.
  /// Heap headers are on separate cache lines.
  static const int _heapHeaderSize = 128;
  static const int _heapCountOffset = mutexSize;

  final int workerCount;
  final int taskCount;

  /// The tasks that depend on each task.
  final List<List<int>> dependents;

  /// The priority of each task, higher is taken first.
  final List<int> priorities;

  /// The number of leading tasks, whose completion is counted separately.
  final int leadingTaskCount;

  final int address;

  new _(
    this.workerCount,
    this.taskCount,
    this.dependents,
    this.priorities,
    this.leadingTaskCount,
    this.address,
  );

  /// Creates queues for [workerCount] workers, the task `i` depends on the
  /// tasks `dependencies[i]`, and has the priority `priorities[i]`.
  ///
  /// Initially ready tasks are distributed between workers round-robin.
  factory allocate(
    int workerCount,
    List<List<int>> dependencies,
    List<int> priorities, {
    int leadingTaskCount = 0,
  }) {
    var taskCount = dependencies.length;
    var dependents = List.generate(taskCount, (_) => <int>[]);
    for (var task = 0; task < taskCount; task++) {
      for (var dependency in dependencies[task]) {
        dependents[dependency].add(task);
      }
    }

    var memory = allocateZeroed(
      _globalSize +
          taskCount * 4 +
          workerCount * (_heapHeaderSize + taskCount * 4),
    );
    var queues = DependencyQueues._(
      workerCount,
      taskCount,
      dependents,
      priorities,
      leadingTaskCount,
      memory.address,
    );
    queues._globalMutex.init();
    queues._condition.init();
    for (var worker = 0; worker < workerCount; worker++) {
      queues._heapMutex(worker).init();
    }
    var readyCount = 0;
    for (var task = 0; task < taskCount; task++) {
      var pending = dependencies[task].length;
      queues._pending[task] = pending;
      if (pending == 0) {
        queues._push(readyCount++ % workerCount, task);
      }
    }
    return queues;
  }

  Pointer<Int64> get _completedCount {
    return Pointer.fromAddress(address + _completedCountOffset);
  }

  Pointer<Int64> get _completedLeadingCount {
    return Pointer.fromAddress(address + _completedLeadingCountOffset);
  }

  /// Whether all tasks are completed, or the queues are aborted.
  bool get isDone {
    return _withLock(_globalMutex, () {
      return _isAborted.value != 0 || _completedCount.value == taskCount;
    });
  }

  /// Whether all leading tasks, see [leadingTaskCount], are completed.
  bool get isLeadingDone {
    return _withLock(_globalMutex, () {
      return _completedLeadingCount.value == leadingTaskCount;
    });
  }

  NativeCondition get _condition {
    return NativeCondition(Pointer.fromAddress(address + _conditionOffset));
  }

  NativeMutex get _globalMutex => NativeMutex(Pointer.fromAddress(address));

  Pointer<Int64> get _isAborted {
    return Pointer.fromAddress(address + _isAbortedOffset);
  }

  Pointer<Int32> get _pending => Pointer.fromAddress(address + _globalSize);

  /// Incremented when tasks become ready, so that waiting workers can check
  /// whether there were new tasks since they looked for tasks.
  Pointer<Int64> get _version {
    return Pointer.fromAddress(address + _versionOffset);
  }

  /// Makes [take] return `null` in all workers.
  void abort() {
    _withLock(_globalMutex, () {
      _isAborted.value = 1;
      _condition.broadcast();
    });
  }

  /// Marks the [task] as completed by the [worker], which might make tasks
  /// that depend on it ready, and adds them to the heap of the [worker].
  void complete(int worker, int task) {
    var ready = <int>[];
    _withLock(_globalMutex, () {
      for (var dependent in dependents[task]) {
        if (--_pending[dependent] == 0) {
          ready.add(dependent);
        }
      }
    });

    if (ready.isNotEmpty) {
      _withLock(_heapMutex(worker), () {
        for (var task in ready) {
          _push(worker, task);
        }
      });
    }

    _withLock(_globalMutex, () {
      _completedCount.value++;
      if (task < leadingTaskCount) {
        _completedLeadingCount.value++;
      }
      if (ready.isNotEmpty) {
        _version.value++;
      }
      _condition.broadcast();
    });
  }

  void free() {
    freeNative(Pointer<Uint8>.fromAddress(address));
  }

  /// Returns a task for the [worker], from its own heap, or stolen from
  /// another heap, waiting for one if necessary, or `null` if all tasks are
  /// completed.
  ///
  /// Invokes [onSteal] when the task is stolen.
  int? take(int worker, {void Function(int victim)? onSteal}) {
    while (true) {
      var version = _withLock(_globalMutex, () => _version.value);
      if (_isAborted.value != 0) {
        return null;
      }

      if (_withLock(_heapMutex(worker), () => _pop(worker)) case var task?) {
        return task;
      }

      if (_steal(worker) case (var victim, var task)) {
        onSteal?.call(victim);
        return task;
      }

      var isDone = _withLock(_globalMutex, () {
        if (_isAborted.value != 0 || _completedCount.value == taskCount) {
          return true;
        }
        if (_version.value == version) {
          _condition.wait(_globalMutex);
        }
        return false;
      });
      if (isDone) {
        return null;
      }
    }
  }

  /// Returns a ready task for the [worker], from its own heap, or stolen
  /// from another heap, or `null` if there is none now.
  ///
  /// Only tasks with priority at least [minPriority] are taken.
  int? tryTake(
    int worker, {
    int minPriority = 0,
    void Function(int victim)? onSteal,
  }) {
    if (_isAborted.value != 0) {
      return null;
    }
    if (_withLock(_heapMutex(worker), () => _pop(worker, minPriority))
        case var task?) {
      return task;
    }
    if (_steal(worker, minPriority) case (var victim, var task)) {
      onSteal?.call(victim);
      return task;
    }
    return null;
  }

  /// Returns the highest priority of a ready task, or `-1` if none.
  ///
  /// The result is approximate, heaps are read without locks.
  int peekPriority() {
    var result = -1;
    for (var worker = 0; worker < workerCount; worker++) {
      if (_heapCount(worker).value > 0) {
        result = max(result, priorities[_heapItems(worker)[0]]);
      }
    }
    return result;
  }

  Pointer<Int64> _heapCount(int worker) {
    return Pointer.fromAddress(_heapHeaderAddress(worker) + _heapCountOffset);
  }

  int _heapHeaderAddress(int worker) {
    return address + _globalSize + taskCount * 4 + worker * _heapHeaderSize;
  }

  Pointer<Int32> _heapItems(int worker) {
    return Pointer.fromAddress(
      address +
          _globalSize +
          taskCount * 4 +
          workerCount * _heapHeaderSize +
          worker * taskCount * 4,
    );
  }

  NativeMutex _heapMutex(int worker) {
    return NativeMutex(Pointer.fromAddress(_heapHeaderAddress(worker)));
  }

  /// Removes the task with the highest priority from the heap of the
  /// [worker], must be invoked under its mutex.
  int? _pop(int worker, [int minPriority = 0]) {
    var countPointer = _heapCount(worker);
    var count = countPointer.value;
    if (count == 0) {
      return null;
    }
    var items = _heapItems(worker);
    var result = items[0];
    if (priorities[result] < minPriority) {
      return null;
    }
    count--;
    countPointer.value = count;
    var last = items[count];
    var priority = priorities[last];
    var index = 0;
    while (true) {
      var childIndex = 2 * index + 1;
      if (childIndex >= count) {
        break;
      }
      if (childIndex + 1 < count &&
          priorities[items[childIndex + 1]] > priorities[items[childIndex]]) {
        childIndex++;
      }
      var child = items[childIndex];
      if (priorities[child] <= priority) {
        break;
      }
      items[index] = child;
      index = childIndex;
    }
    items[index] = last;
    return result;
  }

  /// Adds the [task] to the heap of the [worker], must be invoked under its
  /// mutex, or before the queues are shared.
  void _push(int worker, int task) {
    var countPointer = _heapCount(worker);
    var items = _heapItems(worker);
    var priority = priorities[task];
    var index = countPointer.value;
    countPointer.value = index + 1;
    while (index > 0) {
      var parentIndex = (index - 1) >> 1;
      var parent = items[parentIndex];
      if (priorities[parent] >= priority) {
        break;
      }
      items[index] = parent;
      index = parentIndex;
    }
    items[index] = task;
  }

  /// Steals the task with the highest priority from other workers.
  (int, int)? _steal(int worker, [int minPriority = 0]) {
    while (true) {
      // Racy look at tops of heaps, to choose the victim.
      int? bestVictim;
      var bestPriority = minPriority - 1;
      for (var i = 1; i < workerCount; i++) {
        var victim = (worker + i) % workerCount;
        if (_heapCount(victim).value > 0) {
          var priority = priorities[_heapItems(victim)[0]];
          if (priority > bestPriority) {
            bestVictim = victim;
            bestPriority = priority;
          }
        }
      }
      if (bestVictim == null) {
        return null;
      }
      var victim = bestVictim;
      if (_withLock(_heapMutex(victim), () => _pop(victim, minPriority))
          case var task?) {
        return (victim, task);
      }
    }
  }

  static T _withLock<T>(NativeMutex mutex, T Function() operation) {
    mutex.lock();
    try {
      return operation();
    } finally {
      mutex.unlock();
    }
  }
}

/// A queue of tasks in native memory, to which the main isolate adds tasks
/// while workers take and complete tasks, when tasks are discovered while
/// processing other tasks.
class GrowingQueue {
  static const int _conditionOffset = mutexSize;
  static const int _pushedOffset = mutexSize + conditionSize;
  static const int _takenOffset = _pushedOffset + 8;
  static const int _completedOffset = _takenOffset + 8;
  static const int _isClosedOffset = _completedOffset + 8;
  static const int _headerSize = _isClosedOffset + 8;

  /// The maximum number of tasks.
  final int capacity;

  final int address;

  new _(this.capacity, this.address);

  factory allocate(int capacity) {
    var memory = allocateZeroed(_headerSize + capacity * 4);
    return GrowingQueue._(capacity, memory.address)
      .._mutex.init()
      .._condition.init();
  }

  /// The tasks in the order of completion.
  Pointer<Int32> get _completed => Pointer.fromAddress(address + _headerSize);

  Pointer<Int64> get _completedCount {
    return Pointer.fromAddress(address + _completedOffset);
  }

  NativeCondition get _condition {
    return NativeCondition(Pointer.fromAddress(address + _conditionOffset));
  }

  Pointer<Int64> get _isClosed {
    return Pointer.fromAddress(address + _isClosedOffset);
  }

  NativeMutex get _mutex => NativeMutex(Pointer.fromAddress(address));

  Pointer<Int64> get _pushed => Pointer.fromAddress(address + _pushedOffset);

  Pointer<Int64> get _taken => Pointer.fromAddress(address + _takenOffset);

  /// The number of tasks pushed so far.
  int get pushedCount => _withLock(() => _pushed.value);

  /// Makes [take] return `null` once there are no tasks to take.
  void close() {
    _withLock(() {
      _isClosed.value = 1;
      _condition.broadcast();
    });
  }

  /// Records that the [task] is completed.
  void complete(int task) {
    _withLock(() {
      var count = _completedCount.value;
      _completed[count] = task;
      _completedCount.value = count + 1;
      _condition.broadcast();
    });
  }

  void free() {
    freeNative(Pointer<Uint8>.fromAddress(address));
  }

  /// For the main isolate: waits until there are tasks completed after the
  /// first [seenCompletedCount], and returns them; or a task to run, if
  /// [takeTasks]; or `null` if all pushed tasks are completed.
  ({List<int> completed, int? task})? next(
    int seenCompletedCount, {
    required bool takeTasks,
  }) {
    return _withLock(() {
      while (true) {
        var completedCount = _completedCount.value;
        if (completedCount > seenCompletedCount) {
          return (
            completed: [
              for (var i = seenCompletedCount; i < completedCount; i++)
                _completed[i],
            ],
            task: null,
          );
        }
        var taken = _taken.value;
        var pushed = _pushed.value;
        if (takeTasks && taken < pushed) {
          _taken.value = taken + 1;
          return (completed: const <int>[], task: taken);
        }
        if (completedCount == pushed) {
          return null;
        }
        _condition.wait(_mutex);
      }
    });
  }

  /// Makes [count] more tasks available, the task indices are consecutive.
  void push(int count) {
    _withLock(() {
      _pushed.value += count;
      _condition.broadcast();
    });
  }

  /// Whether the queue is closed, and all its tasks are taken.
  bool get isDrained {
    return _withLock(() {
      return _isClosed.value != 0 && _taken.value == _pushed.value;
    });
  }

  /// For workers: returns a task, if one is available now.
  int? tryTake() {
    return _withLock(() {
      var taken = _taken.value;
      if (taken < _pushed.value) {
        _taken.value = taken + 1;
        return taken;
      }
      return null;
    });
  }

  /// For workers: returns a task, waiting for one if necessary, or `null`
  /// if the queue is closed and there are no tasks.
  int? take() {
    return _withLock(() {
      while (true) {
        var taken = _taken.value;
        if (taken < _pushed.value) {
          _taken.value = taken + 1;
          return taken;
        }
        if (_isClosed.value != 0) {
          return null;
        }
        _condition.wait(_mutex);
      }
    });
  }

  T _withLock<T>(T Function() operation) {
    var mutex = _mutex;
    mutex.lock();
    try {
      return operation();
    } finally {
      mutex.unlock();
    }
  }
}
