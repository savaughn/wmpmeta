/* C runtime functions that MinGW-w64 imports from msvcrt.dll but that the
 * Windows 2000 msvcrt.dll (6.1) does not export. Defining them here makes
 * the linker use these instead of the import thunks. `make check` fails if
 * any other missing import creeps in.
 *
 *   vsnprintf_s   mbedTLS platform.c (mbedtls_platform_win32_vsnprintf)
 *   strtoll       MinGW's C99 scanf (used by cJSON), normally _strtoi64
 *   strtoull      same, normally _strtoui64
 *
 * No <stdio.h>/<stdlib.h> here: their dllimport declarations would clash. */
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>

int __mingw_vsnprintf(char *, size_t, const char *, va_list);

/* The only caller passes _TRUNCATE ((size_t)-1) as count. */
int vsnprintf_s(char *buf, size_t size, size_t count, const char *fmt, va_list ap)
{
    if (!buf || !size || !fmt)
        return -1;
    size_t limit = count < size ? count + 1 : size;
    int n = __mingw_vsnprintf(buf, limit, fmt, ap);
    if (n < 0 || (size_t)n >= limit) {
        buf[limit - 1] = 0;
        return -1;
    }
    return n;
}

static int digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

/* Parses magnitude and sign; *overflow set if it exceeds ULLONG_MAX. */
static unsigned long long parse(const char *s, char **end, int base, int *neg, int *overflow)
{
    const char *p = s;
    *neg = *overflow = 0;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    if (*p == '+' || *p == '-')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && digit(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = p;
    for (int d; (d = digit(*p)) < base; p++) {
        if (v > (ULLONG_MAX - (unsigned)d) / (unsigned)base)
            *overflow = 1;
        else
            v = v * (unsigned)base + (unsigned)d;
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    int neg, ovf;
    unsigned long long v = parse(s, end, base, &neg, &ovf);
    if (ovf) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return neg ? -v : v;
}

long long strtoll(const char *s, char **end, int base)
{
    int neg, ovf;
    unsigned long long v = parse(s, end, base, &neg, &ovf);
    if (ovf || v > (unsigned long long)LLONG_MAX + neg) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? (long long)(0 - v) : (long long)v;
}
