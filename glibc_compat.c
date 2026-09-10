/* glibc 2.21 compatibility wrappers for VTi cross-compilation.
   Compiled WITHOUT standard sys/stat.h to avoid glibc 2.38 header magic. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Force helper calls to use GLIBC_2.4 versions */
__asm__(".symver vsscanf,vsscanf@GLIBC_2.4");
__asm__(".symver strtol,strtol@GLIBC_2.4");
__asm__(".symver strtoll,strtoll@GLIBC_2.4");
__asm__(".symver __xstat,__xstat@GLIBC_2.4");

/* __isoc23_sscanf (GLIBC_2.38): C23 sscanf — wrap via vsscanf@GLIBC_2.4 */
int __isoc23_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* __isoc23_strtol (GLIBC_2.38): C23 strtol — delegate to strtol@GLIBC_2.4 */
long __isoc23_strtol(const char *nptr, char **endptr, int base)
{
    return strtol(nptr, endptr, base);
}

/* __isoc23_strtoll (GLIBC_2.38): C23 strtoll — delegate to strtoll@GLIBC_2.4.
   Same redirect mechanism as __isoc23_strtol above, just for the 64-bit
   variant - needed as soon as any code in this .so calls strtoll() directly
   (first hit: parsing DASH "variant_bitrate" values in ffprobe_length.cpp). */
long long __isoc23_strtoll(const char *nptr, char **endptr, int base)
{
    return strtoll(nptr, endptr, base);
}

/* stat/stat64 (GLIBC_2.33): in glibc 2.21 these are inlines over __xstat/__xstat64.
   Declare __xstat manually to avoid sys/stat.h header conflicts.
   My stat64 definition also handles the stat()->stat64() redirect that newer glibc
   headers inject via __asm__("stat64"). */
struct _compat_stat {
    char _opaque[512]; /* large enough for any stat struct */
};

extern int __xstat(int ver, const char *path, struct _compat_stat *buf);
extern int __xstat64(int ver, const char *path, struct _compat_stat *buf);

#define STAT_VER_LINUX 3

int stat(const char *path, void *buf)
{
    return __xstat(STAT_VER_LINUX, path, (struct _compat_stat *)buf);
}

/* stat64: required because glibc 2.33+ headers redirect stat() -> stat64()
   on 32-bit platforms via __asm__("stat64"). */
int stat64(const char *path, void *buf)
{
    return __xstat64(STAT_VER_LINUX, path, (struct _compat_stat *)buf);
}

/* __libc_single_threaded (GLIBC_2.32): internal glibc optimization flag.
   Define locally — 0 means "assume multi-threaded" (safe, conservative). */
int __libc_single_threaded = 0;

/* OpenSSL 1.1+ API → 1.0 compat. Build host has OpenSSL 3.x headers which
   redirect SSLv23_client_method→TLS_client_method and
   SSL_library_init→OPENSSL_init_ssl. Box only has libssl.so.1.0.0. */
extern int SSL_library_init(void);
extern void SSL_load_error_strings(void);
extern void *SSLv23_client_method(void);

int OPENSSL_init_ssl(unsigned long long opts, const void *settings)
{
    (void)opts; (void)settings;
    SSL_library_init();
    SSL_load_error_strings();
    return 1;
}

void *TLS_client_method(void)
{
    return SSLv23_client_method();
}
