/* Small HTTP/1.0 client over tls.c. HTTP/1.0 keeps servers from answering
 * chunked, but chunked bodies are decoded anyway in case one does. */
#include "wmpmeta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MusicBrainz asks for a name, version and contact URL. */
#define UA "wmpmeta/" WM_VERSION " ( " WM_URL " )"

void fetch_free(fetch_t *r)
{
    free(r->ctype);
    buf_free(&r->body);
    memset(r, 0, sizeof *r);
}

static int parse_url(const char *url, int *tls, char *host, size_t hl, int *port,
                     const char **path)
{
    const char *p;
    if (!_strnicmp(url, "https://", 8)) {
        *tls = 1;
        *port = 443;
        p = url + 8;
    } else if (!_strnicmp(url, "http://", 7)) {
        *tls = 0;
        *port = 80;
        p = url + 7;
    } else {
        return -1;
    }
    size_t n = strcspn(p, ":/?");
    if (!n || n >= hl)
        return -1;
    memcpy(host, p, n);
    host[n] = 0;
    p += n;
    if (*p == ':') {
        *port = atoi(p + 1);
        p += 1 + strspn(p + 1, "0123456789");
    }
    *path = *p ? p : "/";
    return 0;
}

static const char *find_header(const char *hdrs, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = hdrs; (p = strstr(p, "\r\n")) != NULL;) {
        p += 2;
        if (!_strnicmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            return p;
        }
    }
    return NULL;
}

static char *header_value(const char *hdrs, const char *name)
{
    const char *v = find_header(hdrs, name);
    return v ? xstrndup(v, strcspn(v, "\r\n")) : NULL;
}

static void dechunk(buf_t *b)
{
    buf_t out = {0};
    const char *p = b->p, *end = b->p + b->n;
    while (p < end) {
        char *e;
        unsigned long n = strtoul(p, &e, 16);
        const char *nl = strstr(p, "\r\n");
        if (!nl || n == 0 || nl + 2 + n > end)
            break;
        buf_add(&out, nl + 2, n);
        p = nl + 2 + n + 2;
    }
    buf_free(b);
    *b = out;
}

static int fetch_once(const char *url, fetch_t *r, char **location, char *err, size_t errlen)
{
    int tls, port, code;
    char host[256];
    const char *path;
    if (parse_url(url, &tls, host, sizeof host, &port, &path)) {
        snprintf(err, errlen, "bad URL %s", url);
        return -1;
    }
    upstream_t *u = up_connect(host, port, tls, 30000, &code, err, errlen);
    if (!u)
        return -1;
    buf_t req = {0};
    buf_printf(&req,
               "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " UA "\r\n"
               "Accept: application/json, image/jpeg, */*\r\nConnection: close\r\n\r\n",
               path, host);
    int rc = up_write_all(u, req.p, req.n);
    buf_free(&req);
    if (rc) {
        snprintf(err, errlen, "send to %s: %s", host, up_error(u));
        up_close(u);
        return -1;
    }
    buf_t all = {0};
    char tmp[16384];
    int n;
    while ((n = up_read(u, tmp, sizeof tmp)) > 0)
        buf_add(&all, tmp, (size_t)n);
    up_close(u);
    buf_cstr(&all);
    char *eoh = all.p ? strstr(all.p, "\r\n\r\n") : NULL;
    if (!eoh || strncmp(all.p, "HTTP/", 5)) {
        snprintf(err, errlen, "no HTTP response from %s", host);
        buf_free(&all);
        return -1;
    }
    *eoh = 0;
    r->status = atoi(strchr(all.p, ' ') ? strchr(all.p, ' ') + 1 : "0");
    r->ctype = header_value(all.p, "Content-Type");
    *location = header_value(all.p, "Location");
    const char *te = find_header(all.p, "Transfer-Encoding");
    size_t hlen = (size_t)(eoh - all.p) + 4;
    buf_add(&r->body, all.p + hlen, all.n - hlen);
    if (te && !_strnicmp(te, "chunked", 7))
        dechunk(&r->body);
    buf_free(&all);
    return 0;
}

int fetch_url(const char *url, fetch_t *r, char *err, size_t errlen)
{
    char *cur = xstrdup(url);
    memset(r, 0, sizeof *r);
    for (int hop = 0; hop < 6; hop++) {
        char *loc = NULL;
        fetch_free(r);
        if (fetch_once(cur, r, &loc, err, errlen)) {
            free(cur);
            return -1;
        }
        if (r->status < 300 || r->status >= 400 || !loc) {
            free(loc);
            free(cur);
            return 0;
        }
        if (loc[0] == '/') {  /* relative: keep scheme and host */
            const char *s = strstr(cur, "://");
            size_t keep = s ? (size_t)(s + 3 - cur) + strcspn(s + 3, "/") : 0;
            buf_t b = {0};
            buf_add(&b, cur, keep);
            buf_puts(&b, loc);
            free(loc);
            loc = buf_cstr(&b);
        }
        free(cur);
        cur = loc;
    }
    free(cur);
    snprintf(err, errlen, "too many redirects");
    return -1;
}

/* One request, no redirects followed: the Location header (malloc'd, may be
 * NULL) and the status. Used to pass go.microsoft.com links through. */
char *fetch_location(const char *url, int *status, char *err, size_t errlen)
{
    fetch_t r = {0};
    char *loc = NULL;
    *status = 0;
    if (fetch_once(url, &r, &loc, err, errlen))
        return NULL;
    *status = r.status;
    fetch_free(&r);
    return loc;
}

/* Opens a URL for streaming: follows redirects, reads the response headers
 * and returns the connection positioned at the body. Body bytes that arrived
 * with the headers are left in *rest. Accepts SHOUTcast's "ICY 200 OK"
 * status line. Returns NULL with err on failure. */
upstream_t *fetch_open(const char *url, int *status, char **ctype, buf_t *rest,
                       char *err, size_t errlen)
{
    char *cur = xstrdup(url);
    memset(rest, 0, sizeof *rest);
    *ctype = NULL;
    for (int hop = 0; hop < 6; hop++) {
        int tls, port, code;
        char host[256];
        const char *path;
        if (parse_url(cur, &tls, host, sizeof host, &port, &path)) {
            snprintf(err, errlen, "bad URL %s", cur);
            break;
        }
        upstream_t *u = up_connect(host, port, tls, 30000, &code, err, errlen);
        if (!u)
            break;
        buf_t req = {0};
        buf_printf(&req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " UA "\r\n"
                         "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
        int rc = up_write_all(u, req.p, req.n);
        buf_free(&req);
        buf_t hb = {0};
        char tmp[4096], *eoh = NULL;
        int n;
        while (!rc && hb.n < 16384 && (n = up_read(u, tmp, sizeof tmp)) > 0) {
            buf_add(&hb, tmp, (size_t)n);
            if ((eoh = strstr(buf_cstr(&hb), "\r\n\r\n")) != NULL)
                break;
        }
        if (!eoh || (strncmp(hb.p, "HTTP/", 5) && strncmp(hb.p, "ICY ", 4))) {
            snprintf(err, errlen, "no HTTP response from %s", host);
            buf_free(&hb);
            up_close(u);
            break;
        }
        *eoh = 0;
        *status = atoi(strchr(hb.p, ' ') + 1);
        char *loc = header_value(hb.p, "Location");
        if (*status >= 300 && *status < 400 && loc) {
            buf_free(&hb);
            up_close(u);
            if (loc[0] == '/') {
                const char *s = strstr(cur, "://");
                size_t keep = s ? (size_t)(s + 3 - cur) + strcspn(s + 3, "/") : 0;
                buf_t b = {0};
                buf_add(&b, cur, keep);
                buf_puts(&b, loc);
                free(loc);
                loc = buf_cstr(&b);
            }
            free(cur);
            cur = loc;
            continue;
        }
        free(loc);
        *ctype = header_value(hb.p, "Content-Type");
        size_t hlen = (size_t)(eoh - hb.p) + 4;
        buf_add(rest, hb.p + hlen, hb.n - hlen);
        buf_free(&hb);
        free(cur);
        return u;
    }
    free(cur);
    return NULL;
}
