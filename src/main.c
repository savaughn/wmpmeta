/* wmpmeta: answers WMP9's CD metadata requests on 127.0.0.1:80.
 *
 * The hosts file points windowsmedia.com and services.windowsmedia.com here.
 *   GET  /redir/GetMDRCD.asp?cd=<TOC>&...       album lookup (also the
 *        ...Background.asp variant WMP uses for its library pass)
 *   GET  /redir/QueryTOCExt.asp?cd=&wmid=       View Album Info (album.c)
 *   GET  /redir/submittoc.asp?cd=&wmid=         Find Album Info (album.c)
 *   GET  /wmpmeta/usealbum?cd=&mbid=            save a chosen album, return its XML
 *   GET  /redir/mediaguide.asp                  Media Guide page (guide.c)
 *   GET  /wmpmeta/img?u=URL                     https image served as http
 *   GET  /redir/radiotuner.asp?tag=|q=          Radio Tuner page (radio.c)
 *   GET  /redir/services.asp                    archived Premium Services (wayback.c)
 *   GET  /wmpmeta/play.asx?id=UUID              ASX playlist for one station
 *   GET  /wmpmeta/stream?id=UUID                the station's audio, relayed
 *   GET  /cover/<release MBID>                  services.windowsmedia.com art
 *   GET  /fwlink/?LinkId=9705, 9706, 7262       More Skins / Visualizations /
 *        Plug-ins galleries (skins.c);
 *        other go.microsoft.com links are passed on to the real one
 *   GET  /wmpmeta/skin/<file>.wmz               a skin, downloaded once and kept
 *   GET  /wmpmeta/skinimg/<n>                   a skin's preview picture
 *   GET  /wmpmeta/viz/<file>.exe, vizimg/<n>    the same for visualizations
 *   GET  /wmpmeta/plug/<file>.exe, plugimg/<n>  and for plug-ins
 *   GET  /wmpmeta/quit                          stops the server (wmpmeta -stop)
 * Everything else gets a 404 and a log line, so new request kinds show up in
 * wmpmeta.log next to the exe.
 *
 *   wmpmeta            run (no window; GUI subsystem)
 *   wmpmeta -stop      stop a running copy
 *   wmpmeta -p PORT    listen on another port (testing)
 *   wmpmeta -q TOC     print the XML for a TOC to wmpmeta.log and exit
 *   wmpmeta -getskins  download everything the galleries offer */
#include "wmpmeta.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SOCKET g_listen = INVALID_SOCKET;
static int    g_port = 80;

static void reply_cached(SOCKET c, int status, const char *ctype, const void *body, size_t n,
                         int max_age)
{
    const char *why = status == 200 ? "OK" : status == 404 ? "Not Found" : "Error";
    char h[256], cc[40];
    if (max_age)
        snprintf(cc, sizeof cc, "max-age=%d", max_age);
    else
        snprintf(cc, sizeof cc, "no-cache");
    int hl = snprintf(h, sizeof h,
                      "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                      "Cache-Control: %s\r\nConnection: close\r\n\r\n",
                      status, why, ctype, (unsigned)n, cc);
    sock_send_all(c, h, (size_t)hl);
    if (n)
        sock_send_all(c, body, n);
}

/* The LinkId of a go.microsoft.com/fwlink URL (lowercased), or -1. */
static int fwlink_id(const char *lower)
{
    const char *p = strstr(lower, "linkid=");
    return p ? atoi(p + 7) : -1;
}

static void reply(SOCKET c, int status, const char *ctype, const void *body, size_t n)
{
    reply_cached(c, status, ctype, body, n, 0);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hx[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hx, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;  /* '+' stays: it separates TOC fields */
        }
    }
    *o = 0;
}

/* Value of a query parameter, case-insensitive name, decoded. malloc'd. */
static char *query_param(const char *uri, const char *name)
{
    size_t nl = strlen(name);
    for (const char *sep = strchr(uri, '?'); sep; sep = strchr(sep + 1, '&')) {
        const char *p = sep + 1;
        if (!_strnicmp(p, name, nl) && p[nl] == '=') {
            char *v = xstrndup(p + nl + 1, strcspn(p + nl + 1, "&"));
            url_decode(v);
            return v;
        }
    }
    return NULL;
}

/* Like query_param, but for HTML form fields, where '+' means a space. */
static char *form_param(const char *uri, const char *name)
{
    size_t nl = strlen(name);
    for (const char *sep = strchr(uri, '?'); sep; sep = strchr(sep + 1, '&')) {
        const char *p = sep + 1;
        if (!_strnicmp(p, name, nl) && p[nl] == '=') {
            char *v = xstrndup(p + nl + 1, strcspn(p + nl + 1, "&"));
            for (char *q = v; *q; q++)
                if (*q == '+')
                    *q = ' ';
            url_decode(v);
            return v;
        }
    }
    return NULL;
}

static const char *find_mbid(const char *s)
{
    for (; *s; s++) {
        int ok = 1;
        for (int i = 0; i < 36 && ok; i++) {
            char ch = s[i];
            ok = (i == 8 || i == 13 || i == 18 || i == 23) ? ch == '-' : isxdigit((unsigned char)ch);
        }
        if (ok)
            return s;
    }
    return NULL;
}

static const char wb_down[] =
    "<html><body style=\"font:11px Tahoma\">The archived Premium Services page couldn't be "
    "fetched from the Internet Archive. Try again later.</body></html>";

static const char no_match[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<METADATA xmlns:sql=\"urn:schemas-microsoft-com:xml-sql\"></METADATA>";

static DWORD WINAPI client_thread(void *arg)
{
    SOCKET c = (SOCKET)(UINT_PTR)arg;
    buf_t req = {0};
    char tmp[8192];
    int n;
    char *eoh = NULL;
    while ((n = sock_recv(c, tmp, sizeof tmp, 15000)) > 0) {
        buf_add(&req, tmp, (size_t)n);
        if ((eoh = strstr(buf_cstr(&req), "\r\n\r\n")) != NULL)
            break;
        if (req.n > 65536)
            break;
    }
    if (!eoh) {
        buf_free(&req);
        sock_close(c);
        return 0;
    }
    /* Read a POST body so it can be logged. */
    const char *cl = strstr(req.p, "Content-Length:");
    if (!cl)
        cl = strstr(req.p, "content-length:");
    size_t want = cl ? (size_t)atol(cl + 15) : 0, have = req.n - (size_t)(eoh + 4 - req.p);
    while (have < want && have < 262144 && (n = sock_recv(c, tmp, sizeof tmp, 15000)) > 0) {
        size_t off = (size_t)(eoh - req.p);
        buf_add(&req, tmp, (size_t)n);
        eoh = req.p + off;
        have += (size_t)n;
    }
    buf_cstr(&req);

    char method[8] = "", *uri;
    size_t ml = strcspn(req.p, " ");
    snprintf(method, sizeof method, "%.*s", (int)(ml < 7 ? ml : 7), req.p);
    const char *u = req.p + ml + (req.p[ml] ? 1 : 0);
    uri = xstrndup(u, strcspn(u, " \r\n"));
    const char *hostp = strstr(req.p, "\nHost:");
    char host[128] = "";
    if (hostp) {
        hostp += 6;
        while (*hostp == ' ')
            hostp++;
        snprintf(host, sizeof host, "%.*s", (int)strcspn(hostp, "\r\n:"), hostp);
    }
    log_msg("%s %s%s", method, host, uri);
    if (have && eoh[4])
        log_msg("  body: %.2000s", eoh + 4);

    char *lower = xstrdup(uri);
    for (char *p = lower; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    if (strstr(lower, "/wmpmeta/quit")) {
        reply(c, 200, "text/plain", "bye\r\n", 5);
        log_msg("quit requested");
        sock_close(c);
        closesocket(g_listen);
        ExitProcess(0);
    } else if (strstr(lower, "getmdrcd") && !strstr(lower, "posturl")) {
        char *toc = query_param(uri, "cd");
        if (!toc)
            toc = query_param(uri, "toc");
        char *xml = toc ? mb_lookup_toc(toc) : NULL;
        if (xml) {
            reply(c, 200, "text/xml; charset=utf-8", xml, strlen(xml));
            if (g_verbose)
                log_msg("  -> %s", xml);
        } else {
            reply(c, 200, "text/xml; charset=utf-8", no_match, sizeof no_match - 1);
        }
        free(xml);
        free(toc);
    } else if (!strncmp(lower, "/fwlink", 7) &&
               (fwlink_id(lower) == 9705 || fwlink_id(lower) == 9706 ||
                fwlink_id(lower) == 7262)) {
        /* WMP's More Skins (9705), Visualizations (9706), Plug-ins (7262) */
        int id = fwlink_id(lower);
        char *page = skins_page(id == 9706 ? SK_VIZ : id == 7262 ? SK_PLUG : SK_SKINS);
        reply(c, 200, "text/html; charset=utf-8", page, strlen(page));
        free(page);
    } else if (!strncmp(lower, "/fwlink", 7) && !_stricmp(host, "go.microsoft.com")) {
        /* Any other go.microsoft.com link: ask the real one where it goes. */
        char url[1200], err[300];
        int st;
        snprintf(url, sizeof url, "https://go.microsoft.com%s", uri);
        char *loc = fetch_location(url, &st, err, sizeof err);
        if (loc && st >= 300 && st < 400) {
            char h[1400];
            int hl = snprintf(h, sizeof h, "HTTP/1.0 302 Found\r\nLocation: %s\r\n"
                                           "Content-Length: 0\r\nConnection: close\r\n\r\n", loc);
            sock_send_all(c, h, (size_t)hl);
            log_msg("  -> %s", loc);
        } else {
            if (!loc && st == 0)
                log_msg("fwlink: %s", err);
            reply(c, 404, "text/html", "", 0);
        }
        free(loc);
    } else if (strstr(lower, "/9series/personalization/skins.asp") ||
               !strcmp(lower, "/wmpmeta/skins") || !strcmp(lower, "/wmpmeta/viz") ||
               !strcmp(lower, "/wmpmeta/plugins")) {
        char *page = skins_page(strstr(lower, "skins") ? SK_SKINS
                                : strstr(lower, "plugins") ? SK_PLUG : SK_VIZ);
        reply(c, 200, "text/html; charset=utf-8", page, strlen(page));
        free(page);
    } else if (!strncmp(lower, "/wmpmeta/skin/", 14) || !strncmp(lower, "/wmpmeta/viz/", 13) ||
               !strncmp(lower, "/wmpmeta/plug/", 14)) {
        int kind = lower[9] == 'v' ? SK_VIZ : lower[9] == 'p' ? SK_PLUG : SK_SKINS;
        buf_t b;
        if (!skins_file(kind, uri + (kind == SK_VIZ ? 13 : 14), &b))
            reply_cached(c, 200, kind == SK_SKINS ? "application/x-ms-wmz" : "application/octet-stream",
                         b.p, b.n, 86400);
        else
            reply(c, 404, "text/plain", "", 0);
        buf_free(&b);
    } else if (!strncmp(lower, "/wmpmeta/skinimg/", 17) || !strncmp(lower, "/wmpmeta/vizimg/", 16) ||
               !strncmp(lower, "/wmpmeta/plugimg/", 17)) {
        int kind = lower[9] == 'v' ? SK_VIZ : lower[9] == 'p' ? SK_PLUG : SK_SKINS;
        buf_t b;
        const char *ct = "image/jpeg";
        if (!skins_image(kind, atoi(lower + (kind == SK_VIZ ? 16 : 17)), &b, &ct))
            reply_cached(c, 200, ct, b.p, b.n, 86400);
        else
            reply(c, 404, "text/plain", "", 0);
        buf_free(&b);
    } else if (strstr(lower, "/redir/querytocext.asp") || strstr(lower, "/redir/submittoc.asp")) {
        char *toc = form_param(uri, "cd"), *wmid = form_param(uri, "wmid");
        char *page;
        if (strstr(lower, "querytocext")) {
            char *use = form_param(uri, "use");
            page = album_view(toc, wmid, use);
            free(use);
        } else {
            char *artist = form_param(uri, "artist"), *album = form_param(uri, "album");
            page = album_find(toc, wmid, artist, album);
            free(artist);
            free(album);
        }
        reply(c, 200, "text/html; charset=utf-8", page, strlen(page));
        free(page);
        free(toc);
        free(wmid);
    } else if (!strncmp(lower, "/wmpmeta/usealbum?", 18)) {
        char *toc = form_param(uri, "cd"), *mbid = form_param(uri, "mbid");
        char *xml = toc && mbid ? mb_use_album(toc, mbid) : NULL;
        if (xml)
            reply(c, 200, "text/xml; charset=utf-8", xml, strlen(xml));
        else
            reply(c, 502, "text/plain", "", 0);
        free(xml);
        free(toc);
        free(mbid);
    } else if (strstr(lower, "/redir/mediaguide.asp") || strstr(lower, "/mg/home.asp")) {
        char *page = guide_page();
        reply(c, 200, "text/html; charset=utf-8", page, strlen(page));
        free(page);
    } else if (strstr(lower, "/redir/services.asp")) {
        char *page = wayback_services();
        if (page)
            reply(c, 200, "text/html; charset=iso-8859-1", page, strlen(page));
        else
            reply(c, 503, "text/html", wb_down, sizeof wb_down - 1);
        free(page);
    } else if (strstr(lower, "/redir/radiotuner.asp")) {
        char *tag = form_param(uri, "tag"), *q = form_param(uri, "q");
        char *sort = form_param(uri, "sort");
        char *page = radio_page(tag, q, sort);
        free(sort);
        reply(c, 200, "text/html; charset=utf-8", page, strlen(page));
        free(page);
        free(tag);
        free(q);
    } else if (!strncmp(lower, "/wmpmeta/play.asx?", 18)) {
        char *id = query_param(uri, "id");
        char *asx = radio_asx(id);
        if (asx)
            reply(c, 200, "video/x-ms-asf", asx, strlen(asx));
        else
            reply(c, 404, "text/plain", "", 0);
        free(asx);
        free(id);
    } else if (!strncmp(lower, "/wmpmeta/stream?", 16)) {
        char *id = query_param(uri, "id");
        radio_stream(c, id);
        free(id);
    } else if (!strncmp(lower, "/wmpmeta/static/", 16)) {
        const unsigned char *data;
        size_t len;
        if (!guide_static(lower + 16, &data, &len))
            reply_cached(c, 200, "image/gif", data, len, 86400);
        else
            reply(c, 404, "text/plain", "", 0);
    } else if (!strncmp(lower, "/wmpmeta/img?", 13)) {
        char *u = query_param(uri, "u");
        fetch_t r;
        if (u && !guide_image(u, &r)) {
            reply_cached(c, 200, r.ctype, r.body.p, r.body.n, 86400);
            fetch_free(&r);
        } else {
            reply(c, 404, "text/plain", "", 0);
        }
        free(u);
    } else if (strstr(lower, "/cover/") || strstr(lower, "dvdcover")) {
        const char *id = find_mbid(lower);
        fetch_t r;
        if (id) {
            char mbid[37];
            memcpy(mbid, id, 36);
            mbid[36] = 0;
            if (!mb_cover(mbid, &r)) {
                reply_cached(c, 200, "image/jpeg", r.body.p, r.body.n, 86400);
                fetch_free(&r);
                goto done;
            }
            fetch_free(&r);
        }
        reply(c, 404, "text/plain", "", 0);
    } else {
        reply(c, 404, "text/html", "", 0);
    }
done:
    free(lower);
    free(uri);
    buf_free(&req);
    sock_close(c);
    return 0;
}

static void exe_dir_file(char *out, size_t n, const char *name)
{
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    snprintf(s ? s + 1 : out, n - (size_t)(s ? s + 1 - out : 0), "%s", name);
}

static int stop_running(void)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)g_port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&sa, sizeof sa)) {
        log_msg("-stop: nothing listening on 127.0.0.1:%d", g_port);
        return 1;
    }
    const char q[] = "GET /wmpmeta/quit HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    send(s, q, sizeof q - 1, 0);
    char tmp[256];
    recv(s, tmp, sizeof tmp, 0);
    closesocket(s);
    log_msg("-stop: sent");
    return 0;
}

int main(int argc, char **argv)
{
    char logpath[MAX_PATH];
    exe_dir_file(logpath, sizeof logpath, "wmpmeta.log");
    freopen(logpath, "a", stdout);
    util_init();
    mb_init();
    guide_init();
    radio_init();
    wayback_init();
    skins_init();

    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) {
        log_msg("WSAStartup failed");
        return 1;
    }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v"))
            g_verbose = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            g_port = atoi(argv[++i]);
    if (argc > 1 && !strcmp(argv[1], "-stop"))
        return stop_running();

    char err[300];
    if (tls_init(NULL, err, sizeof err)) {
        log_msg("TLS init: %s", err);
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "-getskins"))
        return skins_fetch_all();
    if (argc > 2 && !strcmp(argv[1], "-q")) {
        char *xml = mb_lookup_toc(argv[2]);
        log_msg("%s", xml ? xml : "(no match)");
        free(xml);
        return xml ? 0 : 2;
    }

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)g_port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(g_listen, (struct sockaddr *)&sa, sizeof sa) || listen(g_listen, 16)) {
        log_msg("can't listen on 127.0.0.1:%d (WSA %d); already running?", g_port, WSAGetLastError());
        return 1;
    }
    log_msg("wmpmeta " WM_VERSION " listening on 127.0.0.1:%d", g_port);
    for (;;) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET)
            break;
        HANDLE h = CreateThread(NULL, 0, client_thread, (void *)(UINT_PTR)c, 0, NULL);
        if (h)
            CloseHandle(h);
        else
            closesocket(c);
    }
    return 0;
}
