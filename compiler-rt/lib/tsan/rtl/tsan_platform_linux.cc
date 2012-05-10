//===-- tsan_platform_linux.cc ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
// Linux-specific code.
//===----------------------------------------------------------------------===//

#include "tsan_platform.h"
#include "tsan_rtl.h"
#include "tsan_flags.h"

#include <asm/prctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <dlfcn.h>

extern "C" int arch_prctl(int code, __tsan::uptr *addr);

namespace __tsan {

static uptr g_tls_size;

ScopedInRtl::ScopedInRtl()
    : thr_(cur_thread()) {
  in_rtl_ = thr_->in_rtl;
  thr_->in_rtl++;
  errno_ = errno;
}

ScopedInRtl::~ScopedInRtl() {
  thr_->in_rtl--;
  errno = errno_;
  CHECK_EQ(in_rtl_, thr_->in_rtl);
}

void Die() {
  _exit(1);
}

static void *my_mmap(void *addr, size_t length, int prot, int flags,
                    int fd, u64 offset) {
  ScopedInRtl in_rtl;
# if __WORDSIZE == 64
  return (void *)syscall(__NR_mmap, addr, length, prot, flags, fd, offset);
# else
  return (void *)syscall(__NR_mmap2, addr, length, prot, flags, fd, offset);
# endif
}

void sched_yield() {
  ScopedInRtl in_rtl;
  syscall(__NR_sched_yield);
}

fd_t internal_open(const char *name, bool write) {
  ScopedInRtl in_rtl;
  return syscall(__NR_open, name,
      write ? O_WRONLY | O_CREAT | O_CLOEXEC : O_RDONLY, 0660);
}

void internal_close(fd_t fd) {
  ScopedInRtl in_rtl;
  syscall(__NR_close, fd);
}

uptr internal_filesize(fd_t fd) {
  struct stat st = {};
  if (syscall(__NR_fstat, fd, &st))
    return -1;
  return (uptr)st.st_size;
}

uptr internal_read(fd_t fd, void *p, uptr size) {
  ScopedInRtl in_rtl;
  return syscall(__NR_read, fd, p, size);
}

uptr internal_write(fd_t fd, const void *p, uptr size) {
  ScopedInRtl in_rtl;
  return syscall(__NR_write, fd, p, size);
}

const char *internal_getpwd() {
  return getenv("PWD");
}

static void ProtectRange(uptr beg, uptr end) {
  ScopedInRtl in_rtl;
  CHECK_LE(beg, end);
  if (beg == end)
    return;
  if (beg != (uptr)my_mmap((void*)(beg), end - beg,
      PROT_NONE,
      MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE,
      -1, 0)) {
    Printf("FATAL: ThreadSanitizer can not protect [%lx,%lx]\n", beg, end);
    Printf("FATAL: Make sure you are not using unlimited stack\n");
    Die();
  }
}

void InitializeShadowMemory() {
  const uptr kClosedLowBeg  = 0x200000;
  const uptr kClosedLowEnd  = kLinuxShadowBeg - 1;
  const uptr kClosedMidBeg = kLinuxShadowEnd + 1;
  const uptr kClosedMidEnd = kLinuxAppMemBeg - 1;
  uptr shadow = (uptr)my_mmap((void*)kLinuxShadowBeg,
      kLinuxShadowEnd - kLinuxShadowBeg,
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE,
      0, 0);
  if (shadow != kLinuxShadowBeg) {
    Printf("FATAL: ThreadSanitizer can not mmap the shadow memory\n");
    Printf("FATAL: Make sure to compile with -fPIE and to link with -pie.\n");
    Die();
  }
  ProtectRange(kClosedLowBeg, kClosedLowEnd);
  ProtectRange(kClosedMidBeg, kClosedMidEnd);
  DPrintf("kClosedLow   %lx-%lx (%luGB)\n",
      kClosedLowBeg, kClosedLowEnd, (kClosedLowEnd - kClosedLowBeg) >> 30);
  DPrintf("kLinuxShadow %lx-%lx (%luGB)\n",
      kLinuxShadowBeg, kLinuxShadowEnd,
      (kLinuxShadowEnd - kLinuxShadowBeg) >> 30);
  DPrintf("kClosedMid   %lx-%lx (%luGB)\n",
      kClosedMidBeg, kClosedMidEnd, (kClosedMidEnd - kClosedMidBeg) >> 30);
  DPrintf("kLinuxAppMem %lx-%lx (%luGB)\n",
      kLinuxAppMemBeg, kLinuxAppMemEnd,
      (kLinuxAppMemEnd - kLinuxAppMemBeg) >> 30);
  DPrintf("stack        %lx\n", (uptr)&shadow);
}

static void CheckPIE() {
  // Ensure that the binary is indeed compiled with -pie.
  fd_t fmaps = internal_open("/proc/self/maps", false);
  if (fmaps == kInvalidFd)
    return;
  char buf[20];
  if (internal_read(fmaps, buf, sizeof(buf)) == sizeof(buf)) {
    buf[sizeof(buf) - 1] = 0;
    u64 addr = strtoll(buf, 0, 16);
    if ((u64)addr < kLinuxAppMemBeg) {
      Printf("FATAL: ThreadSanitizer can not mmap the shadow memory ("
             "something is mapped at 0x%llx < 0x%lx)\n",
             addr, kLinuxAppMemBeg);
      Printf("FATAL: Make sure to compile with -fPIE"
             " and to link with -pie.\n");
      Die();
    }
  }
  internal_close(fmaps);
}

#ifdef __i386__
# define INTERNAL_FUNCTION __attribute__((regparm(3), stdcall))
#else
# define INTERNAL_FUNCTION
#endif
extern "C" void _dl_get_tls_static_info(size_t*, size_t*)
    __attribute__((weak)) INTERNAL_FUNCTION;

static int InitTlsSize() {
  typedef void (*get_tls_func)(size_t*, size_t*) INTERNAL_FUNCTION;
  get_tls_func get_tls = &_dl_get_tls_static_info;
  if (get_tls == 0)
    get_tls = (get_tls_func)dlsym(RTLD_NEXT, "_dl_get_tls_static_info");
  CHECK_NE(get_tls, 0);
  size_t tls_size = 0;
  size_t tls_align = 0;
  get_tls(&tls_size, &tls_align);
  return tls_size;
}

const char *InitializePlatform() {
  void *p = 0;
  if (sizeof(p) == 8) {
    // Disable core dumps, dumping of 16TB usually takes a bit long.
    // The following magic is to prevent clang from replacing it with memset.
    volatile rlimit lim;
    lim.rlim_cur = 0;
    lim.rlim_max = 0;
    setrlimit(RLIMIT_CORE, (rlimit*)&lim);
  }

  CheckPIE();
  g_tls_size = (uptr)InitTlsSize();
  return getenv("TSAN_OPTIONS");
}

void FinalizePlatform() {
  fflush(0);
}

uptr GetTlsSize() {
  return g_tls_size;
}

void GetThreadStackAndTls(uptr *stk_addr, uptr *stk_size,
                          uptr *tls_addr, uptr *tls_size) {
  *stk_addr = 0;
  *stk_size = 0;
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    pthread_attr_getstack(&attr, (void**)stk_addr, (size_t*)stk_size);
    pthread_attr_destroy(&attr);
  }
  arch_prctl(ARCH_GET_FS, tls_addr);
  *tls_addr -= g_tls_size;
  *tls_size = g_tls_size;

  // If stack and tls intersect, make them non-intersecting.
  if (*tls_addr > *stk_addr && *tls_addr < *stk_addr + *stk_size) {
    CHECK_GT(*tls_addr + *tls_size, *stk_addr);
    CHECK_LE(*tls_addr + *tls_size, *stk_addr + *stk_size);
    *stk_size = *tls_addr - *stk_addr;
    *stk_size = RoundUp(*stk_size, kPageSize);
    uptr stk_end = *stk_addr + *stk_size;
    if (stk_end > *tls_addr) {
      *tls_size -= *tls_addr - stk_end;
      *tls_addr = stk_end;
    }
  }
}

int GetPid() {
  return getpid();
}

}  // namespace __tsan
