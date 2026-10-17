/* Upstream connections: name lookup, TCP connect, TLS 1.2 via mbedTLS.
 *
 * Windows 2000 notes: ws2_32 has no getaddrinfo, so name lookup is
 * gethostbyname (IPv4 only; Winsock keeps its result per thread). Entropy
 * comes from CryptGenRandom, since BCryptGenRandom is Vista+.
 *
 * Threads: each connection has its own mbedtls_ssl_context. The config and
 * CA chain are shared read-only. The CTR-DRBG is the one shared mutable
 * object, so every draw from it goes through rng_lock. */
#include "tlsnet.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wincrypt.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

extern const char tp_ca_pem[];
extern const unsigned tp_ca_pem_len;

/* ---------------------------------------------------------------- time */

/* msvcrt.dll on Win2k has no gmtime_s/_gmtime64_s; mbedTLS needs gmtime_r
 * to check certificate validity dates. Days-to-civil from H. Hinnant. */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm)
{
    long long t = (long long)*tt;
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    memset(tm, 0, sizeof *tm);
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days + 4) % 7 + 7) % 7;
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = (int)m - 1;
    tm->tm_mday = (int)d;
    return tm;
}

/* ---------------------------------------------------------------- globals */

static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt         cacert;
static mbedtls_ssl_config       sslconf;
static CRITICAL_SECTION         rng_lock;

static int cryptgen_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
    HCRYPTPROV prov;
    (void)data;
    *olen = 0;
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    BOOL ok = CryptGenRandom(prov, (DWORD)len, out);
    CryptReleaseContext(prov, 0);
    if (!ok)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = len;
    return 0;
}

static int locked_rng(void *ctx, unsigned char *out, size_t len)
{
    EnterCriticalSection(&rng_lock);
    int rc = mbedtls_ctr_drbg_random(ctx, out, len);
    LeaveCriticalSection(&rng_lock);
    return rc;
}

static void tls_errstr(int rc, char *err, size_t errlen, const char *what)
{
    char m[160];
    mbedtls_strerror(rc, m, sizeof m);
    snprintf(err, errlen, "%s: %s (-0x%04X)", what, m, (unsigned)-rc);
}

static int load_pem_file(const char *path, char *err, size_t errlen)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, errlen, "cannot open CA file %s", path);
        return -1;
    }
    DWORD size = GetFileSize(h, NULL), got = 0;
    unsigned char *pem = xmalloc((size_t)size + 1);
    ReadFile(h, pem, size, &got, NULL);
    CloseHandle(h);
    pem[got] = 0;
    int rc = mbedtls_x509_crt_parse(&cacert, pem, (size_t)got + 1);
    free(pem);
    if (rc < 0) {
        tls_errstr(rc, err, errlen, path);
        return -1;
    }
    return 0;
}

int tls_init(const char *extra_ca_file, char *err, size_t errlen)
{
    int rc;
    InitializeCriticalSection(&rng_lock);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ssl_config_init(&sslconf);

    mbedtls_entropy_add_source(&entropy, cryptgen_poll, NULL, 32,
                               MBEDTLS_ENTROPY_SOURCE_STRONG);
    static const char pers[] = "wmpmeta";
    rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)pers, sizeof pers - 1);
    if (rc) {
        tls_errstr(rc, err, errlen, "random seed (CryptGenRandom)");
        return -1;
    }
    /* A positive return counts certificates that did not parse (for example
     * roots on curves this build leaves out). The rest still load. */
    rc = mbedtls_x509_crt_parse(&cacert, (const unsigned char *)tp_ca_pem, tp_ca_pem_len);
    if (rc < 0) {
        tls_errstr(rc, err, errlen, "built-in CA bundle");
        return -1;
    }
    if (extra_ca_file && *extra_ca_file && load_pem_file(extra_ca_file, err, errlen))
        return -1;
    rc = mbedtls_ssl_config_defaults(&sslconf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) {
        tls_errstr(rc, err, errlen, "TLS config");
        return -1;
    }
    mbedtls_ssl_conf_authmode(&sslconf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&sslconf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&sslconf, locked_rng, &drbg);
    return 0;
}

/* ---------------------------------------------------------------- connections */

struct upstream {
    SOCKET              s;
    int                 tls;
    DWORD               timeout;
    mbedtls_ssl_context ssl;
    char                why[256];
};

static void set_why(upstream_t *u, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->why, sizeof u->why, fmt, ap);
    va_end(ap);
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    upstream_t *u = ctx;
    if (sock_wait(u->s, 1, u->timeout) != 1) {
        set_why(u, "send timed out");
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    int n = send(u->s, (const char *)buf, (int)(len > 65536 ? 65536 : len), 0);
    if (n == SOCKET_ERROR) {
        set_why(u, "send failed (WSA %d)", WSAGetLastError());
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    upstream_t *u = ctx;
    int w = sock_wait(u->s, 0, u->timeout);
    if (w != 1) {
        set_why(u, w ? "connection error" : "timed out");
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    int n = recv(u->s, (char *)buf, (int)(len > 65536 ? 65536 : len), 0);
    if (n == SOCKET_ERROR) {
        set_why(u, "receive failed (WSA %d)", WSAGetLastError());
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;  /* 0 is EOF; mbedTLS turns it into MBEDTLS_ERR_SSL_CONN_EOF */
}

static int tcp_connect(upstream_t *u, const char *host, int port, int *code)
{
    unsigned long addrs[16];
    int naddr = 0;
    unsigned long a = inet_addr(host);
    if (a != INADDR_NONE) {
        addrs[naddr++] = a;
    } else {
        struct hostent *he = gethostbyname(host);
        if (!he || he->h_addrtype != AF_INET) {
            set_why(u, "cannot resolve %s (WSA %d)", host, WSAGetLastError());
            *code = UP_EDNS;
            return -1;
        }
        for (int i = 0; he->h_addr_list[i] && naddr < 16; i++)
            memcpy(&addrs[naddr++], he->h_addr_list[i], 4);
    }
    *code = UP_ECONNECT;
    for (int i = 0; i < naddr; i++) {
        u->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (u->s == INVALID_SOCKET) {
            set_why(u, "socket() failed (WSA %d)", WSAGetLastError());
            return -1;
        }
        u_long nb = 1;
        ioctlsocket(u->s, FIONBIO, &nb);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((u_short)port);
        sa.sin_addr.s_addr = addrs[i];
        int e = 0;
        if (connect(u->s, (struct sockaddr *)&sa, sizeof sa) == 0 ||
            (e = WSAGetLastError()) == WSAEWOULDBLOCK) {
            int w = sock_wait(u->s, 1, 20000), el = sizeof e;
            e = 0;
            getsockopt(u->s, SOL_SOCKET, SO_ERROR, (char *)&e, &el);
            if (w == 1 && !e) {
                nb = 0;
                ioctlsocket(u->s, FIONBIO, &nb);
                BOOL one = TRUE;
                setsockopt(u->s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
                return 0;
            }
            if (w == 0)
                set_why(u, "connect to %s:%d timed out", host, port);
            else
                set_why(u, "connect to %s:%d failed (WSA %d)", host, port, e);
        } else {
            set_why(u, "connect to %s:%d failed (WSA %d)", host, port, e);
        }
        closesocket(u->s);
        u->s = INVALID_SOCKET;
    }
    return -1;
}

static void verify_why(upstream_t *u, const char *host)
{
    uint32_t flags = mbedtls_ssl_get_verify_result(&u->ssl);
    char info[512];
    int n = mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
    /* One line: join the per-flag lines with "; ". */
    for (int i = 0; i < n; i++)
        if (info[i] == '\n')
            info[i] = i == n - 1 ? 0 : ';';
    const char *hint = "";
    if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
        hint = " (is the system clock right?)";
    set_why(u, "certificate for %s rejected:%s%s", host, info, hint);
}

upstream_t *up_connect(const char *host, int port, int tls, DWORD timeout_ms,
                       int *code, char *err, size_t errlen)
{
    upstream_t *u = xmalloc(sizeof *u);
    memset(u, 0, sizeof *u);
    u->s = INVALID_SOCKET;
    u->timeout = timeout_ms;
    if (tcp_connect(u, host, port, code))
        goto fail;
    if (tls) {
        *code = UP_ETLS;
        mbedtls_ssl_init(&u->ssl);
        u->tls = 1;
        int rc = mbedtls_ssl_setup(&u->ssl, &sslconf);
        if (!rc)
            rc = mbedtls_ssl_set_hostname(&u->ssl, host);
        if (rc) {
            tls_errstr(rc, u->why, sizeof u->why, "TLS setup");
            goto fail;
        }
        mbedtls_ssl_set_bio(&u->ssl, u, bio_send, bio_recv, NULL);
        while ((rc = mbedtls_ssl_handshake(&u->ssl)) != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                verify_why(u, host);
            } else if (!u->why[0] || rc != MBEDTLS_ERR_SSL_INTERNAL_ERROR) {
                char m[160];
                snprintf(m, sizeof m, "TLS handshake with %s:%d", host, port);
                tls_errstr(rc, u->why, sizeof u->why, m);
            }
            goto fail;
        }
    }
    *code = UP_OK;
    return u;
fail:
    snprintf(err, errlen, "%s", u->why[0] ? u->why : "connection failed");
    up_close(u);
    return NULL;
}

int up_read(upstream_t *u, char *buf, size_t len)
{
    if (!u->tls) {
        int w = sock_wait(u->s, 0, u->timeout);
        if (w != 1) {
            set_why(u, w ? "connection error" : "timed out");
            return -1;
        }
        int n = recv(u->s, buf, (int)(len > 65536 ? 65536 : len), 0);
        if (n == SOCKET_ERROR)
            set_why(u, "receive failed (WSA %d)", WSAGetLastError());
        return n == SOCKET_ERROR ? -1 : n;
    }
    for (;;) {
        int n = mbedtls_ssl_read(&u->ssl, (unsigned char *)buf, len);
        if (n >= 0)
            return n;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        /* Plenty of servers close without close_notify; treat as EOF. */
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF)
            return 0;
        if (!u->why[0] || n != MBEDTLS_ERR_SSL_INTERNAL_ERROR)
            tls_errstr(n, u->why, sizeof u->why, "TLS read");
        return -1;
    }
}

int up_write_all(upstream_t *u, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        int n;
        if (u->tls) {
            n = mbedtls_ssl_write(&u->ssl, (const unsigned char *)p, len);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (n < 0 && (!u->why[0] || n != MBEDTLS_ERR_SSL_INTERNAL_ERROR))
                tls_errstr(n, u->why, sizeof u->why, "TLS write");
        } else {
            n = sock_send_all(u->s, p, len) ? -1 : (int)len;
            if (n < 0)
                set_why(u, "send failed");
        }
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

size_t up_pending(upstream_t *u)
{
    return u->tls ? mbedtls_ssl_get_bytes_avail(&u->ssl) : 0;
}

SOCKET up_socket(upstream_t *u)
{
    return u->s;
}

const char *up_error(upstream_t *u)
{
    return u->why[0] ? u->why : "connection closed";
}

const char *up_cipher(upstream_t *u)
{
    return u->tls ? mbedtls_ssl_get_ciphersuite(&u->ssl) : "plain";
}

void up_close(upstream_t *u)
{
    if (!u)
        return;
    if (u->tls) {
        if (u->s != INVALID_SOCKET) {
            u->timeout = 2000;
            mbedtls_ssl_close_notify(&u->ssl);
        }
        mbedtls_ssl_free(&u->ssl);
    }
    if (u->s != INVALID_SOCKET)
        closesocket(u->s);
    free(u);
}
