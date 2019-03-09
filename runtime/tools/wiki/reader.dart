import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:async';

import 'package:path/path.dart' as p;

abstract class Kind {
  static const Unknown = 0;

  static const File = 1;
  static const Module = 2;
  static const Namespace = 3;
  static const Package = 4;
  static const Class = 5;
  static const Method = 6;
  static const Property = 7;
  static const Field = 8;
  static const Constructor = 9;
  static const Enum = 10;
  static const Interface = 11;
  static const Function = 12;
  static const Variable = 13;
  static const Constant = 14;
  static const String = 15;
  static const Number = 16;
  static const Boolean = 17;
  static const Array = 18;
  static const Object = 19;
  static const Key = 20;
  static const Null = 21;
  static const EnumMember = 22;
  static const Struct = 23;
  static const Event = 24;
  static const Operator = 25;

  // For C++, this is interpreted as "template parameter" (including
  // non-type template parameters).
  static const TypeParameter = 26;

  // cquery extensions
  // See also https://github.com/Microsoft/language-server-protocol/issues/344
  // for new SymbolKind clang/Index/IndexSymbol.h clang::index::SymbolKind
  static const TypeAlias = 252;
  static const Parameter = 253;
  static const StaticMethod = 254;
  static const Macro = 255;

}

final files = <String, int>{};
final filesByIndex = [];
int addFile(String name) => files.putIfAbsent(name, () {
  filesByIndex.add(name);
  return filesByIndex.length - 1;
});



class Klass {
  String name;
  Location loc;
  Map<String, Location> members;

  Klass([this.name]);

  defineMember(String name, Location loc) {
    if (members == null) {
      members = <String, Location>{};
    }
    if (members.containsKey(name) && name == 'BuildGraph') {
      print('Duplicate definition of BuildGraph in ${this.name}');
    }
    members[name] = members.containsKey(name) ? Location.invalid : loc;
  }

  static Klass byUsr(int usr) => classes.putIfAbsent(usr, () => Klass());

  dynamic toJson() {
    final result = [loc?.toJson() ?? 0];
    if (members != null) {
      final res = <String, Location>{};
      members.forEach((key, val) {
        if (val != Location.invalid) {
          res[key] = val;
        }
      });
      if (res.isNotEmpty) {
        result.add(res);
      }
    }
    return result;
  }
}

final classes = <int, Klass>{};
final globals = Klass('\$Globals');

void defineClass(int usr, String name, Location loc) {
  final kl = Klass.byUsr(usr);
  if (kl.name != null && kl.name != "" && kl.name != name) {
    throw "Mismatched names";
  }
  if (name != "")
    kl.name = name;
  if (kl.loc == null) {
    kl.loc = loc;
  } else {
    kl.loc = Location.invalid;
  }
}

class Location {
  final int file;
  final int lineNo;

  const Location._(this.file, this.lineNo);

  Location(String file, this.lineNo) : file = addFile(file);

  toJson() => identical(this, invalid) ? null : '${file}:${lineNo}';

  toString() => '${filesByIndex[file]}:${lineNo}';

  static const invalid = const Location._(-1, -1);
}

shortName(entity) {
  final offset = entity['short_name_offset'];
  final length = entity['short_name_size'] ?? 0;
  final detailedName = entity['detailed_name'];
  if (length == 0) return detailedName;
  return detailedName.substring(offset, offset + length);
}

loadFile(File indexFile) {
  final result = jsonDecode(indexFile.readAsStringSync().split('\n')[1]);
  final sourceFile = p.basenameWithoutExtension(indexFile.path).replaceAll('@', '/');
  if (!sourceFile.startsWith('runtime/')) return;
  //print('Loading index for ${sourceFile}');
  for (var type in result['types']) {
    if (type['kind'] != Kind.Class) continue;
    final extent = type['extent'];
    if (extent == null) continue;

    final detailedName = type['detailed_name'];
    final lineStart = int.parse(extent.substring(0, extent.indexOf(':')));
    defineClass(type['usr'], detailedName, Location(sourceFile, lineStart));
  }
  for (var func in result['funcs']) {
    final kind = func['kind'];
    if (kind != Kind.Method && kind != Kind.StaticMethod) continue;
    final extent = func['extent'];
    if (extent == null) continue;
    final short = shortName(func);
    final lineStart = int.parse(extent.substring(0, extent.indexOf(':')));
    if (func['declaring_type'] is! int) continue;
    Klass.byUsr(result['types'][func['declaring_type']]['usr']).defineMember(short, Location(sourceFile, lineStart));
  }
  for (var func in result['funcs']) {
    final kind = func['kind'];
    if (kind != Kind.Function) continue;
    final extent = func['extent'];
    if (extent == null) continue;
    final short = shortName(func);
    final lineStart = int.parse(extent.substring(0, extent.indexOf(':')));
    globals.defineMember(short, Location(sourceFile, lineStart));
  }
}

Future<String> commitHash() async {
  final results = await Process.run('git', ['rev-parse', 'HEAD']);
  return results.stdout;
}

void main() async {
  final cacheRoot = Directory('/tmp/cquery-cache/@Users@vegorov@src@dart@sdk');
  final files = cacheRoot.listSync().whereType<File>().where((file) => file.path.endsWith('.json')).toList();
  print('${files.length} indexes available');
  files.forEach(loadFile);

  var functions = globals.toJson();
  if (functions.length < 2) functions = []; else functions = functions[1];

  var classesByName = <String, Klass>{};
  classes.forEach((usr, klass) {
    if (klass.name != "" && klass.name != null) {
      classesByName[klass.name] = klass;
    }
  });

  final database = {
    'commit': await commitHash(),
    'files': filesByIndex,
    'classes': classesByName,
    'functions': functions
  };

  File('xref.json').writeAsStringSync(jsonEncode(database));
}