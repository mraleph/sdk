#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#if !defined(DART_STATICALLY_LINKED_SNAPSHOT)
#include <dlfcn.h>
#endif

#if defined(ANDROID) || defined(__ANDROID__)
#include <android/log.h>
#endif

#include "include/dart_api.h"
#include "include/dart_embedder_api.h"

#if defined(DART_STATICALLY_LINKED_SNAPSHOT)
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
#else
#if defined(__APPLE__)
#define ASM_SYMBOL_STR(Sym) #Sym
#else
#define ASM_SYMBOL_STR(Sym) "_" #Sym
#endif
#endif

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
#if defined(DART_STATICALLY_LINKED_SNAPSHOT)
  const uint8_t* vm_snapshot_data = ASM_SYMBOL(kDartVmSnapshotData);
  const uint8_t* vm_snapshot_code = ASM_SYMBOL(kDartVmSnapshotInstructions);
  const uint8_t* isolate_snapshot_data = ASM_SYMBOL(kDartIsolateSnapshotData);
  const uint8_t* isolate_snapshot_code =
      ASM_SYMBOL(kDartIsolateSnapshotInstructions);
  const bool loaded_library = false;
#else
  const uint8_t* vm_snapshot_data = nullptr;
  const uint8_t* vm_snapshot_code = nullptr;
  const uint8_t* isolate_snapshot_data = nullptr;
  const uint8_t* isolate_snapshot_code = nullptr;
  const bool loaded_library = true;
  if (argc < 2) {
    LOG_ERROR("Usage: %s <aot-snapshot> <args>\n", argv[0]);
    abort();
  }

  void* library = dlopen(argv[1], RTLD_LAZY);
  if (library == nullptr) {
    LOG_ERROR("Failed to load %s: %s\n", argv[1], dlerror());
    abort();
  }

  const auto resolve_symbol = [&](const char* symbol) -> const uint8_t* {
    void* result = dlsym(library, symbol);
    if (result == nullptr) {
      LOG_ERROR("Failed to resolve %s in %s: %s\n", symbol, argv[1], dlerror());
      abort();
    }
    return static_cast<const uint8_t*>(result);
  };

  vm_snapshot_data = resolve_symbol(ASM_SYMBOL_STR(kDartVmSnapshotData));
  vm_snapshot_code =
      resolve_symbol(ASM_SYMBOL_STR(kDartVmSnapshotInstructions));
  isolate_snapshot_data =
      resolve_symbol(ASM_SYMBOL_STR(kDartIsolateSnapshotData));
  isolate_snapshot_code =
      resolve_symbol(ASM_SYMBOL_STR(kDartIsolateSnapshotInstructions));
#endif

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
  init_params.vm_snapshot_data = vm_snapshot_data;
  init_params.vm_snapshot_instructions = vm_snapshot_code;

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
      /*name=*/"main", isolate_snapshot_data, isolate_snapshot_code,
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

  const int first_arg = loaded_library ? 2 : 1;
  const int total_args = argc - first_arg;
  Dart_Handle args =
      Dart_NewListOfTypeFilled(string_type, NewString(""), total_args);
  for (int i = 0; i < total_args; i++) {
    Dart_ListSetAt(args, i, NewString(argv[first_arg + i]));
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
