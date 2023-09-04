// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#if !defined(RUNTIME_BIN_SIMPLE_EMBEDDER)
#define RUNTIME_BIN_SIMPLE_EMBEDDER

#include <utility>

namespace dart::embedder::simple {

void ConnectToEventLoop(void (*notify)(void*));
void ProcessEvents(void* isolate);
void EnterMainIsolate();
void ExitMainIsolate();
void* MainIsolate();

template <typename ExportTable>
class AutoIsolate {
 public:
  AutoIsolate() = default;
  AutoIsolate(const AutoIsolate&) = delete;
  AutoIsolate(AutoIsolate&& o) : entered_(std::exchange(o.entered_, false)) {}

  ~AutoIsolate() {
    if (entered_) {
      ExitMainIsolate();
    }
  }

  const ExportTable* operator->() {
    EnterMainIsolate();
    entered_ = true;
    return static_cast<const ExportTable*>(MainIsolate());
  }

 private:
  bool entered_ = false;
};

template <typename ExportTable>
AutoIsolate<ExportTable> Exports() {
  return {};
}

}  // namespace dart::embedder::simple

#endif  // !defined(RUNTIME_BIN_SIMPLE_EMBEDDER)
