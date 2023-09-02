load("@bazel_skylib//lib:selects.bzl", "selects")

SDK_ROOT = "external/dart-sdk"

def prefix(dir, files):
    return [dir + "/" + file for file in files]

DART_EMBEDDER_API_SOURCES = prefix("runtime", [
    "include/dart_embedder_api.h",
    "bin/dart_embedder_api_impl.cc",
])

# runtime/bin/BUILD.gn, dart_executable
DART_EXECUTABLE_SOURCES = prefix("runtime/bin", [
    "error_exit.cc",
    "error_exit.h",
    "crashpad.cc",
    "crashpad.h",
    "main_options.cc",
    "main_options.h",
    "options.cc",
    "options.h",
    "isolate_data.cc",
    "isolate_data.h",
    "snapshot_utils.cc",
    "snapshot_utils.h",
    "vmservice_impl.cc",
    "vmservice_impl.h",

    # ELF loader sources
    "elf_loader.cc",
    "elf_loader.h",
    "virtual_memory.h",
    "virtual_memory_fuchsia.cc",
    "virtual_memory_posix.cc",
    "virtual_memory_win.cc",
])

IO_IMPL_SOURCES = prefix("runtime/bin", [
    "console.h",
    "console_posix.cc",
    "console_win.cc",
    "eventhandler.cc",
    "eventhandler.h",
    "eventhandler_android.cc",
    "eventhandler_android.h",
    "eventhandler_fuchsia.cc",
    "eventhandler_fuchsia.h",
    "eventhandler_linux.cc",
    "eventhandler_linux.h",
    "eventhandler_macos.cc",
    "eventhandler_macos.h",
    "eventhandler_win.cc",
    "eventhandler_win.h",
    "file_system_watcher.cc",
    "file_system_watcher.h",
    "file_system_watcher_android.cc",
    "file_system_watcher_fuchsia.cc",
    "file_system_watcher_linux.cc",
    "file_system_watcher_macos.cc",
    "file_system_watcher_win.cc",
    "filter.cc",
    "filter.h",
    "ifaddrs-android.cc",
    "ifaddrs-android.h",
    "io_service.cc",
    "io_service.h",
    "io_service_no_ssl.cc",
    "io_service_no_ssl.h",
    "namespace.cc",
    "namespace.h",
    "namespace_android.cc",
    "namespace_fuchsia.cc",
    "namespace_fuchsia.h",
    "namespace_linux.cc",
    "namespace_macos.cc",
    "namespace_win.cc",
    "platform.cc",
    "platform.h",
    "platform_android.cc",
    "platform_fuchsia.cc",
    "platform_linux.cc",
    "platform_macos.cc",
    "platform_macos.h",
    "platform_win.cc",
    "process.cc",
    "process.h",
    "process_android.cc",
    "process_fuchsia.cc",
    "process_linux.cc",
    "process_macos.cc",
    "process_win.cc",
    "reference_counting.h",
    "root_certificates_unsupported.cc",
    "secure_socket_filter.cc",
    "secure_socket_filter.h",
    "secure_socket_unsupported.cc",
    "secure_socket_utils.cc",
    "secure_socket_utils.h",
    "security_context.cc",
    "security_context.h",
    "security_context_android.cc",
    "security_context_fuchsia.cc",
    "security_context_linux.cc",
    "security_context_macos.cc",
    "security_context_win.cc",
    "socket.cc",
    "socket.h",
    "socket_android.cc",
    "socket_base.cc",
    "socket_base.h",
    "socket_base_android.cc",
    "socket_base_android.h",
    "socket_base_fuchsia.cc",
    "socket_base_fuchsia.h",
    "socket_base_linux.cc",
    "socket_base_linux.h",
    "socket_base_macos.cc",
    "socket_base_macos.h",
    "socket_base_posix.cc",
    "socket_base_win.cc",
    "socket_base_win.h",
    "socket_fuchsia.cc",
    "socket_linux.cc",
    "socket_macos.cc",
    "socket_win.cc",
    "stdio.cc",
    "stdio.h",
    "stdio_android.cc",
    "stdio_fuchsia.cc",
    "stdio_linux.cc",
    "stdio_macos.cc",
    "stdio_win.cc",
    "sync_socket.cc",
    "sync_socket.h",
    "sync_socket_android.cc",
    "sync_socket_fuchsia.cc",
    "sync_socket_linux.cc",
    "sync_socket_macos.cc",
    "sync_socket_win.cc",
    "typed_data_utils.cc",
    "typed_data_utils.h",
])

VM_SOURCES = prefix("runtime/vm", [
    "allocation.cc",
    "allocation.h",
    "app_snapshot.cc",
    "app_snapshot.h",
    "base64.cc",
    "base64.h",
    "base_isolate.h",
    "bit_set.h",
    "bit_vector.cc",
    "bit_vector.h",
    "bitfield.h",
    "bitmap.cc",
    "bitmap.h",
    "boolfield.h",
    "bootstrap.h",
    "bootstrap_natives.cc",
    "bootstrap_natives.h",
    "bss_relocs.cc",
    "bss_relocs.h",
    "canonical_tables.cc",
    "canonical_tables.h",
    "class_finalizer.cc",
    "class_finalizer.h",
    "class_id.h",
    "class_table.cc",
    "class_table.h",
    "closure_functions_cache.cc",
    "closure_functions_cache.h",
    "code_comments.cc",
    "code_comments.h",
    "code_descriptors.cc",
    "code_descriptors.h",
    "code_entry_kind.h",
    "code_observers.cc",
    "code_observers.h",
    "code_patcher.cc",
    "code_patcher.h",
    "code_patcher_arm.cc",
    "code_patcher_arm64.cc",
    "code_patcher_ia32.cc",
    "code_patcher_riscv.cc",
    "code_patcher_x64.cc",
    "constants.h",
    "constants_arm.cc",
    "constants_arm.h",
    "constants_arm64.cc",
    "constants_arm64.h",
    "constants_base.h",
    "constants_ia32.cc",
    "constants_ia32.h",
    "constants_riscv.cc",
    "constants_riscv.h",
    "constants_x64.cc",
    "constants_x64.h",
    "constants_x86.h",
    "cpu.h",
    "cpu_arm.cc",
    "cpu_arm.h",
    "cpu_arm64.cc",
    "cpu_arm64.h",
    "cpu_ia32.cc",
    "cpu_ia32.h",
    "cpu_riscv.cc",
    "cpu_riscv.h",
    "cpu_x64.cc",
    "cpu_x64.h",
    "cpuid.cc",
    "cpuid.h",
    "cpuinfo.h",
    "cpuinfo_android.cc",
    "cpuinfo_fuchsia.cc",
    "cpuinfo_linux.cc",
    "cpuinfo_macos.cc",
    "cpuinfo_win.cc",
    "dart.cc",
    "dart.h",
    "dart_api_impl.h",
    "dart_api_message.h",
    "dart_api_state.cc",
    "dart_api_state.h",
    "dart_entry.cc",
    "dart_entry.h",
    "datastream.cc",
    "datastream.h",
    "debugger.cc",
    "debugger.h",
    "debugger_arm.cc",
    "debugger_arm64.cc",
    "debugger_ia32.cc",
    "debugger_riscv.cc",
    "debugger_x64.cc",
    "deferred_objects.cc",
    "deferred_objects.h",
    "deopt_instructions.cc",
    "deopt_instructions.h",
    "dispatch_table.cc",
    "dispatch_table.h",
    "double_conversion.cc",
    "double_conversion.h",
    "double_internals.h",
    "dwarf.cc",
    "dwarf.h",
    "elf.cc",
    "elf.h",
    "exceptions.cc",
    "exceptions.h",
    "experimental_features.cc",
    "experimental_features.h",
    "ffi_callback_metadata.cc",
    "ffi_callback_metadata.h",
    "field_table.cc",
    "field_table.h",
    "finalizable_data.h",
    "fixed_cache.h",
    "flag_list.h",
    "flags.cc",
    "flags.h",
    "frame_layout.h",
    "gdb_helpers.cc",
    "globals.h",
    "growable_array.h",
    "handle_visitor.h",
    "handles.cc",
    "handles.h",
    "handles_impl.h",
    "hash.h",
    "hash_map.h",
    "hash_table.h",
    "image_snapshot.cc",
    "image_snapshot.h",
    "instructions.cc",
    "instructions.h",
    "instructions_arm.cc",
    "instructions_arm.h",
    "instructions_arm64.cc",
    "instructions_arm64.h",
    "instructions_ia32.cc",
    "instructions_ia32.h",
    "instructions_riscv.cc",
    "instructions_riscv.h",
    "instructions_x64.cc",
    "instructions_x64.h",
    "intrusive_dlist.h",
    "isolate.cc",
    "isolate.h",
    "isolate_reload.cc",
    "isolate_reload.h",
    "json_stream.cc",
    "json_stream.h",
    "json_writer.cc",
    "json_writer.h",
    "kernel.cc",
    "kernel.h",
    "kernel_binary.cc",
    "kernel_binary.h",
    "kernel_isolate.cc",
    "kernel_isolate.h",
    "kernel_loader.cc",
    "kernel_loader.h",
    "lockers.cc",
    "lockers.h",
    "log.cc",
    "log.h",
    "longjump.cc",
    "longjump.h",
    "megamorphic_cache_table.cc",
    "megamorphic_cache_table.h",
    "memory_region.cc",
    "memory_region.h",
    "message.cc",
    "message.h",
    "message_handler.cc",
    "message_handler.h",
    "message_snapshot.cc",
    "message_snapshot.h",
    "metrics.cc",
    "metrics.h",
    "native_arguments.h",
    "native_entry.cc",
    "native_entry.h",
    "native_function.h",
    "native_message_handler.cc",
    "native_message_handler.h",
    "native_symbol.h",
    "native_symbol_android.cc",
    "native_symbol_fuchsia.cc",
    "native_symbol_linux.cc",
    "native_symbol_macos.cc",
    "native_symbol_win.cc",
    "object.cc",
    "object.h",
    "object_graph.cc",
    "object_graph.h",
    "object_graph_copy.cc",
    "object_graph_copy.h",
    "object_id_ring.cc",
    "object_id_ring.h",
    "object_reload.cc",
    "object_service.cc",
    "object_set.h",
    "object_store.cc",
    "object_store.h",
    "os.h",
    "os_android.cc",
    "os_fuchsia.cc",
    "os_linux.cc",
    "os_macos.cc",
    "os_thread.cc",
    "os_thread.h",
    "os_thread_absl.cc",
    "os_thread_absl.h",
    "os_thread_android.cc",
    "os_thread_android.h",
    "os_thread_fuchsia.cc",
    "os_thread_fuchsia.h",
    "os_thread_linux.cc",
    "os_thread_linux.h",
    "os_thread_macos.cc",
    "os_thread_macos.h",
    "os_thread_win.cc",
    "os_thread_win.h",
    "os_win.cc",
    "parser.cc",
    "parser.h",
    "pending_deopts.cc",
    "pending_deopts.h",
    "perfetto_utils.h",
    "pointer_tagging.h",
    "port.cc",
    "port.h",
    "port_set.h",
    "proccpuinfo.cc",
    "proccpuinfo.h",
    "profiler.cc",
    "profiler.h",
    "profiler_service.cc",
    "profiler_service.h",
    "program_visitor.cc",
    "program_visitor.h",
    "protos/perfetto/common/builtin_clock.pbzero.h",
    "protos/perfetto/trace/clock_snapshot.pbzero.h",
    "protos/perfetto/trace/trace_packet.pbzero.h",
    "protos/perfetto/trace/track_event/debug_annotation.pbzero.h",
    "protos/perfetto/trace/track_event/process_descriptor.pbzero.h",
    "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h",
    "protos/perfetto/trace/track_event/track_descriptor.pbzero.h",
    "protos/perfetto/trace/track_event/track_event.pbzero.h",
    "random.cc",
    "random.h",
    "raw_object.cc",
    "raw_object.h",
    "raw_object_fields.cc",
    "raw_object_fields.h",
    "regexp.cc",
    "regexp.h",
    "regexp_assembler.cc",
    "regexp_assembler.h",
    "regexp_assembler_bytecode.cc",
    "regexp_assembler_bytecode.h",
    "regexp_assembler_bytecode_inl.h",
    "regexp_assembler_ir.cc",
    "regexp_assembler_ir.h",
    "regexp_ast.cc",
    "regexp_ast.h",
    "regexp_bytecodes.h",
    "regexp_interpreter.cc",
    "regexp_interpreter.h",
    "regexp_parser.cc",
    "regexp_parser.h",
    "report.cc",
    "report.h",
    "resolver.cc",
    "resolver.h",
    "reusable_handles.h",
    "reverse_pc_lookup_cache.cc",
    "reverse_pc_lookup_cache.h",
    "ring_buffer.h",
    "runtime_entry.cc",
    "runtime_entry.h",
    "runtime_entry_arm.cc",
    "runtime_entry_arm64.cc",
    "runtime_entry_ia32.cc",
    "runtime_entry_list.h",
    "runtime_entry_riscv.cc",
    "runtime_entry_x64.cc",
    "scope_timer.h",
    "scopes.cc",
    "scopes.h",
    "service.cc",
    "service.h",
    "service_event.cc",
    "service_event.h",
    "service_isolate.cc",
    "service_isolate.h",
    "signal_handler.h",
    "signal_handler_android.cc",
    "signal_handler_fuchsia.cc",
    "signal_handler_linux.cc",
    "signal_handler_macos.cc",
    "signal_handler_win.cc",
    "simulator.h",
    "simulator_arm.cc",
    "simulator_arm.h",
    "simulator_arm64.cc",
    "simulator_arm64.h",
    "simulator_riscv.cc",
    "simulator_riscv.h",
    "simulator_x64.cc",
    "simulator_x64.h",
    "snapshot.cc",
    "snapshot.h",
    "source_report.cc",
    "source_report.h",
    "splay-tree.h",
    "stack_frame.cc",
    "stack_frame.h",
    "stack_frame_arm.h",
    "stack_frame_arm64.h",
    "stack_frame_ia32.h",
    "stack_frame_riscv.h",
    "stack_frame_x64.h",
    "stack_trace.cc",
    "stack_trace.h",
    "static_type_exactness_state.h",
    "stub_code.cc",
    "stub_code.h",
    "stub_code_list.h",
    "symbols.cc",
    "symbols.h",
    "tagged_pointer.h",
    "tags.cc",
    "tags.h",
    "thread.cc",
    "thread.h",
    "thread_barrier.h",
    "thread_interrupter.cc",
    "thread_interrupter.h",
    "thread_interrupter_android.cc",
    "thread_interrupter_fuchsia.cc",
    "thread_interrupter_linux.cc",
    "thread_interrupter_macos.cc",
    "thread_interrupter_win.cc",
    "thread_pool.cc",
    "thread_pool.h",
    "thread_registry.cc",
    "thread_registry.h",
    "thread_stack_resource.cc",
    "thread_stack_resource.h",
    "thread_state.cc",
    "thread_state.h",
    "timeline.cc",
    "timeline.h",
    "timeline_android.cc",
    "timeline_fuchsia.cc",
    "timeline_linux.cc",
    "timeline_macos.cc",
    "timer.cc",
    "timer.h",
    "token.cc",
    "token.h",
    "token_position.cc",
    "token_position.h",
    "type_testing_stubs.cc",
    "type_testing_stubs.h",
    "unibrow-inl.h",
    "unibrow.cc",
    "unibrow.h",
    "unicode.cc",
    "unicode_data.cc",
    "unwinding_records.cc",
    "unwinding_records.h",
    "unwinding_records_win.cc",
    "uri.cc",
    "uri.h",
    "v8_snapshot_writer.cc",
    "v8_snapshot_writer.h",
    "virtual_memory.cc",
    "virtual_memory.h",
    "virtual_memory_compressed.cc",
    "virtual_memory_compressed.h",
    "virtual_memory_fuchsia.cc",
    "virtual_memory_posix.cc",
    "virtual_memory_win.cc",
    "visitor.cc",
    "visitor.h",
    "version.h",
    "zone.cc",
    "zone.h",
    "zone_text_buffer.cc",
    "zone_text_buffer.h",
    "dart_api_impl.cc",
    "native_api_impl.cc",
]) + [
    "gen/runtime/version.cc",
]

CONSTANTS_SOURCES = prefix("runtime/vm", [
    "constants_arm.cc",
    "constants_arm.h",
    "constants_arm64.cc",
    "constants_arm64.h",
    "constants_base.h",
    "constants_ia32.cc",
    "constants_ia32.h",
    "constants_riscv.cc",
    "constants_riscv.h",
    "constants_x64.cc",
    "constants_x64.h",
])

COMPILER_SOURCES = prefix("runtime/vm/compiler", [
    "aot/aot_call_specializer.cc",
    "aot/aot_call_specializer.h",
    "aot/dispatch_table_generator.cc",
    "aot/dispatch_table_generator.h",
    "aot/precompiler.cc",
    "aot/precompiler.h",
    "aot/precompiler_tracer.cc",
    "aot/precompiler_tracer.h",
    "asm_intrinsifier.cc",
    "asm_intrinsifier.h",
    "asm_intrinsifier_arm.cc",
    "asm_intrinsifier_arm64.cc",
    "asm_intrinsifier_ia32.cc",
    "asm_intrinsifier_riscv.cc",
    "asm_intrinsifier_x64.cc",
    "assembler/assembler.h",
    "assembler/assembler_arm.cc",
    "assembler/assembler_arm.h",
    "assembler/assembler_arm64.cc",
    "assembler/assembler_arm64.h",
    "assembler/assembler_base.cc",
    "assembler/assembler_base.h",
    "assembler/assembler_ia32.cc",
    "assembler/assembler_ia32.h",
    "assembler/assembler_riscv.cc",
    "assembler/assembler_riscv.h",
    "assembler/assembler_x64.cc",
    "assembler/assembler_x64.h",
    "backend/block_builder.h",
    "backend/block_scheduler.cc",
    "backend/block_scheduler.h",
    "backend/branch_optimizer.cc",
    "backend/branch_optimizer.h",
    "backend/code_statistics.cc",
    "backend/code_statistics.h",
    "backend/compile_type.h",
    "backend/constant_propagator.cc",
    "backend/constant_propagator.h",
    "backend/evaluator.cc",
    "backend/evaluator.h",
    "backend/flow_graph.cc",
    "backend/flow_graph.h",
    "backend/flow_graph_checker.cc",
    "backend/flow_graph_checker.h",
    "backend/flow_graph_compiler.cc",
    "backend/flow_graph_compiler.h",
    "backend/flow_graph_compiler_arm.cc",
    "backend/flow_graph_compiler_arm64.cc",
    "backend/flow_graph_compiler_ia32.cc",
    "backend/flow_graph_compiler_riscv.cc",
    "backend/flow_graph_compiler_x64.cc",
    "backend/il.cc",
    "backend/il.h",
    "backend/il_arm.cc",
    "backend/il_arm64.cc",
    "backend/il_ia32.cc",
    "backend/il_printer.cc",
    "backend/il_printer.h",
    "backend/il_riscv.cc",
    "backend/il_serializer.cc",
    "backend/il_serializer.h",
    "backend/il_x64.cc",
    "backend/inliner.cc",
    "backend/inliner.h",
    "backend/linearscan.cc",
    "backend/linearscan.h",
    "backend/locations.cc",
    "backend/locations.h",
    "backend/locations_helpers.h",
    "backend/locations_helpers_arm.h",
    "backend/loops.cc",
    "backend/loops.h",
    "backend/parallel_move_resolver.cc",
    "backend/parallel_move_resolver.h",
    "backend/range_analysis.cc",
    "backend/range_analysis.h",
    "backend/redundancy_elimination.cc",
    "backend/redundancy_elimination.h",
    "backend/slot.cc",
    "backend/slot.h",
    "backend/type_propagator.cc",
    "backend/type_propagator.h",
    "call_specializer.cc",
    "call_specializer.h",
    "cha.cc",
    "cha.h",
    "compiler_pass.cc",
    "compiler_pass.h",
    "compiler_state.cc",
    "compiler_state.h",
    "compiler_timings.cc",
    "compiler_timings.h",
    "ffi/abi.cc",
    "ffi/abi.h",
    "ffi/call.cc",
    "ffi/call.h",
    "ffi/callback.cc",
    "ffi/callback.h",
    "ffi/frame_rebase.cc",
    "ffi/frame_rebase.h",
    "ffi/marshaller.cc",
    "ffi/marshaller.h",
    "ffi/native_calling_convention.cc",
    "ffi/native_calling_convention.h",
    "ffi/native_location.cc",
    "ffi/native_location.h",
    "ffi/native_type.cc",
    "ffi/range.h",
    "ffi/recognized_method.cc",
    "ffi/recognized_method.h",
    "frontend/base_flow_graph_builder.cc",
    "frontend/base_flow_graph_builder.h",
    "frontend/constant_reader.cc",
    "frontend/constant_reader.h",
    "frontend/flow_graph_builder.cc",
    "frontend/flow_graph_builder.h",
    "frontend/kernel_binary_flowgraph.cc",
    "frontend/kernel_binary_flowgraph.h",
    "frontend/kernel_fingerprints.cc",
    "frontend/kernel_fingerprints.h",
    "frontend/kernel_to_il.cc",
    "frontend/kernel_to_il.h",
    "frontend/kernel_translation_helper.cc",
    "frontend/kernel_translation_helper.h",
    "frontend/prologue_builder.cc",
    "frontend/prologue_builder.h",
    "frontend/scope_builder.cc",
    "frontend/scope_builder.h",
    "graph_intrinsifier.cc",
    "graph_intrinsifier.h",
    "intrinsifier.cc",
    "intrinsifier.h",
    "jit/jit_call_specializer.cc",
    "jit/jit_call_specializer.h",
    "method_recognizer.cc",
    "relocation.cc",
    "relocation.h",
    "stub_code_compiler.cc",
    "stub_code_compiler.h",
    "stub_code_compiler_arm.cc",
    "stub_code_compiler_arm64.cc",
    "stub_code_compiler_ia32.cc",
    "stub_code_compiler_riscv.cc",
    "stub_code_compiler_x64.cc",
    "write_barrier_elimination.cc",
    "write_barrier_elimination.h",
])

COMPILER_API_SOURCES = prefix("runtime/vm/compiler", [
    "assembler/object_pool_builder.h",
    "api/deopt_id.h",
    "api/print_filter.cc",
    "api/print_filter.h",
    "api/type_check_mode.h",
    "jit/compiler.cc",
    "jit/compiler.h",
    "method_recognizer.h",
    "recognized_methods_list.h",
    "runtime_api.cc",
    "runtime_api.h",
    "runtime_offsets_list.h",
    "runtime_offsets_extracted.h",
    "ffi/native_type.h",
])

DISASSEMBLER_SOURCES = prefix("runtime/vm/compiler", [
    "assembler/disassembler.cc",
    "assembler/disassembler.h",
    "assembler/disassembler_arm.cc",
    "assembler/disassembler_arm64.cc",
    "assembler/disassembler_riscv.cc",
    "assembler/disassembler_x86.cc",
])

FFI_SOURCES = prefix("runtime/vm/ffi", [
    "native_assets.cc",
    "native_assets.h",
])

HEAP_SOURCES = prefix("runtime/vm/heap", [
    "become.cc",
    "become.h",
    "compactor.cc",
    "compactor.h",
    "freelist.cc",
    "freelist.h",
    "gc_shared.cc",
    "gc_shared.h",
    "heap.cc",
    "heap.h",
    "marker.cc",
    "marker.h",
    "page.cc",
    "page.h",
    "pages.cc",
    "pages.h",
    "pointer_block.cc",
    "pointer_block.h",
    "safepoint.cc",
    "safepoint.h",
    "sampler.cc",
    "sampler.h",
    "scavenger.cc",
    "scavenger.h",
    "spaces.h",
    "sweeper.cc",
    "sweeper.h",
    "verifier.cc",
    "verifier.h",
    "weak_code.cc",
    "weak_code.h",
    "weak_table.cc",
    "weak_table.h",
])

BUILTIN_IMPL_SOURCES = prefix("runtime/bin", [
    "builtin.cc",
    "builtin.h",
    "crypto.cc",
    "crypto.h",
    "crypto_android.cc",
    "crypto_fuchsia.cc",
    "crypto_linux.cc",
    "crypto_macos.cc",
    "crypto_win.cc",
    "dartutils.cc",
    "dartutils.h",
    "directory.cc",
    "directory.h",
    "directory_android.cc",
    "directory_fuchsia.cc",
    "directory_linux.cc",
    "directory_macos.cc",
    "directory_win.cc",
    "exe_utils.cc",
    "exe_utils.h",
    "fdutils.h",
    "fdutils_android.cc",
    "fdutils_fuchsia.cc",
    "fdutils_linux.cc",
    "fdutils_macos.cc",
    "file.cc",
    "file.h",
    "file_android.cc",
    "file_fuchsia.cc",
    "file_linux.cc",
    "file_macos.cc",
    "file_support.cc",
    "file_win.cc",
    "file_win.h",
    "io_buffer.cc",
    "io_buffer.h",
    "lockers.h",
    "thread.h",
    "thread_absl.cc",
    "thread_absl.h",
    "thread_android.cc",
    "thread_android.h",
    "thread_fuchsia.cc",
    "thread_fuchsia.h",
    "thread_linux.cc",
    "thread_linux.h",
    "thread_macos.cc",
    "thread_macos.h",
    "thread_win.cc",
    "thread_win.h",
    "utils.cc",
    "utils.h",
    "utils_android.cc",
    "utils_fuchsia.cc",
    "utils_linux.cc",
    "utils_macos.cc",
    "utils_win.cc",
    "utils_win.h",
])

CLI_IMPL_SOURCES = prefix("runtime/bin", ["cli.cc"])

PLATFORM_SOURCES = prefix(
    "runtime/platform",
    [
        "address_sanitizer.h",
        "allocation.cc",
        "allocation.h",
        "assert.cc",
        "assert.h",
        "atomic.h",
        "elf.h",
        "floating_point.h",
        "floating_point_win.cc",
        "floating_point_win.h",
        "globals.h",
        "growable_array.h",
        "hashmap.cc",
        "hashmap.h",
        "leak_sanitizer.h",
        "mach_o.h",
        "memory_sanitizer.h",
        "pe.h",
        "priority_queue.h",
        "safe_stack.h",
        "signal_blocker.h",
        "splay-tree.h",
        "splay-tree-inl.h",
        "syslog.h",
        "syslog_android.cc",
        "syslog_fuchsia.cc",
        "syslog_linux.cc",
        "syslog_macos.cc",
        "syslog_win.cc",
        "text_buffer.cc",
        "text_buffer.h",
        "thread_sanitizer.h",
        "unaligned.h",
        "undefined_behavior_sanitizer.h",
        "unicode.cc",
        "unicode.h",
        "unwinding_records.cc",
        "unwinding_records.h",
        "unwinding_records_win.cc",
        "utils.cc",
        "utils.h",
        "utils_android.cc",
        "utils_android.h",
        "utils_fuchsia.cc",
        "utils_fuchsia.h",
        "utils_linux.cc",
        "utils_linux.h",
        "utils_macos.cc",
        "utils_macos.h",
        "utils_win.cc",
        "utils_win.h",
    ],
)

RUNTIME_LIB_SOURCES = prefix("runtime/vm", [
    "bootstrap.cc",
]) + prefix("runtime/lib", [
    # dart:async
    "async.cc",

    # dart:core
    "array.cc",
    "bool.cc",
    "date.cc",
    "double.cc",
    "errors.cc",
    "function.cc",
    "growable_array.cc",
    "identical.cc",
    "integers.cc",
    "integers.h",
    "invocation_mirror.h",
    "object.cc",
    "regexp.cc",
    "stacktrace.cc",
    "stacktrace.h",
    "stopwatch.cc",
    "string.cc",
    "uri.cc",

    # dart:developer
    "developer.cc",
    "profiler.cc",
    "timeline.cc",

    # dart:ffi
    "ffi.cc",
    "ffi_dynamic_library.cc",

    # dart:isolate
    "isolate.cc",

    # dart:math
    "math.cc",

    # dart:mirrors
    "mirrors.cc",
    "mirrors.h",

    # dart:typed_data
    "typed_data.cc",
    "simd128.cc",

    # dart:_vmservice
    "vmservice.cc",
]) + prefix("runtime", ["include/internal/dart_api_dl_impl.h"])

VM_API_HEADERS = prefix(
    "runtime",
    [
        "include/dart_api.h",
        "include/dart_api_dl.h",
        "include/dart_version.h",
        "include/dart_native_api.h",
        "include/dart_tools_api.h",
    ],
)

ALL_VM_SOURCES = PLATFORM_SOURCES + VM_SOURCES + COMPILER_API_SOURCES + \
                 DISASSEMBLER_SOURCES + FFI_SOURCES + HEAP_SOURCES + \
                 RUNTIME_LIB_SOURCES + VM_API_HEADERS

RUNTIME_INLUDE_DIRS = [
    # "-I" + SDK_ROOT,
    "-I" + SDK_ROOT + "/runtime",
    "-I" + SDK_ROOT + "/third_party",
]

COMMON_COPTS = [
    "-Wno-comment",
    "-Wno-unused-private-field",
    "-Wunused-but-set-variable",
    "-Wno-deprecated-declarations",
    "-std=c++17",
    "-DDART_IO_SECURE_SOCKET_DISABLED",
] + RUNTIME_INLUDE_DIRS

def srcs_from(files):
    return [file for file in files if not file.endswith(".h")]

def hdrs_from(files):
    return [file for file in files if file.endswith(".h")]

def vm_library(runtime_mode = "release", compiler = "jit"):
    srcs = ALL_VM_SOURCES
    copts = COMMON_COPTS
    local_defines = []
    if runtime_mode == "product":
        local_defines.append("PRODUCT")
    if compiler == "aot":
        local_defines.append("DART_PRECOMPILED_RUNTIME")
    elif compiler == "precompiler":
        local_defines.append("DART_PRECOMPILER")
    if compiler != "aot":
        srcs = srcs + COMPILER_SOURCES
    local_defines.append("EXCLUDE_CFE_AND_KERNEL_PLATFORM")
    local_defines.append("DART_EXCLUDE_ICU")
    native.cc_library(
        name = "vm_%s_%s" % (runtime_mode, compiler),
        srcs = srcs_from(srcs),
        hdrs = hdrs_from(srcs),
        # EXCLUDED: "@dart-sdk//:icu"
        deps = [":libdouble_conversion", "//:target_configuration"],
        copts = copts,
        local_defines = local_defines,

        # TODO: if (android): thread_interrupter_android_arm.S
    )

# runtime/bin/BUILD.gn, dart_executable
def dart_executable(name, runtime_mode = "release", compiler = "jit", srcs = [], deps = [], no_main_sources = False):
    deps = deps + []

    deps += [
        ":io_" + runtime_mode,
        ":vm_%s_%s" % (runtime_mode, compiler),
    ]

    local_defines = []
    if runtime_mode == "product":
        local_defines.append("PRODUCT")
    if compiler == "aot":
        local_defines.append("DART_PRECOMPILED_RUNTIME")
    elif compiler == "precompiler":
        local_defines.append("DART_PRECOMPILER")
    local_defines.append("EXCLUDE_CFE_AND_KERNEL_PLATFORM")
    local_defines.append("EXCLUDE_DARTDEV")

    if not no_main_sources:
        srcs = srcs + DART_EXECUTABLE_SOURCES + DART_EMBEDDER_API_SOURCES
        srcs += prefix("runtime/bin", ["observatory_assets_empty.cc"])

    native.cc_binary(
        name = name,
        deps = deps,
        copts = COMMON_COPTS,
        local_defines = local_defines,
        srcs = srcs,
    )

def dart_io(runtime_mode = "release", deps = [], srcs = [], local_defines = []):
    deps = deps + [":zlib"]

    srcs = srcs + IO_IMPL_SOURCES
    srcs += CLI_IMPL_SOURCES
    srcs += BUILTIN_IMPL_SOURCES
    srcs += prefix("runtime/bin", [
        "builtin_natives.cc",
        "io_natives.cc",
        "io_natives.h",
    ])
    srcs += hdrs_from(PLATFORM_SOURCES)
    srcs += VM_API_HEADERS
    local_defines = local_defines + ["DART_IO_ROOT_CERTS_DISABLED"]
    if runtime_mode == "product":
        local_defines.append("PRODUCT")

    native.objc_library(
        name = "platform_macos_cocoa_" + runtime_mode,
        srcs = prefix("runtime/bin", [
            "platform_macos_cocoa.mm",
        ]) + hdrs_from(PLATFORM_SOURCES),
        hdrs = ["runtime/bin/platform_macos_cocoa.h"],
        copts = COMMON_COPTS + ["-D" + define for define in local_defines],
    )
    native.cc_library(
        name = "io_" + runtime_mode,
        deps = deps + selects.with_or({
            (":macos", ":ios"): [":platform_macos_cocoa_" + runtime_mode],
            "//conditions:default": [],
        }),
        srcs = srcs,
        hdrs = ["runtime/include/bin/dart_io_api.h"],
        copts = COMMON_COPTS,
        linkopts = selects.with_or({
            (":macos", ":ios"): [
                "-framework CoreFoundation",
                "-framework Security",
                "-framework Foundation",
            ],
            "//conditions:default": [],
        }) + selects.with_or({
            (":macos"): [
                "-framework CoreServices",
            ],
            "//conditions:default": [],
        }),
        local_defines = local_defines,
    )
    # if (is_ios || is_mac) {
    #  sources += [
    #    "platform_macos_cocoa.h",
    #    "platform_macos_cocoa.mm",
    #  ]
    # }

    # if (is_linux || is_win || is_fuchsia) {
    #  if (dart_use_fallback_root_certificates) {
    #    sources += [ "//third_party/root_certificates/root_certificates.cc" ]
    #  } else {
    #    defines += [ "DART_IO_ROOT_CERTS_DISABLED" ]
    #  }
    # }

def aot_snapshot(name, script, srcs = [], format = "assembly", sources = []):
    run_from_source = name == "gen_kernel"
    sources = sources + ["package:ffi/ffi.dart"]
    if script not in srcs:
        srcs = srcs + [script]
    gen_kernel_tools = []
    gen_kernel_cmd = []
    gen_kernel_srcs = srcs + [
        "@dart-sdk//:platform_product.dill",
        "@dart-sdk//:.dart_tool/package_config.json",
    ]
    if run_from_source:
        gen_kernel_srcs = gen_kernel_srcs + ["@dart-sdk//:gen_kernel_sources"]
        gen_kernel_cmd += [
            "$(location @dart-sdk//:tools/sdks/dart-sdk/bin/dart)",
            "--packages=$(location @dart-sdk//:.dart_tool/package_config.json)",
            "$(location @dart-sdk//:pkg/vm/bin/gen_kernel.dart)",
        ]
        gen_kernel_tools.append("@dart-sdk//:tools/sdks/dart-sdk/bin/dart")
        gen_kernel_tools.append("@dart-sdk//:pkg/vm/bin/gen_kernel.dart")
    else:
        gen_kernel_cmd.append("$(location @dart-sdk//:gen_kernel)")
        gen_kernel_tools.append("@dart-sdk//:gen_kernel")

    if len(sources) > 0:
        if not run_from_source:
            gen_kernel_srcs += ["@dart-sdk//:gen_kernel_sources"]
        for source in sources:
            gen_kernel_cmd += [
                "--source",
                source,
            ]

    native.genrule(
        name = name + "_aot_dill",
        srcs = gen_kernel_srcs,
        outs = [
            name + ".aot.dill",
        ],
        cmd = " ".join(gen_kernel_cmd + [
            "--packages=$(location @dart-sdk//:.dart_tool/package_config.json)",
            "--platform",
            "$(location @dart-sdk//:platform_product.dill)",
            "--aot",
            "-Ddart.vm.product=true",
            "--sound-null-safety",
            "-o",
            "$(location %s.aot.dill)" % (name),
            "$(location %s)" % (script),
        ]),
        tools = gen_kernel_tools,
    )

    native.genrule(
        name = name + "_dill",
        srcs = gen_kernel_srcs,
        outs = [
            name + ".dill",
        ],
        cmd = " ".join(gen_kernel_cmd + [
            "--packages=$(location @dart-sdk//:.dart_tool/package_config.json)",
            "--platform",
            "$(location @dart-sdk//:platform_product.dill)",
            "--no-link-platform",
            "--sound-null-safety",
            "--filesystem-scheme",
            "root",
            "--filesystem-root",
            "$(@D)",
            "--filesystem-root",
            "$(RULEDIR)",
            "--filesystem-root",
            "$(location %s)/.." % (script),
            "-o",
            "$(location %s.dill)" % (name),
            "root:///%s" % (script),
        ]),
        tools = gen_kernel_tools,
    )

    native.genrule(
        name = name + "_exports_to_c",
        srcs = [
            name + ".aot.dill",
        ],
        outs = [
            name + ".h",
            name + ".cc",
        ],
        cmd = " ".join([
            "$(location @dart-sdk//:dump_exports_exe)",
            "$(location " + name + ".aot.dill)",
            name,
            "$(RULEDIR)/" + name + "",
        ]),
        tools = [
            "@dart-sdk//:tools/sdks/dart-sdk/bin/dart",
            "@dart-sdk//:dump_exports_exe",
        ],
    )

    native.genrule(
        name = name + "_assembly",
        srcs = [name + ".aot.dill"],
        outs = [name + ".S"],
        cmd = select({
            "@dart-sdk//:linux_x86_64": "$(location @dart-sdk//:linux_x86_64/gen_snapshot)",
            "@dart-sdk//:android_arm32": "$(location @dart-sdk//:android_arm32/gen_snapshot)",
            "@dart-sdk//:android_arm64": "$(location @dart-sdk//:android_arm64/gen_snapshot)",
            "@dart-sdk//:android_x86_64": "$(location @dart-sdk//:android_x86_64/gen_snapshot)",
            "@dart-sdk//:macos_arm64": "$(location @dart-sdk//:macos_arm64/gen_snapshot)",
            "@dart-sdk//:ios_arm64": "$(location @dart-sdk//:ios_arm64/gen_snapshot)",
        }) + " " + " ".join([
            "--snapshot-kind=app-aot-assembly",
            "--assembly=$@",
            "$<",
        ]),
        tools = select({
            "@dart-sdk//:linux_x86_64": ["@dart-sdk//:linux_x86_64/gen_snapshot"],
            "@dart-sdk//:android_arm32": ["@dart-sdk//:android_arm32/gen_snapshot"],
            "@dart-sdk//:android_arm64": ["@dart-sdk//:android_arm64/gen_snapshot"],
            "@dart-sdk//:android_x86_64": ["@dart-sdk//:android_x86_64/gen_snapshot"],
            "@dart-sdk//:macos_arm64": ["@dart-sdk//:macos_arm64/gen_snapshot"],
            "@dart-sdk//:ios_arm64": ["@dart-sdk//:ios_arm64/gen_snapshot"],
        }),
    )

    native.cc_library(
        name = name + "_jit_clib",
        srcs = [
            name + ".cc",
            "@dart-sdk//:runtime/bin/simple_jit_embedder.cc",
            "@dart-sdk//:runtime/bin/simple_embedder.h",
        ],
        hdrs = [
            name + ".h",
        ],
        data = [
            "@dart-sdk//:platform_product.dill",
            name + ".dill",
        ],
        tags = ["swift_module=" + name.capitalize() + "CLib"],
        deps = [
            "@dart-sdk//:libdart_release_jit",
        ],
    )

    native.cc_library(
        name = name + "_clib",
        srcs = [
            name + ".cc",
            name + ".S",
            "@dart-sdk//:runtime/bin/simple_aot_embedder.cc",
            "@dart-sdk//:runtime/bin/simple_aot_embedder.h",
        ],
        hdrs = [
            name + ".h",
        ],
        tags = ["swift_module=" + name.capitalize() + "CLib"],
        deps = [
            "@dart-sdk//:libdart_product_aot",
        ],
    )

def aot_binary(name, script, srcs = []):
    aot_snapshot(
        name = name,
        script = script,
        srcs = srcs,
    )
    native.cc_binary(
        name = name,
        srcs = [
            "@dart-sdk//:runtime/bin/trivial_aot_main.cc",
            name + ".S",
        ],
        copts = COMMON_COPTS,
        deps = [
            "@dart-sdk//:libdart_product_aot",
        ],
    )

def libdart(runtime_mode = "release", compiler = "jit"):
    local_defines = []
    if runtime_mode == "product":
        local_defines.append("PRODUCT")
    if compiler == "aot":
        local_defines.append("DART_PRECOMPILED_RUNTIME")
    elif compiler == "precompiler":
        local_defines.append("DART_PRECOMPILER")
    local_defines.append("EXCLUDE_CFE_AND_KERNEL_PLATFORM")
    local_defines.append("EXCLUDE_DARTDEV")

    srcs = DART_EMBEDDER_API_SOURCES
    if runtime_mode != "product":
        srcs = srcs + prefix("runtime/bin", [
            "vmservice_impl.cc",
            "vmservice_impl.h",
        ])

    native.cc_library(
        name = "libdart_%s_%s" % (runtime_mode, compiler),
        srcs = srcs,
        hdrs = VM_API_HEADERS + hdrs_from(DART_EMBEDDER_API_SOURCES),
        copts = COMMON_COPTS,
        defines = local_defines,
        includes = [
            "runtime",
        ],
        deps = [
            "@dart-sdk//:io_" + runtime_mode,
            "@dart-sdk//:vm_" + runtime_mode + "_" + compiler,
        ],
    )

#

def _target_defines_impl(ctx):
    context = cc_common.create_compilation_context(
        defines = depset(ctx.build_setting_value),
    )
    return [CcInfo(compilation_context = context)]

target_defines = rule(
    implementation = _target_defines_impl,
    build_setting = config.string_list(flag = False),
)

def _dart_target_configuration_implementation(ctx):
    return [ctx.attr._target_defines[CcInfo]]

dart_target_configuration = rule(
    implementation = _dart_target_configuration_implementation,
    attrs = {"_target_defines": attr.label(default = "//:target_defines")},
)

_ARCH_MAPPING = {
    "x86_32": "ia32",
    "x86_64": "x64",
    "arm32": "arm",
    "arm64": "arm64",
}

def _add_target_defines_impl(settings, attr):
    defines = [
        "TARGET_ARCH_" + _ARCH_MAPPING[attr.target_cpu].upper(),
    ]
    if attr.target_os == "ios":
        defines += [
            "DART_TARGET_OS_MACOS_IOS",
            "DART_TARGET_OS_MACOS",
        ]
    else:
        defines.append(
            "DART_TARGET_OS_" + attr.target_os.upper(),
        )
    return {"//:target_defines": defines}

_add_target_defines = transition(
    implementation = _add_target_defines_impl,
    inputs = [],
    outputs = ["//:target_defines"],
)

def _with_target_defines_implementation(ctx):
    out = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = out,
        target_file = ctx.file.binary,
        is_executable = True,
    )
    files = depset(direct = [out])
    runfiles = ctx.runfiles(files = [out])
    return [DefaultInfo(files = files, runfiles = runfiles, executable = out)]

with_target_defines = rule(
    implementation = _with_target_defines_implementation,
    cfg = _add_target_defines,
    attrs = {
        "binary": attr.label(mandatory = True, allow_single_file = True),
        "target_cpu": attr.string(mandatory = True),
        "target_os": attr.string(mandatory = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
