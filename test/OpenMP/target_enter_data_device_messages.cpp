// RUN: %clang_cc1 -triple x86_64-apple-macos10.7.0 -verify -fopenmp -ferror-limit 100 -std=c++11 -o - %s

void foo() {
}

bool foobool(int argc) {
  return argc;
}

struct S1; // expected-note {{declared here}} expected-note {{forward declaration of 'S1'}}

extern S1 v1;

struct S2{
  int f;
  operator int() { return f; } // expected-note {{conversion to integral type 'int'}}
  operator bool() { return f; } // expected-note {{conversion to integral type 'bool'}}
} v2;

struct S3 {
  int f;
  explicit operator int() { return f; } // expected-note {{conversion to integral type 'int'}}
} v3;

int main(int argc, char **argv) {
  #pragma omp target enter data device // expected-error {{expected '(' after 'device'}} expected-error {{expected expression}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device ( // expected-error {{expected expression}} expected-error {{expected ')'}} expected-note {{to match this '('}}  expected-error {{expected at least one map clause}}
  #pragma omp target enter data device () // expected-error {{expected expression}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (argc // expected-error {{expected ')'}} expected-note {{to match this '('}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (argc > 0 ? argv[1] : argv[2]) // expected-error {{statement requires expression of integer type ('char *' invalid)}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (foobool(argc)) device(3) // expected-error {{directive '#pragma omp target enter data' cannot contain more than one 'device' clause}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (S1) // expected-error {{'S1' does not refer to a value}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (argv[1]=2) // expected-error {{expected ')'}} expected-note {{to match this '('}} expected-error {{statement requires expression of integer type ('char *' invalid)}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (v1) // expected-error {{expression has incomplete type 'S1'}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (v2) // expected-error {{multiple conversions from expression type 'struct S2' to an integral or enumeration type}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (v3) // expected-error {{expression type 'struct S3' requires explicit conversion to 'int'}} expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (0) // expected-error {{expected at least one map clause}}
  #pragma omp target enter data device (-1) // expected-error {{expression is not a positive integer value}} expected-error {{expected at least one map clause}}
  foo();

  return 0;
}
