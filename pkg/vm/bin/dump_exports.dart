// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';
import 'dart:io';

import 'package:kernel/ast.dart';
import 'package:kernel/core_types.dart';
import 'package:kernel/external_name.dart';
import 'package:kernel/kernel.dart' show Component, writeComponentToText;
import 'package:kernel/binary/ast_from_binary.dart'
    show BinaryBuilderWithMetadata;

import 'package:vm/metadata/direct_call.dart' show DirectCallMetadataRepository;
import 'package:vm/metadata/inferred_type.dart'
    show InferredTypeMetadataRepository;
import 'package:vm/metadata/procedure_attributes.dart'
    show ProcedureAttributesMetadataRepository;
import 'package:vm/metadata/table_selector.dart'
    show TableSelectorMetadataRepository;
import 'package:vm/metadata/unboxing_info.dart'
    show UnboxingInfoMetadataRepository;
import 'package:vm/metadata/unreachable.dart'
    show UnreachableNodeMetadataRepository;
import 'package:vm/metadata/call_site_attributes.dart'
    show CallSiteAttributesMetadataRepository;
import 'package:vm/metadata/loading_units.dart'
    show LoadingUnitsMetadataRepository;

final String _usage = '''
Usage: dump_exports input.dill name [output]
Dumps kernel binary file with VM-specific metadata.
''';

main(List<String> arguments) async {
  final input = arguments[0];
  final name = arguments[1];
  final output = arguments.length == 3 ? arguments[2] : null;

  final component = new Component();

  // Register VM-specific metadata.
  component.addMetadataRepository(new DirectCallMetadataRepository());
  component.addMetadataRepository(new InferredTypeMetadataRepository());
  component.addMetadataRepository(new ProcedureAttributesMetadataRepository());
  component.addMetadataRepository(new TableSelectorMetadataRepository());
  component.addMetadataRepository(new UnboxingInfoMetadataRepository());
  component.addMetadataRepository(new UnreachableNodeMetadataRepository());
  component.addMetadataRepository(new CallSiteAttributesMetadataRepository());
  component.addMetadataRepository(new LoadingUnitsMetadataRepository());

  final List<int> bytes = new File(input).readAsBytesSync();
  new BinaryBuilderWithMetadata(bytes).readComponent(component);

  final coreTypes = CoreTypes(component);

  Iterable<String> named(Map<String, dynamic> e) sync* {
    final p = e['p'] as List;
    final n = e['pn'] as List;
    for (var i = 0; i < p.length; i++) {
      yield '${p[i]} ${n[i]}';
    }
  }

  for (var p
      in component.mainMethod?.enclosingLibrary.procedures ?? <Procedure>[]) {
    if (p.name.text == '#ffiExports') {
      final exports =
          (jsonDecode(getPragmaOptions(coreTypes, p, 'vm:ffi:exports-list')!)
                  as List)
              .cast<Map<String, dynamic>>();

      final fields = [
        for (var e in exports) '  ${e['r']} (*${e['n']})(${e['p'].join(', ')});'
      ].join('\n');

      final funcs = [
        for (var e in exports)
          '''
${e['r']} ${name}_${e['n']}(${named(e).join(', ')}) {
  return dart::embedder::simple::Exports<${name}_Exports>()->${e['n']}(${e['pn'].join(', ')});
}
'''
      ].join('\n\n');

      final funcDecls = [
        for (var e in exports)
          '''
${e['r']} ${name}_${e['n']}(${named(e).join(', ')});
'''
      ].join('\n\n');

      final ccContent = '''
#include "bin/simple_embedder.h"

namespace {

struct ${name}_Exports {
  $fields
};

};

extern "C" {

$funcs

void ${name}_Configure(const char* platform_path, const char* app_path) {
  dart::embedder::simple::Configure(platform_path, app_path);
}

void ${name}_ConnectToEventLoop(void (*notify) (void*)) {
  dart::embedder::simple::ConnectToEventLoop(notify);
}

void ${name}_ProcessEvents(void* isolate) {
  dart::embedder::simple::ProcessEvents(isolate);
}

}
''';

      final hContent = '''
#ifdef __cplusplus
extern "C" {
#endif

$funcDecls

void ${name}_Configure(const char* platform_path, const char* app_path);

void ${name}_ConnectToEventLoop(void (*notify) (void*));
void ${name}_ProcessEvents(void* isolate);

#ifdef __cplusplus
}  // extern "C"
#endif
''';

      if (output != null) {
        File(output + ".h").writeAsStringSync(hContent);
        File(output + ".cc").writeAsStringSync(ccContent);
      } else {
        print(hContent);
        print(ccContent);
      }
      break;
    }
  }
}
