import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:async';

const HEADER = 'Content-Length: ';
const HEADER_END = '\n\r\n';
const CH0 = 48;
const CH9 = 57;
const CR = 13;
const LF = 10;

class Parser {
  final StringBuffer content = StringBuffer();
  int contentLength = 0;
  int index = 0;
  int pos = 0;
  var state = Parser.readHeader;
  final StreamController<Map<String, dynamic>> events = StreamController<Map<String, dynamic>>();

  Stream<Map<String, dynamic>> get stream => events.stream;

  void add(String data) {
    pos = 0;
    while (pos < data.length) {
      state = state(this, data);
    }
  }

  static readHeader(Parser p, String data) {
    final int codeUnit = data.codeUnitAt(p.pos++);

    if (HEADER.codeUnitAt(p.index) != codeUnit) {
      throw 'Unexpected codeUnit: ${String.fromCharCode(codeUnit)} expected ${HEADER[p.index]}';
    }

    p.index++;
    if (p.index == HEADER.length) {
      p.index = 0;
      return Parser.readLength;
    }
    return Parser.readHeader;
  }

  static readLength(Parser p, String data) {
    final int codeUnit = data.codeUnitAt(p.pos++);

    if (codeUnit == CR) {
      p.contentLength = p.index;
      p.index = 0;
      return Parser.readHeaderEnd;
    }
    if (codeUnit < CH0 || codeUnit > CH9)
      throw 'Unexpected codeUnit: ${String.fromCharCode(codeUnit)} expected 0 to 9';
    p.index = p.index * 10 + (codeUnit - CH0);
    return Parser.readLength;
  }

  static readHeaderEnd(Parser p, String data) {
    final int codeUnit = data.codeUnitAt(p.pos++);

    if (HEADER_END.codeUnitAt(p.index) != codeUnit) {
      throw 'Unexpected codeUnit: ${String.fromCharCode(codeUnit)} expected ${HEADER_END[p.index]}';
    }

    p.index++;
    if (p.index == HEADER_END.length) {
      return Parser.readContent;
    }
    return Parser.readHeaderEnd;
  }

  static readContent(Parser p, String data) {
    final availableBytes = data.length - p.pos;
    final bytesToRead = math.min(availableBytes, p.contentLength);
    p.content.write(data.substring(p.pos, p.pos + bytesToRead));
    p.contentLength -= bytesToRead;
    p.pos += bytesToRead;
    if (p.contentLength == 0) {
      p.events.add(jsonDecode(p.content.toString()));
      p.content.clear();
      p.index = 0;
      return Parser.readHeader;
    } else {
      return Parser.readContent;
    }
  }
}

void main() async {
  final cquery = await Process.start(
      '/Users/vegorov/src/temp/cquery/build/release/bin/cquery',
      ['--language-server']);

  final p = Parser();
  cquery.stdout.transform(utf8.decoder).listen(p.add);
  cquery.stderr.transform(utf8.decoder).listen((data) => print('stderr: ${data}'));

  send(Map<String, dynamic> data) {
    data['jsonrpc'] = '2.0';


    final rq = jsonEncode(data);
    print('requesting: ${rq}');
    cquery.stdin.write('Content-Length: ${rq.length}\r\n\r\n${rq}');
  }

  final pending = <int, Completer<dynamic>>{};
  int rqId = 1;

  Future<dynamic> invoke(String method, {Map<String, dynamic> params, bool noResult: false}) {
    final rq = {'method': method, 'id': rqId++};
    if (!noResult) {
      pending[rq['id']] = Completer();
    }
    if (params != null) rq['params'] = params;
    send(rq);
    return !noResult ? pending[rq['id']].future : null;
  }

  p.stream.listen((data) {
    final method = data['method'];
    final params = data['params'];

    if (method == 'textDocument/publishDiagnostics' && data['params']['diagnostics'].isEmpty) return;

    if (data.containsKey('id') && data.containsKey('result')) {
      final id = data['id'];
      final result = data['result'];
      pending[id].complete(result);
      pending[id] = null;
      return;
    }

    print(data);
  });

  await invoke('initialize', params: {
        'processId': 123,
        'rootUri': 'file:///Users/vegorov/src/dart/sdk',
        'capabilities': {
          'textDocument': {
            'codeLens': null
          }
        },
        'trace': 'off',
        'initializationOptions': {
            'cacheDirectory': '/tmp/cquery-cache',
            'progressReportFrequencyMs': 5000,
        },
        'workspaceFolders': [
          {
            'uri': 'file:///Users/vegorov/src/dart/sdk/runtime/vm',
            'name': 'vm',
          }
        ]
    });

  invoke(r'$cquery/wait', noResult: true);

  // Send request.
  final symbols = await invoke('textDocument/documentSymbol', params:
    {
      'textDocument': {
        'uri': 'file:///Users/vegorov/src/dart/sdk/runtime/vm/object.h',
      },
    }
  );

  print('Got back ${symbols.length} symbols');
  for (var symbol in symbols) {
    if (symbol['kind'] == 5 && symbol['name'] == 'String') {
      print(symbol);
    }
  }
}
