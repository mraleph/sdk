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
#define LOG_ERROR(...) \
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

Dart_Handle AbortIfErrorOrNull(Dart_Handle result, const char* action) {
  if (Dart_IsNull(AbortIfError(result, action))) {
    LOG_ERROR("%s returned null unexpectedly\n", action);
    abort();
  }
  return result;
}

Dart_Handle NewString(const char* str) {
  return ABORT_IF_ERROR(Dart_NewStringFromCString(str));
}

}  // namespace

int main(int argc, char* argv[]) {
  char* error;

  if (!dart::embedder::InitOnce(&error)) {
    LOG_ERROR("Standalone embedder initialization failed: %s\n", error);
    free(error);
    abort();
    return -1;
  }

  std::vector<const char*> flags{"--precompilation"};
  error = Dart_SetVMFlags(flags.size(), flags.data());
  if (error != nullptr) {
    LOG_ERROR("Dart_SetVMFlags failed: %s\n", error);
    abort();
    return -1;
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
    return -1;
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
  if (error != nullptr || isolate == nullptr) {
    dart::embedder::Cleanup();
    LOG_ERROR("Dart_CreateIsolateGroup failed: %s\n", error);
    free(error);
    abort();
  }

  Dart_EnterScope();

  ABORT_IF_ERROR(dart::embedder::InitializeCoreLibraries());

  Dart_Handle root_lib =
      AbortIfErrorOrNull(Dart_RootLibrary(), "getting root library");
  Dart_Handle main_closure = AbortIfErrorOrNull(
      Dart_GetField(root_lib, Dart_NewStringFromCString("main")),
      "getting main closure");
  if (!Dart_IsClosure(main_closure)) {
    LOG_ERROR("main is not a closure");
    abort();
  }

  const intptr_t kNumIsolateArgs = 2;
  Dart_Handle isolate_args[kNumIsolateArgs];
  isolate_args[0] = main_closure;  // entryPoint

  Dart_Handle string_type =
      Dart_GetNonNullableType(Dart_LookupLibrary(NewString("dart:core")),
                   NewString("String"), 0, nullptr);

  Dart_Handle args =
      Dart_NewListOfTypeFilled(string_type, NewString(""), argc - 1);
  for (int i = 0; i < argc - 1; i++) {
    Dart_ListSetAt(args, i, NewString(argv[i + 1]));
  }
  isolate_args[1] = args;

  Dart_Handle isolate_lib = AbortIfErrorOrNull(
      Dart_LookupLibrary(NewString("dart:isolate")), "getting dart:isolate");

  AbortIfError(Dart_Invoke(isolate_lib, NewString("_startMainIsolate"),
                           kNumIsolateArgs, isolate_args),
               "starting isolate");

  Dart_Handle result = Dart_RunLoop();
  AbortIfError(result, "executing main");
  Dart_ExitScope();

  Dart_ExitIsolate();
  return 0;
}
