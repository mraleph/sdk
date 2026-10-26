// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:analyzer/src/dart/analysis/byte_store.dart';
import 'package:analyzer/src/dart/analysis/cache.dart';
import 'package:analyzer/src/dart/analysis/file_byte_store.dart'
    show FileByteStoreValidator;
import 'package:analyzer/src/dart/analysis/parallel/native.dart';
import 'package:path/path.dart' as p;

/// [ByteStore] that keeps recently used values in memory, and stores all
/// values as files in a directory.
///
/// Unlike `FileByteStore`, writes files synchronously, so it does not depend
/// on the event loop of the isolate that puts values. After [share] it can
/// be used concurrently by isolates of the same isolate group.
class SyncFileByteStore implements ByteStore {
  static final _validator = FileByteStoreValidator();

  /// The prefix of temporary file names of the current isolate, unique for
  /// each isolate, because static fields are per isolate.
  static final String _isolateTempPrefix = Random.secure()
      .nextInt(1 << 32)
      .toRadixString(16);
  static int _isolateTempCounter = 0;

  final String _cachePath;
  final Cache<String, Uint8List> _memoryCache;
  NativeMutex? _mutex;

  new(this._cachePath, {int memoryCacheSize = 256 * 1024 * 1024})
    : _memoryCache = Cache(memoryCacheSize, (v) => v.length);

  @override
  Uint8List? get(String key) {
    if (_withLock(() => _memoryCache.get(key)) case var bytes?) {
      return bytes;
    }
    var bytes = _readFile(key);
    if (bytes != null) {
      _withLock(() => _memoryCache.put(key, bytes));
    }
    return bytes;
  }

  @override
  Uint8List putGet(String key, Uint8List bytes) {
    _withLock(() => _memoryCache.put(key, bytes));
    _writeFile(key, bytes);
    return bytes;
  }

  @override
  void release(Iterable<String> keys) {}

  /// Allows isolates of the same group to use this store concurrently,
  /// using the [mutex] to protect the in-memory cache.
  void share(NativeMutex? mutex) {
    _mutex = mutex;
  }

  Uint8List? _readFile(String key) {
    if (!_canShard(key)) {
      return null;
    }
    try {
      var path = p.join(_shardPath(key), key);
      return _validator.getData(File(path).readAsBytesSync());
    } catch (_) {
      return null;
    }
  }

  String _shardPath(String key) => p.join(_cachePath, key.substring(0, 2));

  T _withLock<T>(T Function() operation) {
    var mutex = _mutex;
    if (mutex == null) {
      return operation();
    }
    mutex.lock();
    try {
      return operation();
    } finally {
      mutex.unlock();
    }
  }

  void _writeFile(String key, Uint8List bytes) {
    if (!_canShard(key)) {
      return;
    }
    try {
      var shardPath = _shardPath(key);
      Directory(shardPath).createSync(recursive: true);
      var tempName =
          '$key-temp-$pid-$_isolateTempPrefix-'
          '${_isolateTempCounter++}';
      var tempFile = File(p.join(shardPath, tempName));
      tempFile.writeAsBytesSync(_validator.wrapData(bytes));
      tempFile.renameSync(p.join(shardPath, key));
    } catch (_) {}
  }

  static bool _canShard(String key) {
    return key.length > 2 && !key.startsWith('.') && key[1] != '.';
  }
}
