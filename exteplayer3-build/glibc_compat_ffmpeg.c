/* glibc 2.21 compatibility stubs für ffmpeg (Shared Libraries + Binary).
   Wird als extra-obj in jede Library und das Binary eingebunden.
   Alle Symbole hidden damit keine Kollisionen zwischen Libraries entstehen. */

#include <fcntl.h>
#include <stdarg.h>
#include <glob.h>
#include <pthread.h>

/* fcntl@GLIBC_2.4 als Basis */
extern int __compat_fcntl(int fd, int cmd, ...);
__asm__(".symver __compat_fcntl,fcntl@GLIBC_2.4");

/* fcntl64 (GLIBC_2.28): kommt durch _FILE_OFFSET_BITS=64 auf neuem glibc.
   Auf glibc 2.21 gibt es nur fcntl. */
__attribute__((visibility("hidden")))
int fcntl64(int fd, int cmd, ...)
{
    va_list ap;
    long arg;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    return __compat_fcntl(fd, cmd, arg);
}

/* fstat/fstat64/stat/stat64: kein Versions-, sondern ein Symbol-Problem.
   glibc 2.21 auf der Box exportiert diese vier Namen unter KEINER Version
   (nm -D gegen die echte Box-libc.so.6 bestätigt: nur die multiplexte
   __fxstat/__fxstat64/__xstat/__xstat64-Schnittstelle existiert, alle vier
   einheitlich unter GLIBC_2.4). patch_glibc_version.py kann das nicht
   reparieren, es schreibt nur Versions-Strings um, keine Symbolnamen.
   Moderne Build-Host-Header lassen FFmpegs eigenen Code (libavformat/file.c,
   libavutil/file.c) aber genau diese Namen aufrufen. Alle vier hier selbst
   bereitstellen und auf die alte multiplexte Schnittstelle umleiten. */

struct _compat_stat {
    char _opaque[512]; /* groß genug für jedes stat/stat64-Layout, kein
                           sys/stat.h-Include nötig (vermeidet glibc-2.38-
                           Header-Konflikte, wie in glibc_compat.c) */
};

#define STAT_VER_LINUX 3

extern int __compat_xstat(int ver, const char *path, struct _compat_stat *buf);
extern int __compat_xstat64(int ver, const char *path, struct _compat_stat *buf);
extern int __compat_fxstat(int ver, int fd, struct _compat_stat *buf);
extern int __compat_fxstat64(int ver, int fd, struct _compat_stat *buf);
__asm__(".symver __compat_xstat,__xstat@GLIBC_2.4");
__asm__(".symver __compat_xstat64,__xstat64@GLIBC_2.4");
__asm__(".symver __compat_fxstat,__fxstat@GLIBC_2.4");
__asm__(".symver __compat_fxstat64,__fxstat64@GLIBC_2.4");

__attribute__((visibility("hidden")))
int stat(const char *path, void *buf)
{
    return __compat_xstat(STAT_VER_LINUX, path, (struct _compat_stat *)buf);
}

__attribute__((visibility("hidden")))
int stat64(const char *path, void *buf)
{
    return __compat_xstat64(STAT_VER_LINUX, path, (struct _compat_stat *)buf);
}

__attribute__((visibility("hidden")))
int fstat(int fd, void *buf)
{
    return __compat_fxstat(STAT_VER_LINUX, fd, (struct _compat_stat *)buf);
}

__attribute__((visibility("hidden")))
int fstat64(int fd, void *buf)
{
    return __compat_fxstat64(STAT_VER_LINUX, fd, (struct _compat_stat *)buf);
}

/* fmod: reiner Versions-, kein Symbolnamen-Unterschied. Die Box-libm.so.6
   (glibc 2.21) exportiert fmod/fmodf/fmodl einheitlich unter GLIBC_2.4
   (per objdump -T verifiziert). Der aktuell installierte Cross-Toolchain-
   Stub (libc6-dev-armhf-cross, glibc 2.41) verlinkt beim Fehlen dieses
   Fixes stattdessen gegen die neueste verfuegbare Version (GLIBC_2.38),
   die auf der Box nicht existiert -> Ladefehler. */
extern double __compat_fmod(double x, double y);
__asm__(".symver __compat_fmod,fmod@GLIBC_2.4");

__attribute__((visibility("hidden")))
double fmod(double x, double y)
{
    return __compat_fmod(x, y);
}

/* hypot: dieselbe Situation wie fmod, ebenfalls GLIBC_2.4 auf der Box. */
extern double __compat_hypot(double x, double y);
__asm__(".symver __compat_hypot,hypot@GLIBC_2.4");

__attribute__((visibility("hidden")))
double hypot(double x, double y)
{
    return __compat_hypot(x, y);
}

/* glob64: reiner Versions-, kein Symbolnamen-Unterschied (Box exportiert
   glob64 direkt unter GLIBC_2.4, per objdump -T verifiziert). */
extern int __compat_glob64(const char *pattern, int flags,
                            int (*errfunc)(const char *epath, int eerrno),
                            void *pglob);
__asm__(".symver __compat_glob64,glob64@GLIBC_2.4");

__attribute__((visibility("hidden")))
int glob64(const char *pattern, int flags,
           int (*errfunc)(const char *epath, int eerrno), void *pglob)
{
    return __compat_glob64(pattern, flags, errfunc, pglob);
}

/* lstat64: wie stat/fstat oben ein Symbolnamen-, kein reines Versions-
   problem - die Box exportiert nur die multiplexte __lxstat64-Schnittstelle
   (GLIBC_2.4), kein direktes lstat64. */
extern int __compat_lxstat64(int ver, const char *path, struct _compat_stat *buf);
__asm__(".symver __compat_lxstat64,__lxstat64@GLIBC_2.4");

__attribute__((visibility("hidden")))
int lstat64(const char *path, void *buf)
{
    return __compat_lxstat64(STAT_VER_LINUX, path, (struct _compat_stat *)buf);
}

/* pthread_create/pthread_join/pthread_cancel: seit glibc 2.34 in libc.so
   selbst (vorher eigenes libpthread.so.0), auf der Box (glibc 2.21) nur
   unter GLIBC_2.4 vorhanden. Ein blosses ".symver name,name@version" ohne
   tatsaechlichen Aufruf in dieser Datei wird vom Linker beim Bauen einer
   Shared Library verworfen (mangels eigenem Symboltabelleneintrag) - hier
   daher wie bei stat/fstat oben ueber echte Wrapper-Funktionen geloest,
   nicht per direktem .symver wie in glibc_compat_pthread_time64.h (das
   nur fuer das exteplayer3-Binary eingebunden wird). */
extern int __compat_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                    void *(*start_routine)(void *), void *arg);
extern int __compat_pthread_join(pthread_t thread, void **retval);
extern int __compat_pthread_cancel(pthread_t thread);
__asm__(".symver __compat_pthread_create,pthread_create@GLIBC_2.4");
__asm__(".symver __compat_pthread_join,pthread_join@GLIBC_2.4");
__asm__(".symver __compat_pthread_cancel,pthread_cancel@GLIBC_2.4");

__attribute__((visibility("hidden")))
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*start_routine)(void *), void *arg)
{
    return __compat_pthread_create(thread, attr, start_routine, arg);
}

__attribute__((visibility("hidden")))
int pthread_join(pthread_t thread, void **retval)
{
    return __compat_pthread_join(thread, retval);
}

__attribute__((visibility("hidden")))
int pthread_cancel(pthread_t thread)
{
    return __compat_pthread_cancel(thread);
}
