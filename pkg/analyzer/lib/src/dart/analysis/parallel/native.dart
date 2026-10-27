// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// Native primitives that allow isolates of the same isolate group to share
/// objects and to synchronize access to them.
///
/// Objects are shared using a persistent handle: the isolate that creates
/// the objects makes a persistent handle with `Dart_NewPersistentHandle`, and
/// other isolates of the same group get the same objects back with
/// `Dart_HandleFromPersistent`. Nothing is copied. This is not a supported
/// way to share objects between isolates, it stands in for shared memory
/// multithreading.
library;

import 'dart:ffi';

final DynamicLibrary _process = DynamicLibrary.process();

final _newPersistentHandle = _process
    .lookupFunction<
      Pointer<Void> Function(Handle),
      Pointer<Void> Function(Object)
    >('Dart_NewPersistentHandle');

final _handleFromPersistent = _process
    .lookupFunction<
      Handle Function(Pointer<Void>),
      Object Function(Pointer<Void>)
    >('Dart_HandleFromPersistent');

final _deletePersistentHandle = _process
    .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>(
      'Dart_DeletePersistentHandle',
    );

final _calloc = _process
    .lookupFunction<
      Pointer<Void> Function(IntPtr, IntPtr),
      Pointer<Void> Function(int, int)
    >('calloc');

final _free = _process
    .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>(
      'free',
    );

final _mutexInit = _process
    .lookupFunction<
      Int32 Function(Pointer<Void>, Pointer<Void>),
      int Function(Pointer<Void>, Pointer<Void>)
    >('pthread_mutex_init', isLeaf: true);

final _condInit = _process
    .lookupFunction<
      Int32 Function(Pointer<Void>, Pointer<Void>),
      int Function(Pointer<Void>, Pointer<Void>)
    >('pthread_cond_init', isLeaf: true);

/// Blocks, so must not be a leaf call: other isolates must be able to reach
/// a safepoint while this isolate waits.
final _mutexLock = _process
    .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>(
      'pthread_mutex_lock',
    );

final _mutexUnlock = _process
    .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>(
      'pthread_mutex_unlock',
      isLeaf: true,
    );

/// Blocks, so must not be a leaf call.
final _condWait = _process
    .lookupFunction<
      Int32 Function(Pointer<Void>, Pointer<Void>),
      int Function(Pointer<Void>, Pointer<Void>)
    >('pthread_cond_wait');

final _condBroadcast = _process
    .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>(
      'pthread_cond_broadcast',
      isLeaf: true,
    );

/// The size reserved for `pthread_cond_t`, which is 48 bytes on x64 Linux
/// and on macOS.
const int conditionSize = 64;

/// The size reserved for `pthread_mutex_t`, which is 40 bytes on x64 Linux,
/// and 64 bytes on macOS.
const int mutexSize = 64;

/// Allocates [size] bytes of zero-initialized native memory.
Pointer<Uint8> allocateZeroed(int size) {
  var pointer = _calloc(1, size);
  if (pointer.address == 0) {
    throw StateError('Cannot allocate $size bytes.');
  }
  return pointer.cast();
}

/// Frees memory allocated with [allocateZeroed].
void freeNative(Pointer<NativeType> pointer) {
  _free(pointer.cast());
}

/// A mutex in native memory, can be used by any isolate of the process.
extension type const NativeMutex(Pointer<Void> pointer) {
  /// Allocates a new mutex.
  factory allocate() {
    return NativeMutex(allocateZeroed(mutexSize).cast())..init();
  }

  int get address => pointer.address;

  /// Initializes the mutex, must be invoked before it is used, because
  /// unlike in glibc, zero-initialized memory is not a valid mutex on macOS.
  void init() {
    if (_mutexInit(pointer, nullptr) != 0) {
      throw StateError('Cannot initialize mutex.');
    }
  }

  void free() => freeNative(pointer);

  void lock() {
    _mutexLock(pointer);
  }

  void unlock() {
    _mutexUnlock(pointer);
  }
}

/// A persistent handle for an object, which can be used by any isolate of
/// the isolate group that created it.
extension type const SharedObjectHandle(int address) {
  /// Creates a persistent handle for [object], which keeps it alive until
  /// [delete] is invoked.
  factory create(Object object) {
    return SharedObjectHandle(_newPersistentHandle(object).address);
  }

  /// Deletes the persistent handle.
  void delete() {
    _deletePersistentHandle(Pointer.fromAddress(address));
  }

  /// Returns the object, must be invoked in an isolate of the same group.
  Object get object {
    return _handleFromPersistent(Pointer.fromAddress(address));
  }
}

/// A condition variable in native memory, can be used by any isolate of the
/// process.
extension type const NativeCondition(Pointer<Void> pointer) {
  /// Initializes the condition, must be invoked before it is used.
  void init() {
    if (_condInit(pointer, nullptr) != 0) {
      throw StateError('Cannot initialize condition variable.');
    }
  }

  /// Wakes up all isolates that [wait] on this condition.
  void broadcast() {
    _condBroadcast(pointer);
  }

  /// Releases the [mutex], waits until [broadcast], and locks the [mutex].
  void wait(NativeMutex mutex) {
    _condWait(pointer, mutex.pointer);
  }
}
