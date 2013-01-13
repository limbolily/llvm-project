// RUN: %clang_cc1 -triple "i686-unknown-unknown"   -emit-llvm -x c %s -o - -O3 | FileCheck %s
// RUN: %clang_cc1 -triple "x86_64-unknown-unknown" -emit-llvm -x c %s -o - -O3 | FileCheck %s
// RUN: %clang_cc1 -triple "x86_64-mingw32"         -emit-llvm -x c %s -o - -O3 | FileCheck %s

unsigned short test_addcs(unsigned short x, unsigned short y,
                          unsigned short carryin, unsigned short *z) {
  // CHECK: @test_addcs
  // CHECK: %{{.+}} = {{.*}} call { i16, i1 } @llvm.uadd.with.overflow.i16(i16 %x, i16 %y)
  // CHECK: %{{.+}} = extractvalue { i16, i1 } %{{.+}}, 1
  // CHECK: %{{.+}} = extractvalue { i16, i1 } %{{.+}}, 0
  // CHECK: %{{.+}} = {{.*}} call { i16, i1 } @llvm.uadd.with.overflow.i16(i16 %{{.+}}, i16 %carryin)
  // CHECK: %{{.+}} = extractvalue { i16, i1 } %{{.+}}, 1
  // CHECK: %{{.+}} = extractvalue { i16, i1 } %{{.+}}, 0
  // CHECK: %{{.+}} = or i1 %{{.+}}, %{{.+}}
  // CHECK: %{{.+}} = zext i1 %{{.+}} to i16
  // CHECK: store i16 %{{.+}}, i16* %z, align 2

  unsigned short carryout;
  *z = __builtin_addcs(x, y, carryin, &carryout);

  return carryout;
}

unsigned test_addc(unsigned x, unsigned y, unsigned carryin, unsigned *z) {
  // CHECK: @test_addc
  // CHECK: %{{.+}} = {{.*}} call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %x, i32 %y)
  // CHECK: %{{.+}} = extractvalue { i32, i1 } %{{.+}}, 1
  // CHECK: %{{.+}} = extractvalue { i32, i1 } %{{.+}}, 0
  // CHECK: %{{.+}} = {{.*}} call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %{{.+}}, i32 %carryin)
  // CHECK: %{{.+}} = extractvalue { i32, i1 } %{{.+}}, 1
  // CHECK: %{{.+}} = extractvalue { i32, i1 } %{{.+}}, 0
  // CHECK: %{{.+}} = or i1 %{{.+}}, %{{.+}}
  // CHECK: %{{.+}} = zext i1 %{{.+}} to i32
  // CHECK: store i32 %{{.+}}, i32* %z, align 4
  unsigned carryout;
  *z = __builtin_addc(x, y, carryin, &carryout);

  return carryout;
}

unsigned long test_addcl(unsigned long x, unsigned long y,
                         unsigned long carryin, unsigned long *z) {
  // long is i32 on i686, i64 on x86_64.
  // CHECK: @test_addcl([[UL:i32|i64]] %x
  // CHECK: %{{.+}} = {{.*}} call { [[UL]], i1 } @llvm.uadd.with.overflow.[[UL]]([[UL]] %x, [[UL]] %y)
  // CHECK: %{{.+}} = extractvalue { [[UL]], i1 } %{{.+}}, 1
  // CHECK: %{{.+}} = extractvalue { [[UL]], i1 } %{{.+}}, 0
  // CHECK: %{{.+}} = {{.*}} call { [[UL]], i1 } @llvm.uadd.with.overflow.[[UL]]([[UL]] %{{.+}}, [[UL]] %carryin)
  // CHECK: %{{.+}} = extractvalue { [[UL]], i1 } %{{.+}}, 1
  // CHECK: %{{.+}} = extractvalue { [[UL]], i1 } %{{.+}}, 0
  // CHECK: %{{.+}} = or i1 %{{.+}}, %{{.+}}
  // CHECK: %{{.+}} = zext i1 %{{.+}} to [[UL]]
  // CHECK: store [[UL]] %{{.+}}, [[UL]]* %z
  unsigned long carryout;
  *z = __builtin_addcl(x, y, carryin, &carryout);

  return carryout;
}

unsigned long long test_addcll(unsigned long long x, unsigned long long y,
                               unsigned long long carryin,
                               unsigned long long *z) {
  // CHECK: @test_addcll
  // CHECK: %{{.+}} = {{.*}} call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %x, i64 %y)
  // CHECK: %{{.+}} = extractvalue { i64, i1 } %{{.+}}, 1
  // CHECK: %{{.+}} = extractvalue { i64, i1 } %{{.+}}, 0
  // CHECK: %{{.+}} = {{.*}} call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %{{.+}}, i64 %carryin)
  // CHECK: %{{.+}} = extractvalue { i64, i1 } %{{.+}}, 1
  // CHECK: %{{.+}} = extractvalue { i64, i1 } %{{.+}}, 0
  // CHECK: %{{.+}} = or i1 %{{.+}}, %{{.+}}
  // CHECK: %{{.+}} = zext i1 %{{.+}} to i64
  // CHECK: store i64 %{{.+}}, i64* %z
  unsigned long long carryout;
  *z = __builtin_addcll(x, y, carryin, &carryout);

  return carryout;
}
