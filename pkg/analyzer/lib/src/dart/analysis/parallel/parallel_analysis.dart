// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:async';
import 'dart:developer';
import 'dart:math';
import 'dart:isolate';

import 'package:analyzer/diagnostic/diagnostic.dart';
import 'package:analyzer/src/analysis_options/analysis_options.dart';
import 'package:analyzer/src/analysis_rule/analysis_rule.dart';
import 'package:analyzer/src/dart/analysis/driver.dart';
import 'package:analyzer/src/dart/analysis/file_state.dart';
import 'package:analyzer/src/dart/analysis/library_graph.dart';
import 'package:analyzer/src/dart/analysis/parallel/byte_store.dart';
import 'package:analyzer/src/dart/analysis/parallel/native.dart';
import 'package:analyzer/src/dart/analysis/parallel/shared_singletons.dart';
import 'package:analyzer/src/dart/analysis/parallel/work_stealing.dart';
import 'package:analyzer/src/dart/analysis/session.dart';
import 'package:analyzer/src/lint/registry.dart';
import 'package:analyzer/src/summary/package_bundle_reader.dart';
import 'package:analyzer/src/util/performance/operation_performance.dart';
import 'package:analyzer/src/util/shared_state_lock.dart';
import 'package:analyzer/src/utilities/uri_cache.dart';
import 'package:analyzer/src/workspace/pub.dart';

/// Diagnostics of a single file.
class FileDiagnostics {
  final String path;
  final List<Diagnostic> diagnostics;

  new(this.path, this.diagnostics);
}

/// Analyzes files of an [AnalysisDriver] using multiple isolates of the same
/// isolate group, which share the state of the driver: the file states, the
/// element model, and the byte store.
///
/// The objects are shared through a persistent handle, see
/// [SharedObjectHandle], which stands in for shared memory multithreading.
///
/// The work is done in phases:
/// 1. Prefetching files: reading, parsing, and computing unlinked data, in
///    parallel, while the main isolate discovers referenced files.
/// 2. Building the file graph and library cycles, serially.
/// 3. Linking or reading bundles of library cycles, and computing diagnostics
///    of libraries, in parallel, respecting dependencies between these tasks.
///    Each worker has its own queue of ready tasks, and idle workers steal
///    tasks of other workers, see [DependencyQueues].
///
/// With a single worker, linking and computing diagnostics are done the same
/// way as [AnalysisDriver] does.
class ParallelAnalysis {
  final AnalysisDriver driver;
  final List<String> paths;

  /// Invoked in each new worker isolate before it does any work, e.g. to
  /// register lint rules. Must be sendable to an isolate of the same group.
  final void Function()? isolateSetup;

  final OperationPerformanceImpl performance = OperationPerformanceImpl(
    '<root>',
  );

  /// The elapsed time of each phase of [run], in milliseconds.
  final Map<String, int> phaseTimes = {};

  /// Whether units parsed to compute unlinked data are kept until linking,
  /// so that linking does not parse them again.
  final bool retainParsedUnits;

  /// Statistics of each worker in the last analysis phase.
  List<WorkerStatistics> workerStatistics = const [];

  /// Whether to record timeline events for phases and tasks.
  final bool traceTasks;

  new(
    this.driver,
    this.paths, {
    this.isolateSetup,
    this.retainParsedUnits = true,
    this.traceTasks = false,
  });

  Future<List<FileDiagnostics>> run({int workerCount = 1}) async {
    var byteStore = driver.byteStoreForParallelAnalysis;
    if (workerCount > 1 && byteStore is! SyncFileByteStore) {
      throw StateError(
        'Parallel analysis requires SyncFileByteStore, '
        'not ${byteStore.runtimeType}',
      );
    }

    var pool = await _timeAsync('startWorkers', () {
      return _WorkerPool.start(
        driver,
        workerCount,
        isolateSetup,
        traceTasks: traceTasks,
      );
    });
    var fsState = driver.fsState;
    try {
      if (retainParsedUnits) {
        fsState.startRetainingParsedUnits();
      }
      if (workerCount > 1) {
        await _timeAsync('prefetch', () => _prefetch(pool));
      }
      var libraries = _time('discover', () => _discoverLibraries());
      fsState.prefetchedFiles = null;

      List<List<FileDiagnostics>> results;
      if (workerCount > 1) {
        results = await _timeAsync('linkAndAnalyze', () {
          return _linkAndAnalyze(pool, libraries);
        });
      } else {
        _time('load', () {
          performance.run('load', (performance) {
            for (var library in libraries) {
              driver.libraryContext.loadForResolution(
                targetLibrary: library,
                performance: performance,
              );
            }
          });
        });
        fsState.stopRetainingParsedUnits();
        results = await _timeAsync('analyze', () {
          return _analyze(pool, libraries);
        });
      }

      var pathSet = paths.toSet();
      return [
        for (var libraryResults in results)
          for (var fileResult in libraryResults)
            if (pathSet.contains(fileResult.path)) fileResult,
      ];
    } finally {
      fsState.prefetchedFiles = null;
      fsState.stopRetainingParsedUnits();
      await pool.shutdown();
    }
  }

  Future<List<List<FileDiagnostics>>> _analyze(
    _WorkerPool pool,
    List<LibraryFileKind> libraries,
  ) async {
    // Largest libraries first.
    var sizes = [
      for (var library in libraries)
        library.files.fold(0, (sum, file) => sum + file.content.length),
    ];
    var order = List.generate(libraries.length, (i) => i)
      ..sort((a, b) => sizes[b].compareTo(sizes[a]));

    var phase = _AnalyzePhase(driver, libraries, order);
    await pool.run(phase);
    workerStatistics = phase.statistics.cast<WorkerStatistics>();

    return [
      for (var (i, result) in phase.results.indexed)
        result ?? (throw StateError('No result for ${libraries[i].file.path}')),
    ];
  }

  /// Loads element models of the [libraries], and their dependencies, and
  /// computes diagnostics of the [libraries], in parallel.
  ///
  /// Each library cycle is linked, or its bundle is read, after the cycles
  /// that it depends on. Each library is analyzed after the cycles of the
  /// library and its doc-imported libraries are loaded.
  Future<List<List<FileDiagnostics>>> _linkAndAnalyze(
    _WorkerPool pool,
    List<LibraryFileKind> libraries,
  ) async {
    var libraryContext = driver.libraryContext;
    var cycles = libraryContext.cyclesToLoad([
      ...libraries,
      for (var library in libraries) ...library.docImportedLibraries,
    ]);
    var cycleIndex = Map<LibraryCycle, int>.identity();
    for (var (i, cycle) in cycles.indexed) {
      cycleIndex[cycle] = i;
    }

    // Link tasks are `[0, cycles.length)`, analysis tasks follow.
    var dependencies = [
      for (var cycle in cycles)
        [
          for (var dependency in cycle.directDependencies)
            ?cycleIndex[dependency],
        ],
      for (var library in libraries)
        {
          ?cycleIndex[library.libraryCycle],
          for (var docImported in library.docImportedLibraries)
            ?cycleIndex[docImported.libraryCycle],
        }.toList(),
    ];

    int sizeOf(Iterable<LibraryFileKind> libraries) {
      var size = 0;
      for (var library in libraries) {
        for (var file in library.files) {
          size += file.content.length;
        }
      }
      return size;
    }

    // Analysis of a library takes about 5 times longer than linking.
    var costs = [
      for (var cycle in cycles) sizeOf(cycle.libraries),
      for (var library in libraries) 5 * sizeOf([library]),
    ];

    var phase = _LinkAndAnalyzePhase(
      driver,
      cycles,
      libraries,
      dependencies,
      _criticalPathPriorities(dependencies, costs),
    );
    await pool.run(phase);
    driver.fsState.stopRetainingParsedUnits();
    libraryContext.createTypeProvider();
    workerStatistics = phase.statistics.cast<WorkerStatistics>();

    return [
      for (var (i, result) in phase.results.indexed)
        result ?? (throw StateError('No result for ${libraries[i].file.path}')),
    ];
  }

  /// Returns the priority of each task: the cost of the most expensive path
  /// from the task through the tasks that depend on it.
  static List<int> _criticalPathPriorities(
    List<List<int>> dependencies,
    List<int> costs,
  ) {
    var taskCount = dependencies.length;
    var dependents = List.generate(taskCount, (_) => <int>[]);
    var pending = List.filled(taskCount, 0);
    for (var task = 0; task < taskCount; task++) {
      for (var dependency in dependencies[task]) {
        dependents[dependency].add(task);
      }
      pending[task] = dependencies[task].length;
    }

    // Topological order, dependencies first.
    var order = [
      for (var task = 0; task < taskCount; task++)
        if (pending[task] == 0) task,
    ];
    for (var i = 0; i < order.length; i++) {
      for (var dependent in dependents[order[i]]) {
        if (--pending[dependent] == 0) {
          order.add(dependent);
        }
      }
    }

    var priorities = List.filled(taskCount, 0);
    for (var task in order.reversed) {
      var maxDependent = 0;
      for (var dependent in dependents[task]) {
        maxDependent = max(maxDependent, priorities[dependent]);
      }
      priorities[task] = costs[task] + maxDependent;
    }
    return priorities;
  }

  /// Discovers the libraries that contain [paths], and everything that is
  /// required to analyze them, so that the file state is not changed during
  /// the parallel analysis.
  List<LibraryFileKind> _discoverLibraries() {
    var fsState = driver.fsState;
    var libraries = <LibraryFileKind>{};
    for (var path in paths) {
      var file = fsState.getFileForPath(path);
      var kind = file.kind;
      libraries.add(kind.library ?? kind.asLibrary);
    }
    for (var library in libraries) {
      library.libraryCycle;
      for (var docImported in library.docImportedLibraries) {
        docImported.libraryCycle;
      }
      for (var fileKind in library.fileKinds) {
        fileKind.libraryImports;
        fileKind.libraryExports;
        fileKind.partIncludes;
        fileKind.docLibraryImports;
        var file = fileKind.file;
        file.analysisOptions.signature;
        if (file.workspacePackage case PubPackage package) {
          package.sdkVersionConstraint;
        }
      }
    }
    return libraries.toList();
  }

  /// Reads files, and computes their unlinked data, in parallel, starting
  /// from [paths], and then the files that they reference, transitively.
  ///
  /// The results are used by [_discoverLibraries], when it creates files.
  /// Missing files are read on demand, so this is only an optimization.
  Future<void> _prefetch(_WorkerPool pool) async {
    var phase = _PrefetchPhase(driver, paths, retainParsedUnits);
    driver.fsState.prefetchedFiles = phase.prefetchedFiles;
    await pool.run(phase);
  }

  T _time<T>(String name, T Function() operation) {
    var timer = Stopwatch()..start();
    try {
      return traceTasks ? Timeline.timeSync(name, operation) : operation();
    } finally {
      phaseTimes[name] = (phaseTimes[name] ?? 0) + timer.elapsedMilliseconds;
    }
  }

  Future<T> _timeAsync<T>(String name, Future<T> Function() operation) async {
    var timer = Stopwatch()..start();
    var task = traceTasks ? (TimelineTask()..start(name)) : null;
    try {
      return await operation();
    } finally {
      task?.finish();
      phaseTimes[name] = (phaseTimes[name] ?? 0) + timer.elapsedMilliseconds;
    }
  }
}

/// Statistics of a single worker in a phase.
class WorkerStatistics {
  final int worker;
  int tasks = 0;
  int stolenTasks = 0;
  int elapsedMicroseconds = 0;

  new(this.worker);

  @override
  String toString() {
    return '[worker: $worker] [tasks: $tasks] [stolen: $stolenTasks] '
        '[time: ${elapsedMicroseconds ~/ 1000} ms]';
  }
}

/// Computes diagnostics of libraries.
class _AnalyzePhase extends _IndependentTasksPhase {
  final AnalysisDriver driver;
  final List<LibraryFileKind> libraries;
  final List<List<FileDiagnostics>?> results;

  new(this.driver, this.libraries, super.order)
    : results = List.filled(libraries.length, null);

  @override
  (String, Map<String, Object?>) describeTask(int task) {
    return ('analyze', {'library': libraries[task].file.path});
  }

  @override
  void runTask(int task) {
    var unitResults = driver.computeLibraryDiagnostics(libraries[task]);
    results[task] = [
      for (var (file, diagnostics) in unitResults)
        FileDiagnostics(file.path, diagnostics),
    ];
  }
}

/// A phase with tasks that don't depend on each other, distributed between
/// workers, with work stealing.
abstract class _IndependentTasksPhase extends _Phase {
  /// The tasks ordered by priority, from the highest.
  final List<int> order;

  late WorkStealingQueues _queues;

  new(this.order);

  @override
  void free() {
    _queues.free();
  }

  @override
  void prepare(int workerCount) {
    var workerTasks = List.generate(workerCount, (_) => <int>[]);
    for (var i = 0; i < order.length; i++) {
      workerTasks[i % workerCount].add(order[i]);
    }
    _queues = WorkStealingQueues.allocate(workerTasks);
  }

  @override
  int? take(int worker, WorkerStatistics statistics) {
    return _queues.take(worker, onSteal: (_) => statistics.stolenTasks++);
  }
}

/// Links or reads bundles of library cycles, and computes diagnostics of
/// libraries, respecting dependencies between these tasks.
class _LinkAndAnalyzePhase extends _Phase {
  final AnalysisDriver driver;
  final List<LibraryCycle> cycles;
  final List<LibraryFileKind> libraries;
  final List<List<int>> dependencies;
  final List<int> priorities;
  final List<List<FileDiagnostics>?> results;

  late DependencyQueues _queues;

  new(
    this.driver,
    this.cycles,
    this.libraries,
    this.dependencies,
    this.priorities,
  ) : results = List.filled(libraries.length, null);

  @override
  void abort() {
    _queues.abort();
  }

  @override
  void complete(int worker, int task) {
    _queues.complete(worker, task);
  }

  @override
  (String, Map<String, Object?>) describeTask(int task) {
    if (task < cycles.length) {
      var cycle = cycles[task];
      return (
        'link',
        {
          'library': cycle.libraries.first.file.path,
          'libraries': cycle.libraries.length,
        },
      );
    }
    var library = libraries[task - cycles.length];
    return ('analyze', {'library': library.file.path});
  }

  @override
  void free() {
    _queues.free();
  }

  @override
  void prepare(int workerCount) {
    _queues = DependencyQueues.allocate(workerCount, dependencies, priorities);
  }

  @override
  void runTask(int task) {
    var libraryContext = driver.libraryContext;
    if (task < cycles.length) {
      libraryContext.loadCycle(
        cycles[task],
        performance: OperationPerformanceImpl('<root>'),
      );
      return;
    }

    // Loading bundles does not create the type provider.
    libraryContext.createTypeProvider();
    var index = task - cycles.length;
    var unitResults = driver.computeLibraryDiagnostics(libraries[index]);
    results[index] = [
      for (var (file, diagnostics) in unitResults)
        FileDiagnostics(file.path, diagnostics),
    ];
  }

  @override
  int? take(int worker, WorkerStatistics statistics) {
    return _queues.take(worker, onSteal: (_) => statistics.stolenTasks++);
  }
}

/// A set of tasks that workers do in parallel.
///
/// Each worker writes only its own slots of lists, and slots of the tasks
/// that it took.
abstract class _Phase {
  late List<WorkerStatistics?> statistics;
  late List<(Object, StackTrace)?> errors;

  /// Stops giving tasks to workers, invoked when a task fails.
  void abort() {}

  /// Whether to record a timeline event for each task.
  bool traceTasks = false;

  /// Invoked after the [task] is done by the [worker].
  void complete(int worker, int task) {}

  /// Returns the name and the arguments of the timeline event of the [task].
  (String, Map<String, Object?>) describeTask(int task);

  /// Frees resources allocated by [prepare].
  void free();

  /// Prepares to run tasks by [workerCount] workers.
  void prepare(int workerCount);

  /// Runs the task with the given index, in any worker.
  void runTask(int task);

  /// Runs tasks in the [worker] until there are none.
  void runWorker(int worker, WorkerStatistics statistics) {
    while (true) {
      var task = take(worker, statistics);
      if (task == null) {
        break;
      }
      runTracedTask(task);
      complete(worker, task);
      statistics.tasks++;
    }
  }

  /// Runs the [task], with a timeline event if [traceTasks].
  void runTracedTask(int task) {
    if (traceTasks) {
      var (name, arguments) = describeTask(task);
      Timeline.timeSync(name, () => runTask(task), arguments: arguments);
    } else {
      runTask(task);
    }
  }

  /// Returns the next task for the [worker], or `null` if there are none.
  int? take(int worker, WorkerStatistics statistics);
}

/// Reads files and computes their unlinked data, starting from the initial
/// paths, and then files that they reference, transitively.
///
/// The main isolate resolves references of prefetched files, and adds new
/// tasks for files that were not seen yet, while workers prefetch files.
class _PrefetchPhase extends _Phase {
  static const int _capacity = 1 << 16;

  final AnalysisDriver driver;
  final List<String> initialPaths;
  late final Set<String> _initialPathSet = initialPaths.toSet();

  /// The files to prefetch, the main isolate sets slots before it makes
  /// them available in [_queue].
  final List<FileState?> files = List.filled(_capacity, null);
  final List<PrefetchedFile?> results = List.filled(_capacity, null);

  /// The prefetched files, used only by the main isolate.
  final Map<String, PrefetchedFile> prefetchedFiles = {};

  /// The paths of seen files, used only by the main isolate.
  final Set<String> _seenPaths = {};

  /// Resolved absolute URIs, mapped to paths, or `null` if not analyzed.
  final Map<Uri, String?> _resolvedUris = {};

  /// The paths of files whose references are prefetched, used only by the
  /// main isolate. Files that are only registered, e.g. not selected
  /// configurations, are prefetched, but their references are not.
  late final Set<String> _followedPaths = _initialPathSet.toSet();

  /// The number of tasks pushed, used only by the main isolate.
  int _pushedCount = 0;

  /// Whether to keep units parsed to compute unlinked data, for linking.
  final bool keepParsedUnits;

  late GrowingQueue _queue;

  new(this.driver, this.initialPaths, this.keepParsedUnits);

  @override
  void abort() {
    _queue.close();
  }

  @override
  void complete(int worker, int task) {
    _queue.complete(task);
  }

  @override
  void free() {
    _queue.free();
  }

  @override
  (String, Map<String, Object?>) describeTask(int task) {
    return ('prefetch', {'path': files[task]!.path});
  }

  @override
  void prepare(int workerCount) {
    _queue = GrowingQueue.allocate(_capacity);
  }

  @override
  void runTask(int task) {
    try {
      results[task] = files[task]!.prefetch(keepParsedUnit: keepParsedUnits);
    } catch (_) {
      // The file will be read again when it is used.
    }
  }

  @override
  void runWorker(int worker, WorkerStatistics statistics) {
    if (worker != 0) {
      super.runWorker(worker, statistics);
      return;
    }

    try {
      _push(initialPaths);
      var seenCompletedCount = 0;
      while (true) {
        // With enough workers, the main isolate only discovers new files,
        // so that workers don't wait for new tasks.
        var next = _queue.next(
          seenCompletedCount,
          takeTasks: this.statistics.length < 4,
        );
        if (next == null) {
          break;
        }
        if (next.task case var task?) {
          runTracedTask(task);
          complete(worker, task);
          statistics.tasks++;
          continue;
        }
        seenCompletedCount += next.completed.length;
        if (traceTasks) {
          Timeline.timeSync('prefetchReferences', () {
            _pushReferences(next.completed);
          }, arguments: {'completed': next.completed.length});
        } else {
          _pushReferences(next.completed);
        }
      }
    } finally {
      _queue.close();
    }
  }

  @override
  int? take(int worker, WorkerStatistics statistics) => _queue.take();

  void _addReferencedPaths(
    FileState file,
    PrefetchedFile prefetched,
    List<String> paths,
  ) {
    var sourceFactory = driver.sourceFactory;
    var referencedUris = prefetched.referencedUris(
      declaredVariables: driver.declaredVariables,
      // Only doc imports of analyzed libraries are loaded.
      docImports: _initialPathSet.contains(file.path),
    );
    for (var (relativeUriStr, isUsed) in referencedUris) {
      var relativeUri = uriCache.tryParse(relativeUriStr);
      if (relativeUri == null) {
        continue;
      }
      var uri = uriCache.resolveRelative(file.uri, relativeUri);
      var path = _resolvedUris.putIfAbsent(uri, () {
        var source = sourceFactory.forUri2(uri);
        return source != null && source is! InSummarySource
            ? source.fullName
            : null;
      });
      if (path == null) {
        continue;
      }
      if (!_seenPaths.contains(path)) {
        paths.add(path);
      }
      if (isUsed && _followedPaths.add(path)) {
        if (prefetchedFiles[path] case var referenced?) {
          _addReferencedPaths(referenced.file, referenced, paths);
        }
      }
    }
  }

  void _pushReferences(List<int> completed) {
    var newPaths = <String>[];
    for (var task in completed) {
      if (results[task] case var prefetched?) {
        var file = files[task]!;
        prefetchedFiles[file.path] = prefetched;
        if (_followedPaths.contains(file.path)) {
          _addReferencedPaths(file, prefetched, newPaths);
        }
      }
    }
    _push(newPaths);
  }

  void _push(List<String> paths) {
    var fsState = driver.fsState;
    var count = 0;
    for (var path in paths) {
      if (_pushedCount + count < _capacity && _seenPaths.add(path)) {
        if (fsState.newDetachedFile(path) case var file?) {
          files[_pushedCount + count] = file;
          count++;
        }
      }
    }
    _pushedCount += count;
    _queue.push(count);
  }
}

/// [SharedStateLock] that uses a [NativeMutex], shared by isolates.
///
/// Each isolate has its own instance, which tracks the recursion depth.
class _RecursiveLock implements SharedStateLock {
  final NativeMutex mutex;
  int depth = 0;

  new(this.mutex);

  @override
  void acquire() {
    if (depth++ == 0) {
      mutex.lock();
    }
  }

  @override
  void release() {
    if (--depth == 0) {
      mutex.unlock();
    }
  }
}

/// The state shared by all workers.
class _SharedState {
  final AnalysisDriver driver;
  final int workerCount;
  final NativeMutex elementModelMutex;
  final NativeMutex byteStoreMutex;
  final SharedSingletons singletons;
  final void Function()? isolateSetup;
  final bool traceTasks;

  /// The current phase, set by the main isolate before workers are asked to
  /// run it.
  _Phase? phase;

  new({
    required this.driver,
    required this.workerCount,
    required this.elementModelMutex,
    required this.byteStoreMutex,
    required this.singletons,
    required this.isolateSetup,
    required this.traceTasks,
  });
}

/// Worker isolates of the same isolate group as the main isolate.
///
/// The main isolate is the worker `0`, other workers are spawned isolates
/// that wait for phases to run.
class _WorkerPool {
  final _SharedState state;
  final SharedObjectHandle handle;
  final ReceivePort port;
  final StreamIterator<Object?> messages;
  final List<SendPort> workerPorts;

  new _(this.state, this.handle, this.port, this.messages, this.workerPorts);

  /// Runs [phase] in all workers.
  Future<void> run(_Phase phase) async {
    var workerCount = state.workerCount;
    phase.prepare(workerCount);
    phase.traceTasks = state.traceTasks;
    phase.statistics = List.filled(workerCount, null);
    phase.errors = List.filled(workerCount, null);

    state.phase = phase;
    try {
      for (var workerPort in workerPorts) {
        workerPort.send(true);
      }
      _runPhase(state, 0);
      for (var i = 0; i < workerPorts.length; i++) {
        await _nextMessage();
      }
    } finally {
      state.phase = null;
      phase.free();
    }

    for (var error in phase.errors) {
      if (error != null) {
        Error.throwWithStackTrace(error.$1, error.$2);
      }
    }
  }

  Future<void> shutdown() async {
    for (var workerPort in workerPorts) {
      workerPort.send(null);
    }
    for (var i = 0; i < workerPorts.length; i++) {
      await _nextMessage();
    }
    await messages.cancel();
    port.close();

    if (state.workerCount > 1) {
      SharedStateLock.current = null;
      AnalysisSessionImpl.useIsolateCaches(false);
      var byteStore = state.driver.byteStoreForParallelAnalysis;
      (byteStore as SyncFileByteStore).share(null);
    }
    handle.delete();
    state.elementModelMutex.free();
    state.byteStoreMutex.free();
  }

  Future<Object?> _nextMessage() async {
    if (!await messages.moveNext()) {
      throw StateError('Workers stopped unexpectedly.');
    }
    var message = messages.current;
    if (message is List) {
      throw StateError('Worker failed: ${message[0]}\n${message[1]}');
    }
    return message;
  }

  /// Runs the tasks of [_SharedState.phase] until there are none.
  static void _runPhase(_SharedState state, int worker) {
    var phase = state.phase!;
    var statistics = WorkerStatistics(worker);
    var timer = Stopwatch()..start();
    try {
      phase.runWorker(worker, statistics);
    } catch (e, st) {
      phase.errors[worker] = (e, st);
      phase.abort();
    } finally {
      statistics.elapsedMicroseconds = timer.elapsedMicroseconds;
      phase.statistics[worker] = statistics;
    }
  }

  static Future<_WorkerPool> start(
    AnalysisDriver driver,
    int workerCount,
    void Function()? isolateSetup, {
    required bool traceTasks,
  }) async {
    var state = _SharedState(
      driver: driver,
      workerCount: workerCount,
      elementModelMutex: NativeMutex.allocate(),
      byteStoreMutex: NativeMutex.allocate(),
      singletons: SharedSingletons.capture(),
      isolateSetup: isolateSetup,
      traceTasks: traceTasks,
    );
    var handle = SharedObjectHandle.create(state);
    var port = ReceivePort();
    var messages = StreamIterator(port);

    if (workerCount > 1) {
      SharedStateLock.current = _RecursiveLock(state.elementModelMutex);
      AnalysisSessionImpl.useIsolateCaches(true);
      var byteStore = driver.byteStoreForParallelAnalysis;
      (byteStore as SyncFileByteStore).share(state.byteStoreMutex);
    }

    for (var worker = 1; worker < workerCount; worker++) {
      await Isolate.spawn(
        _workerMain,
        [handle.address, worker, port.sendPort],
        onError: port.sendPort,
        debugName: 'ParallelAnalysis worker $worker',
      );
    }

    var workerPorts = <SendPort>[];
    for (var worker = 1; worker < workerCount; worker++) {
      if (!await messages.moveNext()) {
        throw StateError('Workers stopped unexpectedly.');
      }
      var message = messages.current;
      if (message is! SendPort) {
        throw StateError('Worker failed: $message');
      }
      workerPorts.add(message);
    }

    return _WorkerPool._(state, handle, port, messages, workerPorts);
  }

  static Future<void> _workerMain(List<Object> message) async {
    var handle = SharedObjectHandle(message[0] as int);
    var worker = message[1] as int;
    var mainPort = message[2] as SendPort;
    var state = handle.object as _SharedState;

    // Must be done before accessing any of the singletons.
    state.singletons.adopt();
    state.isolateSetup?.call();
    SharedStateLock.current = _RecursiveLock(state.elementModelMutex);
    AnalysisSessionImpl.useIsolateCaches(true);
    var isolateLintRules = Expando<List<AbstractAnalysisRule>>();
    AnalysisOptionsImpl.isolateLintRules = (rules) {
      return isolateLintRules[rules] ??= [
        for (var rule in rules)
          Registry.ruleRegistry[rule.name] ??
              (throw StateError('Lint rule ${rule.name} is not registered.')),
      ];
    };

    var port = ReceivePort();
    mainPort.send(port.sendPort);
    await for (var command in port) {
      if (command == null) {
        break;
      }
      _runPhase(state, worker);
      mainPort.send(worker);
    }
    port.close();
    mainPort.send(worker);
  }
}
