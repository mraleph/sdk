// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library builtin;

import 'dart:async';
import 'dart:collection' hide LinkedList, LinkedListEntry;
import 'dart:_internal' hide Symbol;
import 'dart:io';
import 'dart:convert';
import 'dart:isolate';
import 'dart:typed_data';

// Embedder sets this to true if the --trace-loading flag was passed on the
// command line.
bool _traceLoading = false;

// Before handling an embedder entrypoint we finalize the setup of the
// dart:_builtin library.
bool _setupCompleted = false;

// 'print' implementation.
// The standalone embedder registers the closurized _print function with the
// dart:core library.
@pragma("vm:external-name", "Builtin_PrintString")
external void _printString(String s);

void _print(arg) {
  _printString(arg.toString());
}

@pragma("vm:entry-point")
_getPrintClosure() => _print;

// The current working directory when the embedder was launched.
late Uri _workingDirectory;

// packageConfig specified for the isolate.
Uri? _packageConfigUri;

// Error string set if there was an error resolving package configuration.
// For example not finding a .packages file or packages/ directory, malformed
// .packages file or any other related error.
String? _packageError = null;

// The map describing how certain package names are mapped to Uris.
Map<String, Uri>? _packageMap = null;

// Special handling for Windows paths so that they are compatible with URI
// handling.
// Embedder sets this to true if we are running on Windows.
@pragma("vm:entry-point")
bool _isWindows = false;

// Logging from builtin.dart is prefixed with a '*'.
String _logId = (Isolate.current.hashCode % 0x100000).toRadixString(16);
_log(msg) {
  _print("* $_logId $msg");
}

_sanitizeWindowsPath(path) {
  // For Windows we need to massage the paths a bit according to
  // http://blogs.msdn.com/b/ie/archive/2006/12/06/file-uris-in-windows.aspx
  //
  // Convert
  // C:\one\two\three
  // to
  // /C:/one/two/three

  if (_isWindows == false) {
    // Do nothing when not running Windows.
    return path;
  }

  var fixedPath = "${path.replaceAll('\\', '/')}";

  if ((path.length > 2) && (path[1] == ':')) {
    // Path begins with a drive letter.
    return '/$fixedPath';
  }

  return fixedPath;
}

// Given a uri with a 'package' scheme, return a Uri that is prefixed with
// the package root or resolved relative to the package configuration.
Uri? _resolvePackageUri(Uri uri) {
  assert(uri.isScheme("package"));

  if (_packageMap == null) {
    return null;
  }

  if (uri.host.isNotEmpty) {
    var path = '${uri.host}${uri.path}';
    var right = 'package:$path';
    var wrong = 'package://$path';

    throw "URIs using the 'package:' scheme should look like "
        "'$right', not '$wrong'.";
  }

  var packageNameEnd = uri.path.indexOf('/');
  if (packageNameEnd == 0) {
    // Package URIs must have a non-empty package name (not start with "/").
    throw "URIS using the 'package:' scheme should look like "
        "'package:packageName${uri.path}', not 'package:${uri.path}'";
  }
  if (_traceLoading) {
    _log('Resolving package with uri path: ${uri.path}');
  }
  var resolvedUri;
  final error = _packageError;
  if (error != null) {
    if (_traceLoading) {
      _log("Resolving package with pending resolution error: $error");
    }
    throw error;
  } else {
    if (packageNameEnd < 0) {
      // Package URIs must have a path after the package name, even if it's
      // just "/".
      throw "URIS using the 'package:' scheme should look like "
          "'package:${uri.path}/', not 'package:${uri.path}'";
    }
    var packageName = uri.path.substring(0, packageNameEnd);
    final mapping = _packageMap![packageName];
    if (_traceLoading) {
      _log("Mapped '$packageName' package to '$mapping'");
    }
    if (mapping == null) {
      throw "No mapping for '$packageName' package when resolving '$uri'.";
    }
    var path;
    assert(uri.path.length > packageName.length);
    path = uri.path.substring(packageName.length + 1);
    if (_traceLoading) {
      _log("Path to be resolved in package: $path");
    }
    resolvedUri = mapping.resolve(path);
  }
  if (_traceLoading) {
    _log("Resolved '$uri' to '$resolvedUri'.");
  }
  return resolvedUri;
}

void _ensurePackageMapIsLoaded() {
  if (_packageConfigUri == null ||
      _packageMap != null ||
      _packageError != null) {
    return;
  }

  try {
    _packageMap = _loadPackageConfig(_packageConfigUri!);
    if (_traceLoading) {
      _log("Setup package map: $_packageMap");
    }
  } catch (e, st) {
    if (_traceLoading) {
      _log("Failed to load package config: $e at $st");
    }
    // Remember the error message.
    _packageError = e.toString();
  }
}

// The values go from ' ' to DEL and `x` means disallowed.
const String _invalidPackageNameChars =
    'x.xx.x.........x..........x.x.xx...........................xxxx.x..........................xxx.x';

bool _isValidPackageName(String packageName) {
  const space = 0x20;
  const del = 0x7F;
  const dot = 0x2e;
  const lowerX = 0x78;
  for (int i = 0; i < packageName.length; ++i) {
    final int char = packageName.codeUnitAt(i);
    if (char < space || del < char) {
      return false;
    }
    final int allowed = _invalidPackageNameChars.codeUnitAt(char - space);
    assert(allowed == dot || allowed == lowerX);
    if (allowed == lowerX) {
      return false;
    }
  }
  return true;
}

// The .dart_tool/package_config.json format is described in
//
// https://github.com/dart-lang/language/blob/master/accepted/future-releases/language-versioning/package-config-file-v2.md
//
// The returned list has the format:
//
//    [0] Location of package_config.json file.
//    [1] null
//    [n*2] Name of n-th package
//    [n*2 + 1] Location of n-th package's sources (as a String)
//
Map<String, Uri> _parsePackageConfig(Uri packageConfig, String data) {
  final Map packageJson = json.decode(data);
  final version = packageJson['configVersion'];
  if (version != 2) {
    throw 'The package configuration file has an unsupported version.';
  }
  final result = <String, Uri>{};
  final List packages = packageJson['packages'] ?? [];
  for (final Map package in packages) {
    String rootUri = package['rootUri'];
    if (!rootUri.endsWith('/')) rootUri += '/';
    final String packageName = package['name'];
    final String? packageUri = package['packageUri'];
    final Uri resolvedRootUri = packageConfig.resolve(rootUri);
    final Uri resolvedPackageUri = packageUri != null
        ? resolvedRootUri.resolve(packageUri)
        : resolvedRootUri;
    if (packageUri != null &&
        !'$resolvedPackageUri'.contains('$resolvedRootUri')) {
      throw 'The resolved "packageUri" is not a subdirectory of the "rootUri".';
    }
    if (!_isValidPackageName(packageName)) {
      throw 'Package name in $packageConfig contains disallowed characters ('
          'was: "$packageName")';
    }
    result[packageName] = resolvedPackageUri;
    if (_traceLoading) {
      _log('Resolved package "$packageName" to be at $resolvedPackageUri');
    }
  }
  return result;
}

bool _isValidUtf8DataUrl(UriData data) {
  final mime = data.mimeType;
  if (mime != "text/plain") {
    return false;
  }
  final charset = data.charset;
  if (charset != "utf-8" && charset != "US-ASCII") {
    return false;
  }
  return true;
}

Map<String, Uri> _loadPackageConfig(Uri packageConfig) {
  if (_traceLoading) {
    _log("Handling load of packages map: '$packageConfig'.");
  }
  late Uint8List bytes;
  if (!packageConfig.hasScheme || packageConfig.isScheme('file')) {
    final file = File.fromUri(packageConfig);
    if (!file.existsSync()) {
      throw "Packages file '$packageConfig' does not exit.";
    }
    bytes = file.readAsBytesSync();
  } else if (packageConfig.isScheme('data')) {
    final uriData = packageConfig.data!;
    if (!_isValidUtf8DataUrl(uriData)) {
      throw "The data resource '$packageConfig' must have a "
          "'text/plain' mime type and a 'utf-8' or 'US-ASCII' charset.";
    }
    bytes = uriData.contentAsBytes();
  } else {
    throw "Unknown scheme (${packageConfig.scheme}) for package file at "
        "'$packageConfig'.";
  }

  try {
    final data = utf8.decode(bytes);
    return _parsePackageConfig(packageConfig, data);
  } catch (e) {
    throw "The resource '$packageConfig' is not a valid "
        "'.dart_tool/package_config.json' file.";
  }
}

// Embedder Entrypoint:
// The embedder calls this method to initial the package resolution state.
@pragma("vm:entry-point")
void _setPackageConfig(String? packageConfig) {
  _setupHooks();
  if (_traceLoading) {
    _log('Setting package config: $packageConfig');
  }
  // If the --packages flag was passed, setup _packagesConfig.
  if (packageConfig != null) {
    _packageMap = null;
    var packagesName = _sanitizeWindowsPath(packageConfig);
    var packagesUri = Uri.parse(packagesName);
    if (!packagesUri.hasScheme) {
      // Script does not have a scheme, assume that it is a path,
      // resolve it against the working directory.
      packagesUri = _workingDirectory.resolveUri(packagesUri);
    }
    _packageConfigUri = packagesUri;
  }
}

// Embedder Entrypoint:
// The embedder calls this method with the current working directory.
@pragma("vm:entry-point")
void _setWorkingDirectory(String cwd) {
  _setupHooks();
  if (_traceLoading) {
    _log('Setting working directory: $cwd');
  }
  _workingDirectory = Uri.directory(cwd);
  if (_traceLoading) {
    _log('Working directory URI: $_workingDirectory');
  }
}

// Embedder Entrypoint:
// The embedder calls this method with the value of the --packages command line
// option. It has to point to ".dart_tool/package_config.json" file.
@pragma("vm:entry-point")
String _setPackagesMap(String packagesParam) {
  if (!_setupCompleted) {
    _setupHooks();
  }

  // First convert the packages parameter from the command line to a URI which
  // can be handled by the loader code.
  if (_traceLoading) {
    _log("Resolving packages map: $packagesParam");
  }
  var packagesName = _sanitizeWindowsPath(packagesParam);
  var packagesUri = Uri.parse(packagesName);
  if (!packagesUri.hasScheme) {
    // Script does not have a scheme, assume that it is a path,
    // resolve it against the working directory.
    packagesUri = _workingDirectory.resolveUri(packagesUri);
  }
  var packagesUriStr = packagesUri.toString();
  VMLibraryHooks.packageConfigString = packagesUriStr;
  if (_traceLoading) {
    _log('Resolved packages map to: $packagesUri');
  }
  return packagesUriStr;
}

// Resolves the script uri in the current working directory iff the given uri
// did not specify a scheme (e.g. a path to a script file on the command line).
@pragma("vm:entry-point")
String _resolveScriptUri(String scriptName) {
  if (_traceLoading) {
    _log("Resolving script: $scriptName");
  }
  scriptName = _sanitizeWindowsPath(scriptName);

  var scriptUri = Uri.parse(scriptName);
  if (!scriptUri.hasScheme) {
    // Script does not have a scheme, assume that it is a path,
    // resolve it against the working directory.
    scriptUri = _workingDirectory.resolveUri(scriptUri);
  }

  if (_traceLoading) {
    _log('Resolved entry point to: $scriptUri');
  }

  return scriptUri.toString();
}

// Register callbacks and hooks with the rest of the core libraries.
@pragma("vm:entry-point")
_setupHooks() {
  if (!_setupCompleted) {
    _setupCompleted = true;
    VMLibraryHooks.packageConfigUriFuture = _getPackageConfigFuture;
    VMLibraryHooks.resolvePackageUriFuture = _resolvePackageUriFuture;
  }
}

Future<Uri?> _getPackageConfigFuture() {
  if (_traceLoading) {
    _log("Request for package config from user code.");
  }
  _ensurePackageMapIsLoaded();
  return Future.value(_packageMap != null ? _packageConfigUri : null);
}

Future<Uri?> _resolvePackageUriFuture(Uri packageUri) {
  if (_traceLoading) {
    _log("Request for package Uri resolution from user code: $packageUri");
  }
  if (!packageUri.isScheme("package")) {
    if (_traceLoading) {
      _log("Non-package Uri, returning unmodified: $packageUri");
    }
    // Return the incoming parameter if not passed a package: URI.
    return Future.value(packageUri);
  }
  _ensurePackageMapIsLoaded();

  if (_packageConfigUri == null) {
    return Future.value(null);
  }
  Uri? resolvedUri;
  try {
    resolvedUri = _resolvePackageUri(packageUri);
  } catch (e, s) {
    if (_traceLoading) {
      _log("Exception when resolving package URI: $packageUri:\n$e\n$s");
    }
    resolvedUri = null;
  }
  if (_traceLoading) {
    _log("Resolved '$packageUri' to '$resolvedUri'");
  }
  return Future.value(resolvedUri);
}
