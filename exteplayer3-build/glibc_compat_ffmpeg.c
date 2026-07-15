/* glibc 2.21 compatibility stubs für ffmpeg (Shared Libraries + Binary).
   Wird als extra-obj in jede Library und das Binary eingebunden.
   Alle Symbole hidden damit keine Kollisionen zwischen Libraries entstehen. */

#include <fcntl.h>
#include <stdarg.h>

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
