// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "rs_bindings_from_cc/test/function/simple/simple_functions.h"

int return_value() { return 42; }

int* return_pointer() {
  static int i = 42;
  return &i;
}

int& return_reference() {
  static int i = 42;
  return i;
}

void take_pointer(int* i) { *i = 42; }

void take_reference(int& i) { i = 42; }

const int& forward_reference(const int& i) { return i; }
