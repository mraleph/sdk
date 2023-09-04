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

#include "bin/dartutils.h"

extern "C" {
#define ASM_SYMBOL(Sym) Sym

extern const uint8_t ASM_SYMBOL(kPlatformDill)[];
extern const intptr_t ASM_SYMBOL(kPlatformDillSize);
extern const uint8_t ASM_SYMBOL(kAppDill)[];
extern const intptr_t ASM_SYMBOL(kAppDillSize);
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

#define RETURN_IF_ERROR(e)                                                     \
  do {                                                                         \
    if (Dart_IsError(e)) return e;                                             \
  } while (0);

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

void AbortIfError(char* result, const char* action) {
  if (result != nullptr) {
    LOG_ERROR("%s failed with error: %s\n", action, result);
    free(result);
    abort();
  }
}

Dart_Handle NewString(const char* str) {
  return ABORT_IF_ERROR(Dart_NewStringFromCString(str));
}

void ReadFully(const char* path, uint8_t** data, intptr_t* data_len) {
  auto stream = dart::bin::DartUtils::OpenFileUri(path, /*write=*/false);
  if (stream == nullptr) {
    LOG_ERROR("failed to open file %s\n", path);
    abort();
  }
  dart::bin::DartUtils::ReadFile(data, data_len, stream);
  if (*data_len <= 0) {
    LOG_ERROR("failed to read file %s\n", path);
    abort();
  }
  dart::bin::DartUtils::CloseFile(stream);
}

Dart_Isolate CreateGroupCallback(const char* script_uri,
                                 const char* name,
                                 const char* package_root,
                                 const char* package_config,
                                 Dart_IsolateFlags* flags,
                                 void* isolate_data,
                                 char** error) {
  Dart_Isolate isolate = nullptr;
  if (strcmp(script_uri, DART_VM_SERVICE_ISOLATE_NAME) == 0) {
    LOG_ERROR("STARTING VM SERVICE ISOLATE");
    dart::embedder::IsolateCreationData isolate_creation_data = {
        script_uri,
        name,
        flags,
        isolate_data,
    };

    dart::embedder::VmServiceConfiguration service_config = {
        .ip = "::",
        .port = 8787,
        .dev_mode = true,
        .deterministic = false,
        .disable_auth_codes = true,
    };

    isolate = dart::embedder::CreateVmServiceIsolateFromKernel(
        isolate_creation_data, service_config, ASM_SYMBOL(kPlatformDill),
        ASM_SYMBOL(kPlatformDillSize), error);
    if (isolate == nullptr) {
      LOG_ERROR("Failed to start isolate %s (%s): %s\n", script_uri, name,
                *error);
    }
  }

  return isolate;
}

static void Finalizer(void* isolate_callback_data, void* peer) {
  free(peer);
}

Dart_Handle LibraryTagHandler(Dart_LibraryTag tag,
                              Dart_Handle library,
                              Dart_Handle url) {
  const char* url_string = nullptr;
  RETURN_IF_ERROR(Dart_StringToCString(url, &url_string));

  if (tag == Dart_kKernelTag) {
    uint8_t* kernel_buffer = nullptr;
    intptr_t kernel_buffer_size = 0;
    ReadFully(url_string, &kernel_buffer, &kernel_buffer_size);

    Dart_Handle result = Dart_NewExternalTypedData(
        Dart_TypedData_kUint8, kernel_buffer, kernel_buffer_size);
    Dart_NewFinalizableHandle(result, kernel_buffer, kernel_buffer_size,
                              &Finalizer);
    return result;
  }
  LOG_ERROR("Unimplemented tag : %d '%s'\n", tag, url_string);
  return dart::bin::DartUtils::NewError("Unimplemented tag : %d '%s'", tag,
                                        url_string);
}

void* main_isolate_exports = nullptr;
Dart_Isolate main_isolate = nullptr;

}  // namespace

namespace dart::embedder::simple {

void ProcessEvents(void* isolate) {
  Dart_EnterIsolate(reinterpret_cast<Dart_Isolate>(isolate));
  Dart_EnterScope();
  ABORT_IF_ERROR(Dart_HandleMessage());
  Dart_ExitScope();
  Dart_ExitIsolate();
}

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

  std::vector<const char*> flags{};
  error = Dart_SetVMFlags(flags.size(), flags.data());
  if (error != nullptr) {
    LOG_ERROR("Dart_SetVMFlags failed: %s\n", error);
    abort();
    return nullptr;
  }

  Dart_InitializeParams init_params;
  memset(&init_params, 0, sizeof(init_params));
  init_params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  init_params.create_group = CreateGroupCallback;

  error = Dart_Initialize(&init_params);
  if (error != nullptr) {
    dart::embedder::Cleanup();
    LOG_ERROR("Dart_Initialize failed: %s\n", error);
    free(error);
    return nullptr;
  }

  Dart_IsolateFlags isolate_flags;
  Dart_IsolateFlagsInitialize(&isolate_flags);
  isolate_flags.null_safety = true;

  Dart_Isolate isolate = Dart_CreateIsolateGroupFromKernel(
      /*script_uri=*/"main.dart",
      /*name=*/"main", ASM_SYMBOL(kPlatformDill), ASM_SYMBOL(kPlatformDillSize),
      &isolate_flags,
      /*isolate_group_data=*/nullptr,
      /*isolate_data=*/nullptr, &error);
  if (error != nullptr || isolate == nullptr) {
    dart::embedder::Cleanup();
    LOG_ERROR("Dart_CreateIsolateGroup failed: %s\n", error);
    free(error);
    abort();
  }

  Dart_EnterScope();

  ABORT_IF_ERROR(Dart_SetLibraryTagHandler(&LibraryTagHandler));
  ABORT_IF_ERROR(Dart_LoadScriptFromKernel(ASM_SYMBOL(kAppDill),
                                           ASM_SYMBOL(kAppDillSize)));
  ABORT_IF_ERROR(dart::embedder::InitializeCoreLibraries());

  Dart_Handle exports =
      ABORT_IF_ERROR(Dart_Invoke(ABORT_IF_ERROR(Dart_RootLibrary()),
                                 NewString("#ffiExports"), 0, nullptr));

  int64_t exports_value;
  ABORT_IF_ERROR(Dart_IntegerToInt64(exports, &exports_value));

  Dart_ExitScope();
  Dart_ExitIsolate();

  ABORT_IF_ERROR(Dart_IsolateMakeRunnable(isolate));

  main_isolate = isolate;
  main_isolate_exports =
      reinterpret_cast<void*>(static_cast<intptr_t>(exports_value));

  return 0;
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