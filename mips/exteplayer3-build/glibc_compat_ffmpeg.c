/* glibc 2.21 compatibility stubs für ffmpeg (MIPS-spezifisch). */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

extern int __compat_fcntl(int fd, int cmd, ...);
__asm__(".symver __compat_fcntl,fcntl@GLIBC_2.0");

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

/* fstat/fstat64/stat/stat64: kein Versions-, sondern ein Symbol-Problem, wie
   im ARM-Pendant. Auf MIPS liegen __xstat/__fxstat unter GLIBC_2.0, aber
   __xstat64/__fxstat64 unter GLIBC_2.2 (per nm -D gegen die echte libc.so.6
   der Solo2-Box bestätigt, kein einheitliches Schema wie bei ARM). */

struct _compat_stat {
    char _opaque[512];
};

#define STAT_VER_LINUX 3

extern int __compat_xstat(int ver, const char *path, struct _compat_stat *buf);
extern int __compat_xstat64(int ver, const char *path, struct _compat_stat *buf);
extern int __compat_fxstat(int ver, int fd, struct _compat_stat *buf);
extern int __compat_fxstat64(int ver, int fd, struct _compat_stat *buf);
__asm__(".symver __compat_xstat,__xstat@GLIBC_2.0");
__asm__(".symver __compat_fxstat,__fxstat@GLIBC_2.0");
__asm__(".symver __compat_xstat64,__xstat64@GLIBC_2.2");
__asm__(".symver __compat_fxstat64,__fxstat64@GLIBC_2.2");

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

/*
 * Ersetzt libgccs __bswapsi2: Debians mipsel-linux-gnu-libgcc.a ist für die
 * mips32r2-Baseline vorkompiliert und nutzt dort wsbh/rotr (MIPS32r2-only,
 * SIGILL auf MIPS32r1-CPUs wie BCM7356). Da dieses Objekt (kein Archiv-Member)
 * vor -lgcc gelinkt wird, definiert es __bswapsi2 zuerst, sodass der Linker
 * das Archiv-Member nie zieht.
 */
unsigned int __bswapsi2(unsigned int x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8)  |
           ((x & 0x00ff0000U) >> 8)  |
           ((x & 0xff000000U) >> 24);
}

/*
 * Ersetzt libgccs __floatdisf/__floatundisf (int64_t/uint64_t -> float):
 * gleicher Grund wie __bswapsi2 oben, Debians libgcc.a nutzt hier `ins`
 * (MIPS32r2-only). Der Umweg ueber double ruft stattdessen __floatdidf/
 * __floatundidf auf (diese sind in libgcc r1-sauber) und nutzt fuer die
 * Verengung double->float eine native FPU-Instruktion (cvt.s.d).
 */
float __floatdisf(long long x)
{
    return (float)(double)x;
}

float __floatundisf(unsigned long long x)
{
    return (float)(double)x;
}

/*
 * Ersetzt libgccs __fixdfdi/__fixsfdi/__fixunsdfdi/__fixunssfdi
 * (double/float -> int64_t/uint64_t, die Umkehrrichtung zu __floatdisf
 * oben): Debians libgcc.a nutzt hier mthc1/mfhc1 (MIPS32r2-only FPU-
 * Instruktionen fuer den oberen 32-Bit-Teil eines Doubles im FPXX-Modus).
 * Dieser Crash trat erst bei echter Wiedergabe auf (PTS/Timestamp-Skalierung
 * mit double->int64), nicht schon beim Format-Probing wie __bswapsi2.
 *
 * Eigene Implementierung ueber manuelle Bitmanipulation der IEEE-754-
 * Repraesentation, ohne (long long)double-Cast (der wiederum einen Aufruf
 * von __fixdfdi erzeugen wuerde -> Endlosrekursion).
 */
int64_t __fixdfdi(double x)
{
    uint64_t bits;
    memcpy(&bits, &x, 8);
    int sign = (int)(bits >> 63);
    int exp = (int)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0xFFFFFFFFFFFFFULL;

    if (exp == 0 && mant == 0)
        return 0;
    if (exp == 0x7FF) {
        if (mant != 0)
            return 0;
        return sign ? INT64_MIN : INT64_MAX;
    }

    mant |= (1ULL << 52);
    int shift = exp - 1075; /* 1023 (bias) + 52 (mantissa bits) */
    uint64_t umag;
    if (shift >= 0) {
        /* Ab shift==11 kann mant<<shift bereits 2^63 erreichen/ueberschreiten -
         * das kippt den (int64_t)-Cast unten in einen falschen negativen Wert
         * statt korrekt zu saettigen. Deshalb hier schon abbrechen, nicht erst
         * bei shift>=12 (die unsigned-Variante darf das, da sie den vollen
         * 64-Bit-Bereich nutzt statt nur die Haelfte fuer signed). */
        if (shift >= 11)
            return sign ? INT64_MIN : INT64_MAX;
        umag = mant << shift;
    } else {
        int rshift = -shift;
        umag = (rshift >= 64) ? 0 : (mant >> rshift);
    }
    return sign ? -(int64_t)umag : (int64_t)umag;
}

uint64_t __fixunsdfdi(double x)
{
    if (x <= 0.0)
        return 0;
    uint64_t bits;
    memcpy(&bits, &x, 8);
    int exp = (int)((bits >> 52) & 0x7FF);
    uint64_t mant = bits & 0xFFFFFFFFFFFFFULL;

    if (exp == 0x7FF)
        return mant ? 0 : UINT64_MAX;

    mant |= (1ULL << 52);
    int shift = exp - 1075;
    if (shift >= 0) {
        if (shift >= 12)
            return UINT64_MAX;
        return mant << shift;
    } else {
        int rshift = -shift;
        return (rshift >= 64) ? 0 : (mant >> rshift);
    }
}

int64_t __fixsfdi(float x)
{
    uint32_t bits;
    memcpy(&bits, &x, 4);
    int sign = (int)(bits >> 31);
    int exp = (int)((bits >> 23) & 0xFF);
    uint32_t mant = bits & 0x7FFFFF;

    if (exp == 0 && mant == 0)
        return 0;
    if (exp == 0xFF) {
        if (mant != 0)
            return 0;
        return sign ? INT64_MIN : INT64_MAX;
    }

    uint64_t m = mant | (1U << 23);
    int shift = exp - 150; /* 127 (bias) + 23 (mantissa bits) */
    uint64_t umag;
    if (shift >= 0) {
        /* Analog zu __fixdfdi: ab shift==40 kann m<<shift bereits 2^63
         * erreichen/ueberschreiten, deshalb hier schon saettigen statt erst
         * bei shift>=41. */
        if (shift >= 40)
            return sign ? INT64_MIN : INT64_MAX;
        umag = m << shift;
    } else {
        int rshift = -shift;
        umag = (rshift >= 64) ? 0 : (m >> rshift);
    }
    return sign ? -(int64_t)umag : (int64_t)umag;
}

uint64_t __fixunssfdi(float x)
{
    if (x <= 0.0f)
        return 0;
    uint32_t bits;
    memcpy(&bits, &x, 4);
    int exp = (int)((bits >> 23) & 0xFF);
    uint32_t mant = bits & 0x7FFFFF;

    if (exp == 0xFF)
        return mant ? 0 : UINT64_MAX;

    uint64_t m = mant | (1U << 23);
    int shift = exp - 150;
    if (shift >= 0) {
        if (shift >= 41)
            return UINT64_MAX;
        return m << shift;
    } else {
        int rshift = -shift;
        return (rshift >= 64) ? 0 : (m >> rshift);
    }
}
