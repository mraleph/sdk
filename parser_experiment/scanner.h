// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef SCANNER_H
#define SCANNER_H

#include <cstdint>
#include <cstdlib>

class Token;

Token* ScanUtf8(uint8_t* bytes, size_t length);

#endif
