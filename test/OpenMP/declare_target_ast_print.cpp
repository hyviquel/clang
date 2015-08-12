// RUN: %clang_cc1 -verify -fopenmp -ast-print %s | FileCheck %s
// RUN: %clang_cc1 -fopenmp -x c++ -std=c++11 -emit-pch -o %t %s
// RUN: %clang_cc1 -fopenmp -std=c++11 -include-pch %t -fsyntax-only -verify %s -ast-print | FileCheck %s
// expected-no-diagnostics

#ifndef HEADER
#define HEADER

#pragma omp declare target
// CHECK: #pragma omp declare target

void foo() {}
// CHECK-NEXT: void foo()

#pragma omp end declare target
// CHECK: #pragma omp end declare target

extern "C" {
#pragma omp declare target
// CHECK: #pragma omp declare target

void foo_c() {}
// CHECK-NEXT: void foo_c()

#pragma omp end declare target
// CHECK: #pragma omp end declare target
}

extern "C++" {
#pragma omp declare target
// CHECK: #pragma omp declare target

void foo_cpp() {}
// CHECK-NEXT: void foo_cpp()

#pragma omp end declare target
// CHECK: #pragma omp end declare target
}

int main (int argc, char **argv) {
  foo();
  foo_c();
  foo_cpp();
  return (0);
}

#endif
