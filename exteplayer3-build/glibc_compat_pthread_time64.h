/* glibc_compat_pthread_time64.h — gemeinsamer pthread/dl/time64-Kompat-Block
 * fuer ARM (exteplayer3) und MIPS (serviceapp + exteplayer3).
 *
 * Vor dem #include dieser Datei muessen zwei Makros definiert sein:
 *   GLIBC_COMPAT_VER_MAIN   - Symbolversion fuer pthread_create/cancel/join,
 *                             dlopen/dlclose/dlerror/dlsym, __libc_start_main
 *   GLIBC_COMPAT_VER_RWLOCK - Symbolversion fuer pthread_rwlock_*
 * (auf ARM sind beide "2.4", auf MIPS "2.0" bzw. "2.2" - siehe die jeweilige
 * glibc_compat.c, die das vor dem Include festlegt).
 *
 * __lstat64_time64 ist bewusst NICHT hier drin: die ARM-Variante inkludiert
 * dafuer <sys/stat.h>, die MIPS-Variante vermeidet das explizit (verhindert
 * dort Header-seitige __asm__-Umbenennungen von stat/stat64) - das bleibt
 * daher pro Datei separat.
 */
#ifndef GLIBC_COMPAT_VER_MAIN
#error "GLIBC_COMPAT_VER_MAIN muss vor dem Include definiert werden"
#endif
#ifndef GLIBC_COMPAT_VER_RWLOCK
#error "GLIBC_COMPAT_VER_RWLOCK muss vor dem Include definiert werden"
#endif

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/prctl.h>

#define GLIBC_SYMVER_MAIN(sym)   __asm__(".symver " #sym "," #sym "@GLIBC_" GLIBC_COMPAT_VER_MAIN)
#define GLIBC_SYMVER_RWLOCK(sym) __asm__(".symver " #sym "," #sym "@GLIBC_" GLIBC_COMPAT_VER_RWLOCK)

GLIBC_SYMVER_MAIN(pthread_create);
GLIBC_SYMVER_MAIN(pthread_cancel);
GLIBC_SYMVER_MAIN(pthread_join);
GLIBC_SYMVER_RWLOCK(pthread_rwlock_init);
GLIBC_SYMVER_RWLOCK(pthread_rwlock_rdlock);
GLIBC_SYMVER_RWLOCK(pthread_rwlock_unlock);
GLIBC_SYMVER_RWLOCK(pthread_rwlock_wrlock);
GLIBC_SYMVER_MAIN(dlopen);
GLIBC_SYMVER_MAIN(dlclose);
GLIBC_SYMVER_MAIN(dlerror);
GLIBC_SYMVER_MAIN(dlsym);
GLIBC_SYMVER_MAIN(__libc_start_main);

/* time64 Wrapper: direkte Syscalls statt glibc-Wrapper (kein Rekursionsrisiko) */

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

int __select64(int nfds, fd_set *readfds, fd_set *writefds,
               fd_set *exceptfds, struct timeval *timeout) {
#ifdef SYS__newselect
    return (int)syscall(SYS__newselect, nfds, readfds, writefds, exceptfds, timeout);
#else
    return (int)syscall(SYS_select, nfds, readfds, writefds, exceptfds, timeout);
#endif
}

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
