import 'dart:io';

void main(List<String> args) async {
  final allFiles = Directory('${args[0]}/tests')
      .listSync(recursive: true)
      .whereType<File>()
      .map((e) => e.absolute.path)
      .where((p) => p.endsWith('.dart'))
      .toList();

  final RandomAccessFile raf =
      new File('input.list').openSync(mode: FileMode.write);
  try {
    for (var p in allFiles) raf.writeStringSync('$p\n');
  } finally {
    raf.closeSync();
  }
}
