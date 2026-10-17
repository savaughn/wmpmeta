/* Sockets, buffers and TLS 1.2 client connections (mbedTLS) for Windows
 * 2000. Taken from tlsproxy, a TLS proxy for the same machine. */
#ifndef TLSNET_H
#define TLSNET_H

#include <winsock2.h>
#include <windows.h>
#include <stddef.h>

/* ---- util.c ---- */
typedef struct {
    char  *p;
    size_t n, cap;
} buf_t;

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void  buf_add(buf_t *b, const void *data, size_t n);
void  buf_puts(buf_t *b, const char *s);
void  buf_printf(buf_t *b, const char *fmt, ...);
char *buf_cstr(buf_t *b);               /* NUL-terminates, returns b->p */
void  buf_free(buf_t *b);

void  util_init(void);
/* Timestamped line on stdout; safe from any thread. */
void  log_msg(const char *fmt, ...);
extern int g_verbose;

/* Waits for a socket. 1 ready, 0 timeout, -1 error. */
int   sock_wait(SOCKET s, int for_write, DWORD timeout_ms);
int   sock_send_all(SOCKET s, const void *data, size_t n);
/* >0 bytes, 0 orderly close, -1 error or timeout. */
int   sock_recv(SOCKET s, char *buf, size_t len, DWORD timeout_ms);
void  sock_close(SOCKET s);

/* ---- tls.c ---- */
int tls_init(const char *extra_ca_file, char *err, size_t errlen);

enum { UP_OK = 0, UP_EDNS = -1, UP_ECONNECT = -2, UP_ETLS = -3 };

typedef struct upstream upstream_t;
/* Connects (and does the TLS handshake if tls). On failure returns NULL,
 * sets *code to one of UP_E* and fills err. */
upstream_t *up_connect(const char *host, int port, int tls, DWORD timeout_ms,
                       int *code, char *err, size_t errlen);
int         up_read(upstream_t *u, char *buf, size_t len);  /* >0, 0 EOF, -1 */
int         up_write_all(upstream_t *u, const void *data, size_t n);
size_t      up_pending(upstream_t *u);  /* decrypted bytes already buffered */
SOCKET      up_socket(upstream_t *u);
const char *up_error(upstream_t *u);
const char *up_cipher(upstream_t *u);
void        up_close(upstream_t *u);

#endif
