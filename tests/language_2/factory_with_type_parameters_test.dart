// Copyright (c) 2018, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

class Foo<T> {
  Foo._();

  factory Foo
             <X> //# 01: compile-time error
             <X extends T> //# 02: compile-time error
             () => new Bar<T>();

  factory Foo
             <X> //# 03: compile-time error
             <X extends T> //# 04: compile-time error
             .far
                 <X> //# 05: compile-time error
                 <X extends T> //# 06: compile-time error
                 <X>.fip //# 07: compile-time error
                 <X extends T>.fip //# 08: compile-time error
                 () => new Bar<T>();
}

class Bar<T> extends Foo<T> {
  Bar(): super._() {}
}

main() {
  new Foo<String>();
  new Foo<String>.far();
}
