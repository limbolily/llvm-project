// RUN: %clang_cc1 -analyze -analyzer-checker=core,unix.Malloc,alpha.cplusplus.NewDelete -analyzer-store region -std=c++11 -verify %s

typedef __typeof(sizeof(int)) size_t;
void *malloc(size_t);
void free(void *);

//-------------------------------------------------------------------
// Check that unix.Malloc + alpha.cplusplus.NewDelete does not enable
// warnings produced by unix.MismatchedDeallocator.
//-------------------------------------------------------------------
void testMismatchedDeallocator() {
  int *p = (int *)malloc(sizeof(int));
  delete p;
} // expected-warning{{Memory is never released; potential leak of memory pointed to by 'p'}}
