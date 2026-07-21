/* glibc_compat.c — Versionskompatibilität GCC14/glibc2.34+ → glibc2.18 (Box)
 *
 * Problem: GCC14 auf Debian Trixie erzeugt Referenzen zu glibc 2.34-Symbolen:
 * - pthread_create@GLIBC_2.34 (pthread in libc integriert)
 * - __fcntl_time64@GLIBC_2.34 (year-2038-Fix für 32-bit ARM)
 * - etc.
 *
 * Lösung: time64-Symbole als lokale Funktionen implementieren die direkte
 * Linux-Syscalls nutzen (nicht glibc-Wrapper, um Rekursion zu vermeiden).
 */
/* pthread/dlopen/time64: gemeinsamer Block mit MIPS ausgelagert, siehe Header */
#define GLIBC_COMPAT_VER_MAIN   "2.4"
#define GLIBC_COMPAT_VER_RWLOCK "2.4"
#include "glibc_compat_pthread_time64.h"

/* lstat64 direkt - eigenes #include <sys/stat.h>, siehe Header-Kommentar */
#include <sys/stat.h>
int __lstat64_time64(const char *pathname, struct stat *statbuf) {
    return (int)syscall(SYS_lstat64, pathname, statbuf);
}
