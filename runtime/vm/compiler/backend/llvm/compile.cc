// Copyright 2019 UCWeb Co., Ltd.

#include "vm/compiler/backend/llvm/compile.h"

#include "vm/compiler/backend/llvm/compiler_state.h"
#include "vm/compiler/backend/llvm/llvm_log.h"
#if defined(DART_ENABLE_LLVM_COMPILER)

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>

#define SECTION_NAME_PREFIX "."
#define SECTION_NAME(NAME) (SECTION_NAME_PREFIX NAME)

namespace dart {
namespace dart_llvm {
typedef CompilerState State;

static uint8_t* mmAllocateCodeSection(void* opaqueState,
                                      uintptr_t size,
                                      unsigned alignment,
                                      unsigned,
                                      const char* sectionName) {
  State& state = *static_cast<State*>(opaqueState);

  state.code_section_list_.push_back(dart_llvm::ByteBuffer());
  state.code_section_names_.push_back(sectionName);

  dart_llvm::ByteBuffer& bb(state.code_section_list_.back());
  bb.resize(size);

  return const_cast<uint8_t*>(bb.data());
}

static uint8_t* mmAllocateDataSection(void* opaqueState,
                                      uintptr_t size,
                                      unsigned alignment,
                                      unsigned,
                                      const char* sectionName,
                                      LLVMBool) {
  State& state = *static_cast<State*>(opaqueState);

  state.data_section_list_.push_back(dart_llvm::ByteBuffer());
  state.data_section_names_.push_back(sectionName);

  dart_llvm::ByteBuffer& bb(state.data_section_list_.back());
  bb.resize(size);
  EMASSERT((reinterpret_cast<uintptr_t>(bb.data()) & (alignment - 1)) == 0);
#if defined(TARGET_ARCH_ARM)
  static const char kExceptionTablePrefix[] = SECTION_NAME("ARM.extab");
#else
#error unsupport arch
#endif
  if (!strcmp(sectionName, SECTION_NAME("llvm_stackmaps"))) {
    state.stackMapsSection_ = &bb;
  } else if (!memcmp(sectionName, kExceptionTablePrefix,
                     sizeof(kExceptionTablePrefix) - 1)) {
    state.exception_table_ = &bb;
  }

  return const_cast<uint8_t*>(bb.data());
}

static LLVMBool mmApplyPermissions(void*, char**) {
  return false;
}

static void mmDestroy(void*) {}
#if defined(FEATURE_SAVE_OBJECT_FILE)
// This must be kept in sync with gdb/gdb/jit.h .
typedef enum {
  JIT_NOACTION = 0,
  JIT_REGISTER_FN,
  JIT_UNREGISTER_FN
} jit_actions_t;

struct jit_code_entry {
  struct jit_code_entry* next_entry;
  struct jit_code_entry* prev_entry;
  const char* symfile_addr;
  uint64_t symfile_size;
};

struct jit_descriptor {
  uint32_t version;
  // This should be jit_actions_t, but we want to be specific about the
  // bit-width.
  uint32_t action_flag;
  struct jit_code_entry* relevant_entry;
  struct jit_code_entry* first_entry;
};

extern "C" struct jit_descriptor __jit_debug_descriptor;
void SaveObjectFile(const State& state) {
  if (__jit_debug_descriptor.action_flag != JIT_REGISTER_FN) {
    return;
  }
  char file_name_buf[256];
  snprintf(file_name_buf, 256, "%s.o", state.function_name_);
  int fd = open(file_name_buf, O_WRONLY | O_CREAT | O_EXCL,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
  if (fd == -1) return;
  write(fd, __jit_debug_descriptor.first_entry->symfile_addr,
        __jit_debug_descriptor.first_entry->symfile_size);
  close(fd);
}
#endif  // defined(FEATURE_SAVE_OBJECT_FILE)

void Compile(State& state) {
  LLVMMCJITCompilerOptions options;
  LLVMInitializeMCJITCompilerOptions(&options, sizeof(options));
  options.OptLevel = 3;
  LLVMExecutionEngineRef engine;
  char* error = 0;
  options.MCJMM = LLVMCreateSimpleMCJITMemoryManager(
      &state, mmAllocateCodeSection, mmAllocateDataSection, mmApplyPermissions,
      mmDestroy);
  if (LLVMCreateMCJITCompilerForModule(&engine, state.module_, &options,
                                       sizeof(options), &error)) {
    LLVMLOGE("FATAL: Could not create LLVM execution engine: %s", error);
    EMASSERT(false);
  }
  LLVMModuleRef module = state.module_;
  LLVMPassManagerRef functionPasses = 0;
  LLVMPassManagerRef modulePasses;
  LLVMTargetDataRef targetData = LLVMGetExecutionEngineTargetData(engine);
  char* stringRepOfTargetData = LLVMCopyStringRepOfTargetData(targetData);
  LLVMSetDataLayout(module, stringRepOfTargetData);
  free(stringRepOfTargetData);

  LLVMPassManagerBuilderRef passBuilder = LLVMPassManagerBuilderCreate();
  LLVMPassManagerBuilderSetOptLevel(passBuilder, 2);
  LLVMPassManagerBuilderUseInlinerWithThreshold(passBuilder, 275);
  LLVMPassManagerBuilderSetSizeLevel(passBuilder, 0);

  functionPasses = LLVMCreateFunctionPassManagerForModule(module);
  modulePasses = LLVMCreatePassManager();

  LLVMTargetLibraryInfoRef TLI = LLVMCreateEmptyTargetLibraryInfo();
  LLVMAddTargetLibraryInfo(TLI, functionPasses);
  LLVMAddTargetLibraryInfo(TLI, modulePasses);
  LLVMTargetMachineRef target_machine =
      LLVMGetExecutionEngineTargetMachine(engine);
  LLVMAddAnalysisPasses(target_machine, functionPasses);
  LLVMAddAnalysisPasses(target_machine, modulePasses);
  LLVMPassManagerBuilderPopulateFunctionPassManager(passBuilder,
                                                    functionPasses);
  LLVMPassManagerBuilderPopulateModulePassManager(passBuilder, modulePasses);

  LLVMPassManagerBuilderDispose(passBuilder);

  LLVMInitializeFunctionPassManager(functionPasses);
  for (LLVMValueRef function = LLVMGetFirstFunction(module); function;
       function = LLVMGetNextFunction(function))
    LLVMRunFunctionPassManager(functionPasses, function);
  LLVMFinalizeFunctionPassManager(functionPasses);

  LLVMRunPassManager(modulePasses, module);
  state.entryPoint_ =
      reinterpret_cast<void*>(LLVMGetPointerToGlobal(engine, state.function_));

  if (functionPasses) LLVMDisposePassManager(functionPasses);
  LLVMDisposePassManager(modulePasses);
#if defined(FEATURE_SAVE_OBJECT_FILE)
  SaveObjectFile(state);
#endif  // defined(FEATURE_SAVE_OBJECT_FILE)
  LLVMDisposeExecutionEngine(engine);
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
