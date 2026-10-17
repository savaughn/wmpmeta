/* The Media Guide page: music news and new albums from RSS feeds plus
 * ListenBrainz's weekly artist chart, rendered as plain table-layout HTML
 * that IE 5/6 inside WMP9 can show. Images are https on the web, so the
 * page points them at /wmpmeta/img, which fetches a small thumbnail over
 * TLS and serves it as plain HTTP.
 *
 * Sources come from wmpmeta.ini next to the exe:
 *   news=URL     RSS feed for the news column (repeatable)
 *   albums=URL   RSS feed shown as the cover grid (repeatable)
 *   charts=1     show ListenBrainz's weekly artist chart
 * Without an ini, the defaults below are used. */
#include "wmpmeta.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "guide_img.h"

#define MAX_FEEDS   8
#define MAX_ITEMS   64
#define NEWS_SHOWN  14
#define ALBUMS_SHOWN 8
#define CACHE_MS    (30 * 60 * 1000)

static const char *def_news[] = {"https://pitchfork.com/feed/feed-news/rss",
                                 "https://www.stereogum.com/feed/", NULL};
static const char *def_albums[] = {"https://pitchfork.com/feed/feed-album-reviews/rss", NULL};

typedef struct {
    char *title, *summary, *image, *source, *date;
    char  sortkey[16];  /* yyyymmddhhmmss */
} item_t;

typedef struct {
    item_t it[MAX_ITEMS];
    int    n;
} items_t;

static CRITICAL_SECTION guide_lock;
static char            *cached;
static DWORD            cached_at;

static char *news_feeds[MAX_FEEDS + 1], *album_feeds[MAX_FEEDS + 1];
static int   show_charts = 0;  /* ListenBrainz's sitewide chart is one fandom deep */

/* ---------------------------------------------------------------- config */

static void load_ini(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *s = strrchr(path, '\\');
    snprintf(s ? s + 1 : path, sizeof path - (size_t)(s ? s + 1 - path : 0), "wmpmeta.ini");
    int nn = 0, na = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *e = line + strlen(line);
            while (e > line && isspace((unsigned char)e[-1]))
                *--e = 0;
            char *p = line;
            while (isspace((unsigned char)*p))
                p++;
            if (!_strnicmp(p, "news=", 5) && nn < MAX_FEEDS)
                news_feeds[nn++] = xstrdup(p + 5);
            else if (!_strnicmp(p, "albums=", 7) && na < MAX_FEEDS)
                album_feeds[na++] = xstrdup(p + 7);
            else if (!_strnicmp(p, "charts=", 7))
                show_charts = atoi(p + 7) != 0;
        }
        fclose(f);
        log_msg("guide: %s: %d news, %d album feeds", path, nn, na);
    }
    if (!f || (!nn && !na)) {
        for (int i = 0; def_news[i]; i++)
            news_feeds[nn++] = xstrdup(def_news[i]);
        for (int i = 0; def_albums[i]; i++)
            album_feeds[na++] = xstrdup(def_albums[i]);
    }
}

void guide_init(void)
{
    InitializeCriticalSection(&guide_lock);
    load_ini();
}

/* ---------------------------------------------------------------- text */

static void put_utf8(buf_t *b, unsigned long c)
{
    char u[4];
    int n;
    if (c < 0x80) {
        u[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        u[0] = (char)(0xC0 | c >> 6);
        u[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        u[0] = (char)(0xE0 | c >> 12);
        u[1] = (char)(0x80 | (c >> 6 & 0x3F));
        u[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | c >> 18);
        u[1] = (char)(0x80 | (c >> 12 & 0x3F));
        u[2] = (char)(0x80 | (c >> 6 & 0x3F));
        u[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    buf_add(b, u, (size_t)n);
}

/* Decodes entities; with strip, also drops tags and squeezes whitespace. */
static void decode(buf_t *b, const char *s, size_t n, int strip)
{
    static const struct { const char *name; unsigned long c; } ents[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", ' '}, {"hellip", 0x2026}, {"rsquo", 0x2019}, {"lsquo", 0x2018},
        {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"mdash", 0x2014}, {"ndash", 0x2013}};
    const char *end = s + n;
    int space = 0;
    while (s < end) {
        if (strip && *s == '<') {
            const char *gt = memchr(s, '>', (size_t)(end - s));
            s = gt ? gt + 1 : end;
            space = 1;
            continue;
        }
        if (*s == '&') {
            const char *semi = memchr(s, ';', (size_t)(end - s) < 10 ? (size_t)(end - s) : 10);
            unsigned long c = 0;
            if (semi && s[1] == '#')
                c = s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
            else if (semi)
                for (size_t i = 0; i < sizeof ents / sizeof *ents; i++)
                    if ((size_t)(semi - s - 1) == strlen(ents[i].name) &&
                        !strncmp(s + 1, ents[i].name, (size_t)(semi - s - 1)))
                        c = ents[i].c;
            if (c) {
                if (space && b->n)
                    buf_add(b, " ", 1);
                space = 0;
                put_utf8(b, c);
                s = semi + 1;
                continue;
            }
        }
        if (strip && isspace((unsigned char)*s)) {
            space = 1;
            s++;
            continue;
        }
        if (space && b->n)
            buf_add(b, " ", 1);
        space = 0;
        buf_add(b, s, 1);
        s++;
    }
}

/* Content of <tag>...</tag> inside [p, end), CDATA unwrapped. NULL if absent. */
static const char *element(const char *p, const char *end, const char *tag, size_t *len)
{
    char open[64];
    snprintf(open, sizeof open, "<%s", tag);
    size_t ol = strlen(open);
    for (const char *s = p; s && s < end; s++) {
        s = strstr(s, open);
        if (!s || s >= end)
            return NULL;
        char c = s[ol];
        if (c != '>' && c != ' ' && c != '/')
            continue;
        const char *gt = strchr(s, '>');
        if (!gt || gt >= end || gt[-1] == '/')
            return NULL;
        char close[64];
        snprintf(close, sizeof close, "</%s>", tag);
        const char *ce = strstr(gt + 1, close);
        if (!ce || ce > end)
            return NULL;
        const char *v = gt + 1;
        while (v < ce && isspace((unsigned char)*v))
            v++;
        if (!strncmp(v, "<![CDATA[", 9)) {
            const char *cd = strstr(v, "]]>");
            if (cd && cd < ce) {
                *len = (size_t)(cd - v - 9);
                return v + 9;
            }
        }
        *len = (size_t)(ce - v);
        return v;
    }
    return NULL;
}

/* url="..." of the first <tag ...> carrying one, within [p, end). */
static char *attr_url(const char *p, const char *end, const char *tag)
{
    char open[64];
    snprintf(open, sizeof open, "<%s ", tag);
    for (const char *s = p; (s = strstr(s, open)) != NULL && s < end; s++) {
        const char *gt = strchr(s, '>');
        const char *u = strstr(s, "url=\"");
        if (u && gt && u < gt) {
            u += 5;
            const char *q = strchr(u, '"');
            if (q && q < gt) {
                buf_t b = {0};
                decode(&b, u, (size_t)(q - u), 0);
                return buf_cstr(&b);
            }
        }
    }
    return NULL;
}

static char *text_of(const char *item, const char *end, const char *tag, int strip)
{
    size_t n;
    const char *v = element(item, end, tag, &n);
    if (!v)
        return NULL;
    buf_t b = {0};
    decode(&b, v, n, strip);
    if (strip) {  /* entities may have produced markup, e.g. &lt;em&gt; */
        buf_t c = {0};
        decode(&c, b.p ? b.p : "", b.n, 1);
        buf_free(&b);
        b = c;
    }
    return b.n ? buf_cstr(&b) : (buf_free(&b), NULL);
}

/* "Fri, 16 Oct 2026 16:58:23 +0000" -> "20261016165823" and "16 Oct". */
static void rfc822(const char *d, char key[16], char **shown)
{
    static const char *mons = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int day = 0, year = 0, h = 0, mi = 0, se = 0;
    char mon[4] = "";
    const char *p = strchr(d, ',');
    p = p ? p + 1 : d;
    key[0] = 0;
    *shown = NULL;
    if (sscanf(p, "%d %3s %d %d:%d:%d", &day, mon, &year, &h, &mi, &se) < 3)
        return;
    const char *m = strstr(mons, mon);
    int mn = m && mon[0] ? (int)(m - mons) / 3 + 1 : 0;
    snprintf(key, 16, "%04d%02d%02d%02d%02d%02d", year, mn, day, h, mi, se);
    char s[16];
    snprintf(s, sizeof s, "%d %s", day, mon);
    *shown = xstrdup(s);
}

/* Asks image CDNs for a small rendition when their URL scheme allows it. */
static char *thumb_url(const char *u)
{
    buf_t b = {0};
    const char *pass = strstr(u, "/master/pass/");
    if (strstr(u, "media.pitchfork.com/") && pass) {
        buf_add(&b, u, (size_t)(pass - u));
        buf_puts(&b, "/master/w_160,c_limit/");
        buf_puts(&b, pass + 13);
    } else if (strstr(u, "/wp-content/uploads/")) {
        buf_add(&b, u, strcspn(u, "?"));
        buf_puts(&b, "?w=160");
    } else {
        buf_puts(&b, u);
    }
    return buf_cstr(&b);
}

/* "https://www.stereogum.com/feed/" -> "Stereogum". Feed titles are often
 * things like "RSS: News", so the domain names the source better. */
static char *site_name(const char *url)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t n = strcspn(h, "/:?");
    char *host = xstrndup(h, n);
    char *dot = strrchr(host, '.');
    if (dot)
        *dot = 0;               /* drop the TLD */
    char *name = strrchr(host, '.');
    name = xstrdup(name ? name + 1 : host);
    free(host);
    if (name[0])
        name[0] = (char)toupper((unsigned char)name[0]);
    return name;
}

static void free_item(item_t *it)
{
    free(it->title);
    free(it->summary);
    free(it->image);
    free(it->source);
    free(it->date);
    memset(it, 0, sizeof *it);
}

static void read_feed(const char *url, items_t *out)
{
    fetch_t r;
    char err[300];
    if (fetch_url(url, &r, err, sizeof err) || r.status != 200) {
        log_msg("guide: %s: %s", url, r.status ? "bad status" : err);
        if (r.status)
            fetch_free(&r);
        return;
    }
    const char *d = buf_cstr(&r.body);
    char *source = site_name(url);
    int got = 0;
    for (const char *p = d; out->n < MAX_ITEMS && (p = strstr(p, "<item")) != NULL;) {
        if (p[5] != '>' && p[5] != ' ') {
            p += 5;
            continue;
        }
        const char *ie = strstr(p, "</item>");
        if (!ie)
            break;
        item_t *it = &out->it[out->n];
        it->title = text_of(p, ie, "title", 1);
        it->summary = text_of(p, ie, "description", 1);
        /* WordPress boilerplate says nothing; drop it. */
        if (it->summary && !strncmp(it->summary, "The post ", 9) && strstr(it->summary, "appeared first on")) {
            free(it->summary);
            it->summary = NULL;
        }
        char *img = attr_url(p, ie, "media:thumbnail");
        if (!img)
            img = attr_url(p, ie, "media:content");
        if (!img)
            img = attr_url(p, ie, "enclosure");
        if (img) {
            it->image = thumb_url(img);
            free(img);
        }
        char *pd = text_of(p, ie, "pubDate", 0);
        if (pd) {
            rfc822(pd, it->sortkey, &it->date);
            free(pd);
        }
        it->source = source ? xstrdup(source) : NULL;
        if (it->title) {
            out->n++;
            got++;
        } else {
            free_item(it);
        }
        p = ie + 7;
    }
    log_msg("guide: %d items from %s", got, url);
    free(source);
    fetch_free(&r);
}

static int by_date(const void *a, const void *b)
{
    return strcmp(((const item_t *)b)->sortkey, ((const item_t *)a)->sortkey);
}

/* ---------------------------------------------------------------- HTML */

static void esc(buf_t *b, const char *s)
{
    for (; s && *s; s++) {
        switch (*s) {
        case '&': buf_puts(b, "&amp;"); break;
        case '<': buf_puts(b, "&lt;"); break;
        case '>': buf_puts(b, "&gt;"); break;
        case '"': buf_puts(b, "&quot;"); break;
        default: buf_add(b, s, 1);
        }
    }
}

/* Cuts at a word boundary, counting UTF-8 characters. */
static void esc_trunc(buf_t *b, const char *s, int maxchars)
{
    int chars = 0;
    const char *p = s, *cut = NULL;
    for (; *p; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80 && ++chars > maxchars)
            break;
        if (*p == ' ')
            cut = p;
    }
    if (!*p) {
        esc(b, s);
        return;
    }
    char *t = xstrndup(s, (size_t)((cut ? cut : p) - s));
    esc(b, t);
    free(t);
    buf_puts(b, "&#8230;");
}

static void img_src(buf_t *b, const char *url)
{
    buf_puts(b, "/wmpmeta/img?u=");
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (isalnum(*p) || strchr("-_.~/:", *p))
            buf_add(b, p, 1);
        else
            buf_printf(b, "%%%02X", *p);
    }
}

/* Styled after the WindowsMedia.com Media Guide of 2002-2003: black header
 * band, royal-blue textured feature area with a category nav on the left,
 * and a white "more" list beside a grey "Top 10" box. Verdana at the small
 * sizes the original used. No Microsoft artwork; the texture and gradient
 * are generated (guide_img.h). */
static const char css_base[] =
    "body{margin:0;background:#fff;font-family:Verdana,Arial,sans-serif;font-size:10px;color:#000}"
    "a{color:#000099;text-decoration:none}"
    "td{font-family:Verdana,Arial,sans-serif;font-size:10px}"
    ".logo{font-family:Trebuchet MS,Verdana,sans-serif;font-size:22px;font-weight:bold;color:#fff;padding-left:8px;letter-spacing:-1px}"
    ".logo span{color:#99ccff}"
    ".date{font-family:Arial,sans-serif;font-size:15px;font-weight:bold;color:#999;padding-right:10px}"
    ".nav{color:#fff;font-weight:bold;font-size:10px;padding:2px 8px}"
    ".navsel{color:#fff;font-weight:bold;font-size:10px;padding:1px 7px;background:#003399;border:1px solid #ffff99}"
    ".nav a,.navsel a{color:#fff}"
    ".strip{background:#0033aa;border-bottom:1px solid #0066cc;padding:3px 4px}"
    ".sq{color:#99ccff;font-size:8px}"
    ".bar{background:#003399;color:#fff;font-weight:bold;font-size:10px;padding:3px 4px}"
    ".tt{color:#000099}"
    ".sum{color:#333}"
    ".foot{color:#999;font-size:9px;padding:6px 8px}";

static const char css[] =
    ".ft1{color:#fff;font-weight:bold;font-size:11px}"
    ".ft2{color:#fff;font-size:10px}"
    ".fl{color:#99ccff;font-size:10px}"
    ".cap{color:#fff;font-size:10px}"
    ".rh{color:#fff;font-weight:bold;font-size:10px;padding-top:6px}"
    ".rl{color:#fff;font-size:10px}"
    ".cat{font-weight:bold;color:#000}"
    ".meta{color:#0066cc}"
    ".tab{background:#fff;border:1px solid #003399;border-bottom:0;color:#000099;font-weight:bold;padding:2px 6px}"
    ".tabs{background:#0066cc;padding:3px 3px 0 3px}"
    ".num{color:#000;font-size:10px;width:16px}";

void html_esc(buf_t *b, const char *s)
{
    esc(b, s);
}

/* <head>, the shared stylesheet plus extra_css, and the black header band
 * with a two-tone wordmark ("Media" "Guide") and today's date. */
void html_page_start(buf_t *h, const char *title, const char *w1, const char *w2,
                     const char *extra_css)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const char *mons[] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
                                 "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
    buf_puts(h, "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">"
                "<title>");
    esc(h, title);
    buf_puts(h, "</title><style type=\"text/css\">");
    buf_puts(h, css_base);
    buf_puts(h, extra_css);
    buf_puts(h, "</style></head><body topmargin=0 leftmargin=0 marginwidth=0 marginheight=0>");
    buf_printf(h, "<table width=\"100%%\" cellpadding=0 cellspacing=0 border=0>"
                  "<tr><td height=44 bgcolor=#222222 background=\"/wmpmeta/static/hdr.gif\">"
                  "<table width=\"100%%\" cellpadding=0 cellspacing=0 border=0><tr>"
                  "<td class=logo>%s<span>%s</span></td>"
                  "<td align=right class=date>%s %d</td></tr></table></td></tr>"
                  "<tr><td height=2 bgcolor=#0066cc></td></tr></table>",
               w1, w2, mons[st.wMonth - 1], st.wDay);
}

static void cover(buf_t *h, const char *url, int w)
{
    if (!url) {
        buf_printf(h, "<table width=%d height=%d cellpadding=0 cellspacing=0 bgcolor=#0066cc>"
                      "<tr><td>&nbsp;</td></tr></table>", w, w);
        return;
    }
    buf_puts(h, "<img src=\"");
    img_src(h, url);
    buf_printf(h, "\" width=%d height=%d border=1 style=\"border-color:#99ccff\" alt=\"\">", w, w);
}

static void render(buf_t *h, items_t *news, items_t *albums, cJSON *chart)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    int used[MAX_ITEMS] = {0};
    html_page_start(h, "Home - Media Guide", "Media", "Guide", css);

    /* Category strip. The original had a nav column; WMP9's default window
     * leaves the Media Guide pane only ~520 px wide, so it runs across. */
    buf_puts(h, "<table width=\"100%\" cellpadding=0 cellspacing=0 border=0><tr><td class=strip>"
                "<table cellpadding=0 cellspacing=0 border=0><tr>"
                "<td class=navsel><a href=\"#top\">Home</a></td>"
                "<td class=nav><img src=\"/wmpmeta/static/sq.gif\" width=4 height=4 border=0 alt=\"\" align=absmiddle> <a href=\"#news\">Music News</a></td>"
                "<td class=nav><img src=\"/wmpmeta/static/sq.gif\" width=4 height=4 border=0 alt=\"\" align=absmiddle> <a href=\"#albums\">New Albums</a></td>"
                "</tr></table></td></tr></table>");

    /* Blue feature area: one lead story, then a row of three. */
    buf_puts(h, "<table width=\"100%\" cellpadding=0 cellspacing=0 border=0 bgcolor=#003399 "
                "background=\"/wmpmeta/static/dots.gif\"><tr><td valign=top style=\"padding:6px\">");
    for (int i = 0; i < news->n; i++) {
        item_t *it = &news->it[i];
        if (!it->image)
            continue;
        used[i] = 1;
        buf_puts(h, "<table width=\"100%\" cellpadding=3 cellspacing=0 border=0><tr>"
                    "<td valign=top width=104>");
        cover(h, it->image, 96);
        buf_puts(h, "</td><td valign=top><div class=ft1>");
        esc(h, it->title);
        buf_puts(h, "</div>");
        if (it->summary) {
            buf_puts(h, "<div class=ft2>");
            esc_trunc(h, it->summary, 160);
            buf_puts(h, "</div>");
        }
        buf_puts(h, "<div class=fl>");
        esc(h, it->source);
        buf_puts(h, "</div></td></tr></table>");
        break;
    }
    buf_puts(h, "<table width=\"100%\" cellpadding=3 cellspacing=0 border=0 "
                "style=\"border-top:1px solid #0066cc;margin-top:4px\"><tr>");
    int row = 0;
    for (int i = 0; i < news->n && row < 3; i++) {
        item_t *it = &news->it[i];
        if (used[i] || !it->image)
            continue;
        used[i] = 1;
        row++;
        buf_puts(h, "<td valign=top width=\"33%\">");
        cover(h, it->image, 60);
        buf_puts(h, "<div class=cap>");
        esc_trunc(h, it->title, 55);
        buf_puts(h, "</div><div class=fl>");
        esc(h, it->source);
        buf_puts(h, "</div></td>");
    }
    buf_puts(h, "</tr></table></td></tr></table>");

    /* White lower area: more news | Top 10 */
    buf_puts(h, "<table width=\"100%\" cellpadding=0 cellspacing=0 border=0><tr>"
                "<td valign=top><a name=news></a><div class=bar>More music news:</div>"
                "<table width=\"100%\" cellpadding=4 cellspacing=0 border=0>");
    int shown = 0;
    for (int i = 0; i < news->n && shown < NEWS_SHOWN; i++) {
        item_t *it = &news->it[i];
        if (used[i])
            continue;
        shown++;
        buf_puts(h, "<tr><td><span class=cat>");
        esc(h, it->source);
        buf_puts(h, ":</span> <span class=tt>");
        esc(h, it->title);
        buf_puts(h, "</span>");
        if (it->summary) {
            buf_puts(h, "<br><span class=sum>");
            esc_trunc(h, it->summary, 160);
            buf_puts(h, "</span>");
        }
        if (it->date) {
            buf_puts(h, "<br><span class=meta>");
            esc(h, it->date);
            buf_puts(h, "</span>");
        }
        buf_puts(h, "</td></tr>");
    }
    if (!news->n)
        buf_puts(h, "<tr><td>No news right now. Check that this PC can reach the internet.</td></tr>");
    buf_puts(h, "</table></td>");

    buf_puts(h, "<td width=220 valign=top bgcolor=#e7e7e7 style=\"border-left:1px solid #003399\">"
                "<a name=albums></a><div class=bar>Top 10 Lists:</div>"
                "<div class=tabs><span class=tab>New Album Reviews</span></div>"
                "<table width=\"100%\" cellpadding=3 cellspacing=0 border=0>");
    for (int i = 0; i < albums->n && i < 10; i++) {
        item_t *it = &albums->it[i];
        buf_printf(h, "<tr><td valign=top class=num>%d</td><td valign=top width=40>", i + 1);
        if (it->image) {
            buf_puts(h, "<img src=\"");
            img_src(h, it->image);
            buf_puts(h, "\" width=36 height=36 border=0 alt=\"\">");
        }
        buf_puts(h, "</td><td valign=top><span class=tt><b>");
        esc(h, it->title);
        buf_puts(h, "</b></span>");
        if (it->summary) {
            buf_puts(h, "<br><span class=sum>");
            esc_trunc(h, it->summary, 60);
            buf_puts(h, "</span>");
        }
        buf_puts(h, "</td></tr>");
    }
    if (!albums->n)
        buf_puts(h, "<tr><td>No album reviews right now.</td></tr>");
    buf_puts(h, "</table>");

    const cJSON *arts = chart ? cJSON_GetObjectItemCaseSensitive(
                                    cJSON_GetObjectItemCaseSensitive(chart, "payload"), "artists")
                              : NULL;
    if (cJSON_GetArraySize(arts) > 0) {
        buf_puts(h, "<div class=tabs><span class=tab>Most Played This Week</span></div>"
                    "<table width=\"100%\" cellpadding=3 cellspacing=0 border=0>");
        const cJSON *a, *b;
        int k = 0;
        cJSON_ArrayForEach(a, arts)
        {
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(a, "artist_name");
            int dup = 0;  /* same name under two artist IDs: show it once */
            cJSON_ArrayForEach(b, arts)
            {
                if (b == a)
                    break;
                const cJSON *bn = cJSON_GetObjectItemCaseSensitive(b, "artist_name");
                if (cJSON_IsString(nm) && cJSON_IsString(bn) && !strcmp(nm->valuestring, bn->valuestring))
                    dup = 1;
            }
            if (dup)
                continue;
            buf_printf(h, "<tr><td class=num>%d</td><td class=tt>", ++k);
            esc(h, cJSON_IsString(nm) ? nm->valuestring : "");
            buf_puts(h, "</td></tr>");
        }
        buf_puts(h, "</table>");
    }
    buf_puts(h, "</td></tr></table>");
    buf_puts(h, "<div class=foot>News from ");
    for (int i = 0; news_feeds[i]; i++) {
        char *n = site_name(news_feeds[i]);
        buf_puts(h, i ? (news_feeds[i + 1] ? ", " : " and ") : "");
        esc(h, n);
        free(n);
    }
    buf_printf(h, ". Album info from MusicBrainz and the Cover Art Archive%s. "
                  "Media Guide by wmpmeta %s, updated %02d:%02d.</div></body></html>",
               show_charts ? ", charts from ListenBrainz" : "", WM_VERSION, st.wHour, st.wMinute);
}

/* Reads every feed and takes items from each in turn, newest first within a
 * feed, so a site that posts often doesn't push the others off the page. */
static void read_all(char **feeds, items_t *out)
{
    items_t *per[MAX_FEEDS];
    int nf = 0, pos[MAX_FEEDS] = {0};
    for (; feeds[nf]; nf++) {
        per[nf] = xmalloc(sizeof(items_t));
        per[nf]->n = 0;
        read_feed(feeds[nf], per[nf]);
        qsort(per[nf]->it, (size_t)per[nf]->n, sizeof(item_t), by_date);
    }
    for (int more = 1; more && out->n < MAX_ITEMS;) {
        more = 0;
        for (int f = 0; f < nf && out->n < MAX_ITEMS; f++)
            if (pos[f] < per[f]->n) {
                out->it[out->n++] = per[f]->it[pos[f]];
                memset(&per[f]->it[pos[f]++], 0, sizeof(item_t));
                more = 1;
            }
    }
    for (int f = 0; f < nf; f++) {
        for (int i = 0; i < per[f]->n; i++)
            free_item(&per[f]->it[i]);
        free(per[f]);
    }
}

static char *build(void)
{
    static items_t news, albums;  /* only touched under guide_lock */
    for (int i = 0; i < news.n; i++)
        free_item(&news.it[i]);
    for (int i = 0; i < albums.n; i++)
        free_item(&albums.it[i]);
    news.n = albums.n = 0;

    read_all(news_feeds, &news);
    read_all(album_feeds, &albums);

    cJSON *chart = NULL;
    if (show_charts) {
        fetch_t r;
        char err[300];
        if (!fetch_url("https://api.listenbrainz.org/1/stats/sitewide/artists?range=week&count=10",
                       &r, err, sizeof err)) {
            if (r.status == 200)
                chart = cJSON_Parse(buf_cstr(&r.body));
            fetch_free(&r);
        } else {
            log_msg("guide: listenbrainz: %s", err);
        }
    }
    buf_t h = {0};
    render(&h, &news, &albums, chart);
    cJSON_Delete(chart);
    return buf_cstr(&h);
}

char *guide_page(void)
{
    EnterCriticalSection(&guide_lock);
    if (!cached || GetTickCount() - cached_at > CACHE_MS) {
        char *page = build();
        free(cached);
        cached = page;
        cached_at = GetTickCount();
    }
    char *r = xstrdup(cached);
    LeaveCriticalSection(&guide_lock);
    return r;
}

/* /wmpmeta/img?u=URL: an https image served over plain HTTP. */
int guide_image(const char *url, fetch_t *r)
{
    char err[300];
    if (_strnicmp(url, "https://", 8) && _strnicmp(url, "http://", 7))
        return -1;
    if (fetch_url(url, r, err, sizeof err)) {
        log_msg("img: %s: %s", url, err);
        return -1;
    }
    if (r->status != 200 || !r->ctype || _strnicmp(r->ctype, "image/", 6)) {
        log_msg("img: %d %s for %s", r->status, r->ctype ? r->ctype : "-", url);
        fetch_free(r);
        return -1;
    }
    return 0;
}

/* /wmpmeta/static/<name>: the page's generated GIFs. */
int guide_static(const char *name, const unsigned char **data, size_t *len)
{
    if (!strcmp(name, "dots.gif")) {
        *data = gif_dots;
        *len = sizeof gif_dots;
    } else if (!strcmp(name, "hdr.gif")) {
        *data = gif_hdr;
        *len = sizeof gif_hdr;
    } else if (!strcmp(name, "play.gif")) {
        *data = gif_play;
        *len = sizeof gif_play;
    } else if (!strcmp(name, "sq.gif")) {
        *data = gif_sq;
        *len = sizeof gif_sq;
    } else {
        return -1;
    }
    return 0;
}
