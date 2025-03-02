// RUN: %clangxx_asan -O0 -mllvm -asan-instrument-dynamic-allocas %s -o %t
// RUN: not %run %t 2>&1 | FileCheck %s
//
// REQUIRES: stable-runtime
// MSVC doesn't support VLAs
// UNSUPPORTED: msvc

#include "defines.h"
#include <assert.h>
#include <stdint.h>

ATTRIBUTE_NOINLINE void foo(int index, int len) {
  if (index > len) {
    char str[len];
    assert(!(reinterpret_cast<uintptr_t>(str) & 31L));
    str[index] = '1'; // BOOM
// CHECK: ERROR: AddressSanitizer: dynamic-stack-buffer-overflow on address [[ADDR:0x[0-9a-f]+]]
// CHECK: WRITE of size 1 at [[ADDR]] thread T0
  }
}

int main(int argc, char **argv) {
  foo(33, 10);
  return 0;
}
