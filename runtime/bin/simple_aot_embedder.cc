// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(ANDROID) || defined(__ANDROID__)
#include <android/log.h>
#endif

#include "include/dart_api.h"
#include "include/dart_embedder_api.h"

extern "C" {
#if defined(__APPLE__)
#define ASM_SYMBOL(Sym) Sym
#else
#define ASM_SYMBOL(Sym) _##Sym
#endif

extern const uint8_t ASM_SYMBOL(kDartVmSnapshotData)[];
extern const uint8_t ASM_SYMBOL(kDartVmSnapshotInstructions)[];
extern const uint8_t ASM_SYMBOL(kDartIsolateSnapshotData)[];
extern const uint8_t ASM_SYMBOL(kDartIsolateSnapshotInstructions)[];
}

namespace {

#if defined(ANDROID) || defined(__ANDROID__)
#define LOG_ERROR(...)                                                         \
  __android_log_print(ANDROID_LOG_ERROR, "DART", __VA_ARGS__)
#else
#define LOG_ERROR(...) fprintf(stderr, __VA_ARGS__)
#endif

#define ABORT_IF_ERROR(e) AbortIfError((e), #e)
#define ABORT_IF_ERROR_OR_NULL(e) AbortIfErrorOrNull((e), #e)

const char* ToCString(Dart_Handle handle) {
  if (Dart_IsError(handle)) {
    return Dart_GetError(handle);
  } else if (Dart_IsString(handle)) {
    const char* result = nullptr;
    Dart_Handle err = Dart_StringToCString(handle, &result);
    if (Dart_IsError(err)) {
      return Dart_GetError(err);
    } else {
      return result;
    }
  } else {
    return ToCString(Dart_ToString(handle));
  }
}

Dart_Handle AbortIfError(Dart_Handle result, const char* action) {
  if (Dart_IsError(result)) {
    LOG_ERROR("%s failed with error: %s\n", action, ToCString(result));
    abort();
  }
  return result;
}

Dart_Handle NewString(const char* str) {
  return ABORT_IF_ERROR(Dart_NewStringFromCString(str));
}

void* main_isolate_exports;
Dart_Isolate main_isolate;

}  // namespace

namespace dart::embedder::simple {

void* MainIsolate() {
  if (main_isolate != nullptr) {
    return main_isolate_exports;
  }

  char* error;

  if (!dart::embedder::InitOnce(&error)) {
    LOG_ERROR("Standalone embedder initialization failed: %s\n", error);
    free(error);
    abort();
    return nullptr;
  }

  std::vector<const char*> flags{"--precompilation"};
  error = Dart_SetVMFlags(flags.size(), flags.data());
  if (error != nullptr) {
    LOG_ERROR("Dart_SetVMFlags failed: %s\n", error);
    abort();
    return nullptr;
  }

  Dart_InitializeParams init_params;
  memset(&init_params, 0, sizeof(init_params));
  init_params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  init_params.vm_snapshot_data = ASM_SYMBOL(kDartVmSnapshotData);
  init_params.vm_snapshot_instructions =
      ASM_SYMBOL(kDartVmSnapshotInstructions);

  error = Dart_Initialize(&init_params);
  if (error != nullptr) {
    dart::embedder::Cleanup();
    LOG_ERROR("Dart_Initialize failed: %s\n", error);
    free(error);
    abort();
    return nullptr;
  }

  Dart_IsolateFlags isolate_flags;
  Dart_IsolateFlagsInitialize(&isolate_flags);
  isolate_flags.null_safety = true;

  Dart_Isolate isolate = Dart_CreateIsolateGroup(
      /*script_uri=*/"test.dart",
      /*name=*/"main", ASM_SYMBOL(kDartIsolateSnapshotData),
      ASM_SYMBOL(kDartIsolateSnapshotInstructions),
      /*flags=*/&isolate_flags,
      /*isolate_group_data=*/nullptr,
      /*isolate_data=*/nullptr, &error);
  if (error != nullptr) {
    dart::embedder::Cleanup();
    LOG_ERROR("Dart_CreateIsolateGroup failed: %s\n", error);
    free(error);
    abort();
    return nullptr;
  }

  Dart_EnterScope();
  ABORT_IF_ERROR(dart::embedder::InitializeCoreLibraries());

  Dart_Handle exports =
      ABORT_IF_ERROR(Dart_Invoke(ABORT_IF_ERROR(Dart_RootLibrary()),
                                 NewString("#ffiExports"), 0, nullptr));

  int64_t exports_value;
  ABORT_IF_ERROR(Dart_IntegerToInt64(exports, &exports_value));

  Dart_ExitScope();
  Dart_ExitIsolate();

  main_isolate = isolate;
  main_isolate_exports =
      reinterpret_cast<void*>(static_cast<intptr_t>(exports_value));
  return main_isolate_exports;
}

void EnterMainIsolate() {
  MainIsolate();
  Dart_EnterIsolate(main_isolate);
  Dart_EnterScope();
}

void ExitMainIsolate() {
  Dart_ExitScope();
  Dart_ExitIsolate();
}

void ProcessEvents(void* isolate) {
  Dart_EnterIsolate(reinterpret_cast<Dart_Isolate>(isolate));
  Dart_EnterScope();
  ABORT_IF_ERROR(Dart_HandleMessage());
  Dart_ExitScope();
  Dart_ExitIsolate();
}

void ConnectToEventLoop(void (*notify)(void*)) {
  MainIsolate();
  Dart_EnterIsolate(main_isolate);
  Dart_EnterScope();
  Dart_SetMessageNotifyCallback(
      reinterpret_cast<Dart_MessageNotifyCallback>(notify));
  Dart_ExitScope();
  Dart_ExitIsolate();
}

}  // namespace dart::embedder::simple