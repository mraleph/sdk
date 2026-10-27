#!/usr/bin/env bash
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

# Prepares a copy of a Flutter checkout for parallel_analysis benchmarks, see
# parallel_analysis_README.md. The checkout itself is not changed.
#
# Usage: parallel_analysis_flutter.sh [flutter checkout] [destination]

set -euo pipefail

FLUTTER_SRC="${1:-$HOME/src/flutter/flutter}"
DEST="${2:-${TMPDIR:-/tmp}/parallel_analysis/flutter}"

ENGINE_STAMP="$FLUTTER_SRC/bin/cache/engine.stamp"
FLUTTER_DART="$FLUTTER_SRC/bin/cache/dart-sdk/bin/dart"
if [ ! -f "$ENGINE_STAMP" ] || [ ! -x "$FLUTTER_DART" ]; then
  echo "Run '$FLUTTER_SRC/bin/flutter --version' once to populate bin/cache." >&2
  exit 1
fi

echo "Copying $FLUTTER_SRC to $DEST"
mkdir -p "$DEST"
rsync -a --delete --exclude /.git --exclude /bin/cache "$FLUTTER_SRC/" "$DEST/"

# `package:flutter` depends on `sky_engine`, which provides `dart:ui`.
ENGINE="$(cat "$ENGINE_STAMP")"
mkdir -p "$DEST/bin/cache/pkg"
cp "$ENGINE_STAMP" "$DEST/bin/cache/"
if [ -f "$FLUTTER_SRC/bin/cache/flutter.version.json" ]; then
  cp "$FLUTTER_SRC/bin/cache/flutter.version.json" "$DEST/bin/cache/"
fi
echo "Downloading sky_engine for engine $ENGINE"
curl -fsSL -o "$DEST/bin/cache/sky_engine.zip" \
  "https://storage.googleapis.com/flutter_infra_release/flutter/$ENGINE/sky_engine.zip"
rm -rf "$DEST/bin/cache/pkg/sky_engine"
unzip -q -o "$DEST/bin/cache/sky_engine.zip" -d "$DEST/bin/cache/pkg"

echo "Resolving the workspace"
(cd "$DEST" && FLUTTER_ROOT="$DEST" PUB_ENVIRONMENT=flutter_cli \
  "$FLUTTER_DART" pub get)

echo "Done, analyze $DEST/packages"
