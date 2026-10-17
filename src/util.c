/* Memory, growable buffers, logging and socket helpers. */
#include "tlsnet.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_verbose;
static CRITICAL_SECTION log_lock;

static void oom(void)
{
    fputs("wmpmeta: out of memory\n", stderr);
    ExitProcess(3);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        oom();
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void buf_add(buf_t *b, const void *data, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1)
            cap *= 2;
        b->p = xrealloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

void buf_puts(buf_t *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof tmp) {
        buf_add(b, tmp, (size_t)n);
        return;
    }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_add(b, big, (size_t)n);
    free(big);
}

char *buf_cstr(buf_t *b)
{
    buf_add(b, "", 0);
    b->p[b->n] = 0;
    return b->p;
}

void buf_free(buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

void util_init(void)
{
    InitializeCriticalSection(&log_lock);
}

void log_msg(const char *fmt, ...)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1024];
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&log_lock);
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    LeaveCriticalSection(&log_lock);
}

int sock_wait(SOCKET s, int for_write, DWORD timeout_ms)
{
    fd_set fs, ex;
    FD_ZERO(&fs);
    FD_ZERO(&ex);
    FD_SET(s, &fs);
    FD_SET(s, &ex);
    struct timeval tv = {(long)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000};
    int r = select(0, for_write ? NULL : &fs, for_write ? &fs : NULL, &ex, &tv);
    if (r == SOCKET_ERROR)
        return -1;
    if (r == 0)
        return 0;
    if (FD_ISSET(s, &ex) && !FD_ISSET(s, &fs))
        return -1;
    return 1;
}

int sock_send_all(SOCKET s, const void *data, size_t n)
{
    const char *p = data;
    while (n) {
        if (sock_wait(s, 1, 120000) != 1)
            return -1;
        int k = send(s, p, (int)(n > 65536 ? 65536 : n), 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

int sock_recv(SOCKET s, char *buf, size_t len, DWORD timeout_ms)
{
    if (sock_wait(s, 0, timeout_ms) != 1)
        return -1;
    int n = recv(s, buf, (int)(len > 65536 ? 65536 : len), 0);
    return n == SOCKET_ERROR ? -1 : n;
}

void sock_close(SOCKET s)
{
    if (s == INVALID_SOCKET)
        return;
    shutdown(s, SD_SEND);
    /* Drain briefly so the peer gets a FIN instead of a reset when it still
     * has unread request bytes in flight. */
    char tmp[512];
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 1000 && sock_wait(s, 0, 200) == 1 &&
           recv(s, tmp, sizeof tmp, 0) > 0)
        ;
    closesocket(s);
}
