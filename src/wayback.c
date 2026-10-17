/* The Premium Services tab: Microsoft's own page from July 26, 2003, taken
 * from the Internet Archive's Wayback Machine.
 *
 * Nothing Microsoft-owned ships with wmpmeta. The first time the tab opens,
 * the page is fetched from web.archive.org and saved next to the exe as
 * wayback-services.html; after that the saved copy is served, so it keeps
 * working when the archive is slow or down.
 *
 * The archive kept the HTML but none of its images or its stylesheet, so
 * while serving it wmpmeta swaps each logo <img> for a text logo, supplies a
 * small stylesheet for the page's text classes, and turns off the long-dead
 * sign-up links. The page text itself is left as it was. */
#include "wmpmeta.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WB_URL "https://web.archive.org/web/20030726032509id_/" \
               "http://windowsmedia.com:80/Services/default.asp"

static CRITICAL_SECTION wb_lock;
static char            *raw;   /* the archived HTML, once loaded */

void wayback_init(void)
{
    InitializeCriticalSection(&wb_lock);
}

static void cache_path(char *out, size_t n)
{
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    snprintf(s ? s + 1 : out, n - (size_t)(s ? s + 1 - out : 0), "wayback-services.html");
}

static char *load(void)
{
    char path[MAX_PATH];
    cache_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f) {
        buf_t b = {0};
        char tmp[4096];
        size_t n;
        while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
            buf_add(&b, tmp, n);
        fclose(f);
        if (b.n)
            return buf_cstr(&b);
        buf_free(&b);
    }
    fetch_t r;
    char err[300];
    if (fetch_url(WB_URL, &r, err, sizeof err)) {
        log_msg("wayback: %s", err);
        return NULL;
    }
    if (r.status != 200 || !strstr(buf_cstr(&r.body), "Premium Services")) {
        log_msg("wayback: archive answered %d", r.status);
        fetch_free(&r);
        return NULL;
    }
    if ((f = fopen(path, "wb")) != NULL) {
        fwrite(r.body.p, 1, r.body.n, f);
        fclose(f);
        log_msg("wayback: saved %s", path);
    }
    char *s = xstrdup(r.body.p);
    fetch_free(&r);
    return s;
}

/* Text stand-ins for the logos the archive didn't keep. */
static const char *logo_for(const char *file)
{
    if (!_strnicmp(file, "pressplay", 9))
        return "press<i>play</i>";
    if (!_strnicmp(file, "cinemanow", 9))
        return "CinemaNow";
    if (!_strnicmp(file, "mn_", 3))
        return "MusicNow";
    if (!_strnicmp(file, "intertainer", 11))
        return "Intertainer";
    return NULL;
}

static const char css[] =
    "<style type=\"text/css\">"
    "body{margin:10px;font-family:Tahoma,Verdana,Arial,sans-serif;font-size:11px;color:#000}"
    ".textTitleLarge{font-family:Trebuchet MS,Tahoma,sans-serif;font-size:20px;font-weight:bold;"
    "color:#123a73;margin:0 0 6px 0}"
    ".text{font-size:11px;color:#000}"
    ".textBold{font-size:11px;font-weight:bold;color:#000;text-decoration:none}"
    ".textLarge{font-family:Trebuchet MS,Tahoma,sans-serif;font-size:15px;font-weight:bold;color:#123a73}"
    ".logo{display:block;border:1px solid #6b8fc7;background:#e8eef8;color:#123a73;"
    "font-family:Trebuchet MS,Tahoma,sans-serif;font-size:18px;font-weight:bold;"
    "padding:6px 12px;margin-bottom:4px}"
    ".wbnote{color:#888;font-size:9px;margin-top:20px;border-top:1px solid #ccc;padding-top:4px}"
    "</style>";

static char *rewrite(const char *s)
{
    buf_t o = {0};
    while (*s) {
        if (!_strnicmp(s, "<link ", 6)) {  /* the lost stylesheet: ours instead */
            const char *e = strchr(s, '>');
            buf_puts(&o, css);
            s = e ? e + 1 : s + strlen(s);
            continue;
        }
        if (!_strnicmp(s, "<img ", 5)) {
            const char *e = strchr(s, '>');
            const char *src = strstr(s, "src=");
            const char *logo = NULL;
            if (src && e && src < e) {
                src += 4;
                if (*src == '"')
                    src++;
                logo = logo_for(src);
            }
            if (logo) {
                buf_puts(&o, "<span class=logo>");
                buf_puts(&o, logo);
                buf_puts(&o, "</span>");
            }
            s = e ? e + 1 : s + strlen(s);
            continue;
        }
        if (!_strnicmp(s, "href=\"http", 10)) {  /* dead sign-up links */
            const char *q = strchr(s + 6, '"');
            buf_puts(&o, "href=\"#\" onclick=\"return false\"");
            s = q ? q + 1 : s + strlen(s);
            continue;
        }
        if (!_strnicmp(s, "</body>", 7)) {
            buf_puts(&o, "<div class=wbnote>Archived copy of the Windows Media Player Premium "
                         "Services page as it was on July 26, 2003, from the Internet Archive's "
                         "Wayback Machine. The original logos weren't archived. These services "
                         "closed long ago, so the sign-up links are turned off.</div>");
        }
        buf_add(&o, s, 1);
        s++;
    }
    return buf_cstr(&o);
}

char *wayback_services(void)
{
    EnterCriticalSection(&wb_lock);
    if (!raw)
        raw = load();
    char *page = raw ? rewrite(raw) : NULL;
    LeaveCriticalSection(&wb_lock);
    return page;
}
