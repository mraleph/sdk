import 'package:native_stack_traces/src/elf.dart';

// Step 1:
//
// ```
// $ buildtools/mac-x64/clang/bin/clang++ -target i686-unknown-linux-gnu --sysroot=buildtools/sysroot/linux -MMD -DNDEBUG -DTARGET_ARCH_ARM -Iruntime -I. -Iruntime/include -fPIE -fcolor-diagnostics -Wall -Wextra -Werror -Wendif-labels -Wno-missing-field-initializers -Wno-unused-parameter -Wno-tautological-constant-compare -Wno-unused-but-set-variable -Wno-deprecated-non-prototype -Wpartial-availability -fdebug-prefix-map=/Users/vegorov/src/dart/sdk/= -no-canonical-prefixes -fvisibility=hidden -Wheader-hygiene -Wstring-conversion -O2 -fno-ident -g3 -ggdb3 -Wno-unused-parameter -Wno-unused-private-field -Wnon-virtual-dtor -Wvla -Woverloaded-virtual -Wno-comments -g3 -ggdb3 -fno-rtti -fno-exceptions -Wimplicit-fallthrough -fno-strict-vtable-pointers -O2 -fvisibility-inlines-hidden -fno-omit-frame-pointer -std=c++17 -std=c++17 -fno-rtti -c runtime/vm/compiler/offsets_extractor.cc -o /tmp/output.o
// ```
//
// Step 2: Extract symbol values from the o file.
//

void main() {
  final elf = Elf.fromFile('/tmp/output.o')!;

  // Verify that we don't have any initializers.
  for (var section in elf.sections) {
    if (section.headerEntry.isExecutable && section.length != 0) {
      throw 'Expected .text section to be empty';
    }
  }

  final rodata = elf.namedSections('.rodata').single;
  for (var symbol in elf.staticSymbols) {
    if (symbol.sectionIndex == rodata.headerEntry.sectionIndex) {
      if (symbol.size == rodata.reader.wordSize) {
        rodata.reader.seek(symbol.value, absolute: true);
        print('${symbol.name} = ${rodata.reader.readWord()}');
      } else if ((symbol.size & (rodata.reader.wordSize - 1)) == 0) {
        // TODO: can use _FirstIndex & _LastIndex to prune the array.
        rodata.reader.seek(symbol.value, absolute: true);
        print('${symbol.name}[] = {');
        for (var i = 0, n = symbol.size / rodata.reader.wordSize; i < n; i++) {
          print('  ${rodata.reader.readWord()},');
        }
        print('}');
      }
    }
  }
}
