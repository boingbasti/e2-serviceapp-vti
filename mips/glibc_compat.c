/* glibc_compat.c — MIPS Version für VU+ Solo2 (VTi / glibc 2.21) */

/* I do NOT include <sys/stat.h> to prevent header-level __asm__ renames of stat/stat64 */
struct stat;
struct stat64;

/* pthread/dlopen/time64: gemeinsamer Block mit ARM (exteplayer3) ausgelagert,
 * siehe Header. MIPS braucht hier zwei unterschiedliche Symbolversionen
 * (GLIBC_2.0 fuer die meisten, GLIBC_2.2 speziell fuer pthread_rwlock_*). */
#define GLIBC_COMPAT_VER_MAIN   "2.0"
#define GLIBC_COMPAT_VER_RWLOCK "2.2"
#include "../exteplayer3-build/glibc_compat_pthread_time64.h"

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

/* lstat64 direkt - bewusst OHNE <sys/stat.h>, siehe Kommentar oben/im Header */
int __lstat64_time64(const char *pathname, struct stat *statbuf) {
    return (int)syscall(SYS_lstat64, pathname, statbuf);
}
