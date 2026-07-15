/* glibc_compat.c — MIPS Version für VU+ Solo2 (VTi / glibc 2.21) */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* I do NOT include <sys/stat.h> to prevent header-level __asm__ renames of stat/stat64 */
struct stat;
struct stat64;

/* === pthread / dl / libc: GLIBC_2.34+ → GLIBC_2.0 / GLIBC_2.2 (MIPS-spezifisch) === */
__asm__(".symver pthread_create,pthread_create@GLIBC_2.0");
__asm__(".symver pthread_cancel,pthread_cancel@GLIBC_2.0");
__asm__(".symver pthread_join,pthread_join@GLIBC_2.0");
__asm__(".symver pthread_rwlock_init,pthread_rwlock_init@GLIBC_2.2");
__asm__(".symver pthread_rwlock_rdlock,pthread_rwlock_rdlock@GLIBC_2.2");
__asm__(".symver pthread_rwlock_unlock,pthread_rwlock_unlock@GLIBC_2.2");
__asm__(".symver pthread_rwlock_wrlock,pthread_rwlock_wrlock@GLIBC_2.2");
__asm__(".symver dlopen,dlopen@GLIBC_2.0");
__asm__(".symver dlclose,dlclose@GLIBC_2.0");
__asm__(".symver dlerror,dlerror@GLIBC_2.0");
__asm__(".symver dlsym,dlsym@GLIBC_2.0");
__asm__(".symver __libc_start_main,__libc_start_main@GLIBC_2.0");

/* === __libc_single_threaded (GLIBC_2.32): locally resolved stub to avoid GLIBC_2.32 dependency === */
__attribute__((visibility("hidden")))
char __libc_single_threaded = 0;

/* === stat / stat64 (GLIBC_2.33): wrap using older __xstat / __xstat64 symbols === */
extern int __xstat(int ver, const char *path, struct stat *buf);
extern int __xstat64(int ver, const char *path, struct stat64 *buf);

__attribute__((visibility("hidden")))
int stat(const char *path, struct stat *buf) {
    return __xstat(3, path, buf); // 3 is _STAT_VER for MIPS/ARM (GNU/Linux)
}

__attribute__((visibility("hidden")))
int stat64(const char *path, struct stat64 *buf) {
    return __xstat64(3, path, buf);
}

/* === time64/compat Wrapper === */
int __fcntl_time64(int fd, int cmd, ...) {
    va_list args;
    va_start(args, cmd);
    long arg = va_arg(args, long);
    va_end(args);
    return (int)syscall(SYS_fcntl, fd, cmd, arg);
}

int __ioctl_time64(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    long arg = va_arg(args, long);
    va_end(args);
    return (int)syscall(SYS_ioctl, fd, request, arg);
}

time_t __time64(time_t *tloc) {
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
    if (tloc) *tloc = (time_t)ts.tv_sec;
    return (time_t)ts.tv_sec;
}

int __lstat64_time64(const char *pathname, struct stat *statbuf) {
    return (int)syscall(SYS_lstat64, pathname, statbuf);
}

#include <sys/select.h>
int __select64(int nfds, fd_set *readfds, fd_set *writefds,
               fd_set *exceptfds, struct timeval *timeout) {
#ifdef SYS__newselect
    return (int)syscall(SYS__newselect, nfds, readfds, writefds, exceptfds, timeout);
#else
    return (int)syscall(SYS_select, nfds, readfds, writefds, exceptfds, timeout);
#endif
}

#include <sys/prctl.h>
long __prctl_time64(int option, ...) {
    va_list args;
    va_start(args, option);
    unsigned long a1 = va_arg(args, unsigned long);
    unsigned long a2 = va_arg(args, unsigned long);
    unsigned long a3 = va_arg(args, unsigned long);
    unsigned long a4 = va_arg(args, unsigned long);
    va_end(args);
    return syscall(SYS_prctl, option, a1, a2, a3, a4);
}

extern int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *,
                                   const struct timespec *)
    __asm__("pthread_cond_timedwait");

int __pthread_cond_timedwait64(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                const struct timespec *abstime) {
    return pthread_cond_timedwait(cond, mutex, abstime);
}
