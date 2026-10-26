// Copyright (c) 2017, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:analyzer/dart/analysis/analysis_context.dart';
import 'package:analyzer/dart/analysis/declared_variables.dart';
import 'package:analyzer/dart/analysis/results.dart';
import 'package:analyzer/dart/analysis/session.dart';
import 'package:analyzer/dart/analysis/uri_converter.dart';
import 'package:analyzer/dart/element/element.dart';
import 'package:analyzer/file_system/file_system.dart';
import 'package:analyzer/src/dart/analysis/driver.dart' as driver;
import 'package:analyzer/src/dart/analysis/uri_converter.dart';
import 'package:analyzer/src/dart/element/class_hierarchy.dart';
import 'package:analyzer/src/dart/element/inheritance_manager3.dart';
import 'package:analyzer/src/summary2/linked_element_factory.dart';

/// A concrete implementation of an analysis session.
class AnalysisSessionImpl implements AnalysisSession {
  /// If not `null`, the caches that the current isolate uses instead of
  /// [_classHierarchy] and [_inheritanceManager].
  ///
  /// When the element model is shared by multiple isolates, see
  /// `ParallelAnalysis`, each isolate uses its own caches, because they are
  /// not safe for concurrent modification. Static fields are per isolate.
  static Expando<(ClassHierarchy, InheritanceManager3)>? _isolateCaches;
  static AnalysisSessionImpl? _lastIsolateCachesSession;
  static (ClassHierarchy, InheritanceManager3)? _lastIsolateCaches;

  /// The analysis driver performing analysis for this session.
  final driver.AnalysisDriver _driver;

  /// The URI converter used to convert between URI's and file paths.
  UriConverter? _uriConverter;

  ClassHierarchy _classHierarchy = ClassHierarchy();
  InheritanceManager3 _inheritanceManager = InheritanceManager3();

  /// Initialize a newly created analysis session.
  new(this._driver);

  @override
  AnalysisContext get analysisContext => _driver.analysisContext!;

  ClassHierarchy get classHierarchy {
    if (_isolateCaches case var isolateCaches?) {
      return _getIsolateCaches(isolateCaches).$1;
    }
    return _classHierarchy;
  }

  @override
  DeclaredVariables get declaredVariables => _driver.declaredVariables;

  LinkedElementFactory get elementFactory {
    return _driver.libraryContext.elementFactory;
  }

  InheritanceManager3 get inheritanceManager {
    if (_isolateCaches case var isolateCaches?) {
      return _getIsolateCaches(isolateCaches).$2;
    }
    return _inheritanceManager;
  }

  @override
  ResourceProvider get resourceProvider => _driver.resourceProvider;

  @override
  UriConverter get uriConverter {
    return _uriConverter ??= DriverBasedUriConverter(_driver);
  }

  /// Check to see that results from this session will be consistent, and throw
  /// an [InconsistentAnalysisException] if they might not be.
  void checkConsistency() {
    if (!_driver.shouldReportInconsistentAnalysisException) {
      return;
    }

    if (_driver.hasPendingFileChanges || _driver.currentSession != this) {
      throw InconsistentAnalysisException();
    }
  }

  /// Clear hierarchies, to reduce memory consumption.
  void clearHierarchies() {
    _classHierarchy = ClassHierarchy();
    _inheritanceManager = InheritanceManager3();
  }

  @override
  Future<SomeErrorsResult> getErrors(String path) async {
    checkConsistency();
    return await _driver.getErrors(path);
  }

  @override
  SomeFileResult getFile(String path) {
    checkConsistency();
    return _driver.getFileSync(path);
  }

  @override
  Future<SomeLibraryElementResult> getLibraryByUri(String uri) async {
    checkConsistency();
    return await _driver.getLibraryByUri(uri);
  }

  @override
  SomeParsedLibraryResult getParsedLibrary(String path) {
    checkConsistency();
    return _driver.getParsedLibrary(path);
  }

  @override
  SomeParsedLibraryResult getParsedLibraryByElement(LibraryElement element) {
    checkConsistency();

    if (element.session != this) {
      return NotElementOfThisSessionResult();
    }

    return _driver.getParsedLibraryByUri(element.uri);
  }

  @override
  SomeParsedUnitResult getParsedUnit(String path) {
    checkConsistency();
    return _driver.parseFileSync(path);
  }

  @override
  Future<SomeResolvedLibraryResult> getResolvedLibrary(String path) async {
    checkConsistency();
    return await _driver.getResolvedLibrary(path);
  }

  @override
  Future<SomeResolvedLibraryResult> getResolvedLibraryByElement(
    LibraryElement element,
  ) async {
    checkConsistency();

    if (element.session != this) {
      return NotElementOfThisSessionResult();
    }

    return await _driver.getResolvedLibraryByUri(element.uri);
  }

  @override
  Future<SomeResolvedLibraryResult> getResolvedLibraryContaining(
    String path,
  ) async {
    checkConsistency();
    var libraryFragmentResult = await getUnitElement(path);
    return switch (libraryFragmentResult) {
      UnitElementResult(:var fragment) => await getResolvedLibraryByElement(
        fragment.element,
      ),
      SomeResolvedLibraryResult result => result,
      _ => UnspecifiedInvalidResult(),
    };
  }

  @override
  Future<SomeResolvedUnitResult> getResolvedUnit(String path) async {
    checkConsistency();
    return await _driver.getResolvedUnit(path);
  }

  @override
  Future<SomeUnitElementResult> getUnitElement(String path) {
    checkConsistency();
    return _driver.getUnitElement(path);
  }

  (ClassHierarchy, InheritanceManager3) _getIsolateCaches(
    Expando<(ClassHierarchy, InheritanceManager3)> isolateCaches,
  ) {
    if (identical(_lastIsolateCachesSession, this)) {
      return _lastIsolateCaches!;
    }
    var result = isolateCaches[this] ??= (
      ClassHierarchy(),
      InheritanceManager3(),
    );
    _lastIsolateCachesSession = this;
    _lastIsolateCaches = result;
    return result;
  }

  /// Makes the current isolate use its own class hierarchy and inheritance
  /// caches for all sessions, if [enable] is `true`, or the shared caches
  /// of sessions, if `false`.
  static void useIsolateCaches(bool enable) {
    _isolateCaches = enable ? Expando() : null;
    _lastIsolateCachesSession = null;
    _lastIsolateCaches = null;
  }
}
