/* The Radio Tuner page and its stream relay.
 *
 * Stations come from radio-browser.info, a community-run directory with an
 * open JSON API. Only MP3 stations are listed: WMP9 plays MP3 out of the box
 * but needs extra codecs for AAC, and it cannot play HLS at all.
 *
 * Each station's Play link is an ASX playlist (WMP's own format), which
 * makes WMP play it in place. The ASX points back here, at
 * /wmpmeta/stream?id=<station uuid>. wmpmeta asks the directory for the
 * station's current URL (which also counts the click, as the directory asks
 * clients to), opens it over HTTP or HTTPS, follows redirects, and passes the
 * audio through under one plain "HTTP/1.0 200 OK". WMP never talks to a
 * station directly, so HTTPS-only stations and SHOUTcast's nonstandard
 * "ICY 200 OK" replies don't matter to it. */
#include "wmpmeta.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define LIST_MAX  40
#define CACHE_MS  (30 * 60 * 1000)

static const char *rb_hosts[] = {"de1.api.radio-browser.info", "fi1.api.radio-browser.info",
                                 "de2.api.radio-browser.info", NULL};

/* Genre menu: the directory's own tag list is mostly noise ("radio", "fm",
 * place names), so this is a fixed set of tags it has plenty of stations for. */
static const char *genres[][2] = {
    {"", "Featured"},      {"pop", "Pop"},         {"rock", "Rock"},     {"jazz", "Jazz"},
    {"classical", "Classical"}, {"electronic", "Electronic"}, {"dance", "Dance"},
    {"hip hop", "Hip Hop"}, {"country", "Country"}, {"oldies", "Oldies"}, {"60s", "60s"},
    {"70s", "70s"},        {"80s", "80s"},         {"90s", "90s"},
    {"2000s", "2000s"},
    {"ambient", "Ambient"}, {"blues", "Blues"},
    {"soul", "Soul"},      {"reggae", "Reggae"},   {"metal", "Metal"},   {"news", "News"},
    {"talk", "Talk"},      {NULL, NULL}};

typedef struct rcache {
    char          *key, *html;
    DWORD          at;
    struct rcache *next;
} rcache_t;

static CRITICAL_SECTION radio_lock;
static rcache_t        *cache;

static void load_ini(void);

void radio_init(void)
{
    InitializeCriticalSection(&radio_lock);
    load_ini();
}

static void url_enc(buf_t *b, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("-_.~", *p))
            buf_add(b, p, 1);
        else
            buf_printf(b, "%%%02X", *p);
    }
}

static int valid_id(const char *id)
{
    if (!id || strlen(id) != 36)
        return 0;
    for (int i = 0; i < 36; i++)
        if (!(i == 8 || i == 13 || i == 18 || i == 23 ? id[i] == '-' : isxdigit((unsigned char)id[i])))
            return 0;
    return 1;
}

/* GET from the first directory server that answers 200. */
static cJSON *rb_get(const char *path)
{
    for (int i = 0; rb_hosts[i]; i++) {
        buf_t url = {0};
        buf_printf(&url, "https://%s%s", rb_hosts[i], path);
        fetch_t r;
        char err[300];
        int rc = fetch_url(buf_cstr(&url), &r, err, sizeof err);
        if (rc) {
            log_msg("radio: %s: %s", url.p, err);
            buf_free(&url);
            continue;
        }
        buf_free(&url);
        cJSON *j = r.status == 200 ? cJSON_Parse(buf_cstr(&r.body)) : NULL;
        fetch_free(&r);
        if (j)
            return j;
    }
    return NULL;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int jint(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static const char css[] =
    ".frm td{color:#fff;font-weight:bold;font-size:10px;padding:2px 4px}"
    ".frm select,.frm input{font-family:Verdana,Arial,sans-serif;font-size:10px}"
    ".row0{background:#fff}"
    ".row1{background:#e7e7e7}"
    ".play{background:#003399;color:#fff;font-weight:bold;font-size:10px;padding:1px 5px;"
    "border:1px solid #99ccff;text-decoration:none}"
    ".sn{font-weight:bold;color:#000;font-size:11px}"
    ".tags{color:#666;font-size:10px}"
    ".br{color:#0066cc;font-size:10px;white-space:nowrap}"
    ".srt{color:#99ccff;text-decoration:underline}"
    ".cty{background:#0066cc;color:#fff;font-weight:bold;font-size:10px;padding:2px 4px}";

/* Country codes to leave out of every list, from wmpmeta.ini:
 *   radio_hide_countries=IN,XX */
static char hide_countries[256];

static void load_ini(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *s = strrchr(path, '\\');
    snprintf(s ? s + 1 : path, sizeof path - (size_t)(s ? s + 1 - path : 0), "wmpmeta.ini");
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p))
            p++;
        if (!_strnicmp(p, "radio_hide_countries=", 21)) {
            /* Keep it as ",IN,XX," for whole-code matching. */
            buf_t b = {0};
            buf_puts(&b, ",");
            for (char *q = p + 21; *q && *q != '\r' && *q != '\n'; q++)
                if (*q != ' ' && *q != '\t')
                    buf_add(&b, q, 1);
            buf_puts(&b, ",");
            snprintf(hide_countries, sizeof hide_countries, "%s", buf_cstr(&b));
            buf_free(&b);
            log_msg("radio: hiding countries %s", hide_countries);
        }
    }
    fclose(f);
}

static int hidden_country(const char *cc)
{
    char k[8];
    if (!*cc || !hide_countries[0])
        return 0;
    snprintf(k, sizeof k, ",%.2s,", cc);
    for (char *p = k; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    char up[256];
    snprintf(up, sizeof up, "%s", hide_countries);
    for (char *p = up; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    return strstr(up, k) != NULL;
}

/* The directory uses formal ISO names ("The United Kingdom Of Great
 * Britain And Northern Ireland"). Show and sort by the everyday name. */
static const char *country_name(const char *c)
{
    static const char *fix[][2] = {
        {"The United States Of America", "United States"},
        {"The United Kingdom Of Great Britain And Northern Ireland", "United Kingdom"},
        {"The Russian Federation", "Russia"},
        {"Republic Of Korea", "South Korea"},
        {"The Republic Of Korea", "South Korea"},
        {"Iran (Islamic Republic Of)", "Iran"},
        {"Viet Nam", "Vietnam"},
        {"The Netherlands", "Netherlands"},
        {"Czechia", "Czech Republic"},
        {NULL, NULL}};
    for (int i = 0; fix[i][0]; i++)
        if (!_stricmp(c, fix[i][0]))
            return fix[i][1];
    if (!_strnicmp(c, "The ", 4))
        return c + 4;
    return c;
}

typedef struct {
    const cJSON *st;
    int          rank;  /* popularity order, keeps sorts stable */
} entry_t;

static int by_country(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    const char *cx = country_name(jstr(x->st, "country"));
    const char *cy = country_name(jstr(y->st, "country"));
    if (!*cx != !*cy)
        return *cx ? -1 : 1;  /* stations without a country go last */
    int c = _stricmp(cx, cy);
    return c ? c : x->rank - y->rank;
}

static void station_row(buf_t *h, const cJSON *st, int stripe, int with_country)
{
    const char *id = jstr(st, "stationuuid");
    buf_printf(h, "<tr class=row%d><td width=50 valign=top>"
                  "<a class=play href=\"/wmpmeta/play.asx?id=%s\"><img src=\"/wmpmeta/static/play.gif\" "
                  "width=5 height=9 border=0 alt=\"\">&nbsp;Play</a></td>"
                  "<td valign=top><span class=sn>", stripe, id);
    /* Some stations stuff their names with keywords; keep 50 characters. */
    const char *nm = jstr(st, "name");
    int chars = 0;
    size_t cut = 0;
    for (const char *p = nm; *p; p++)
        if (((unsigned char)*p & 0xC0) != 0x80 && ++chars > 50) {
            cut = (size_t)(p - nm);
            break;
        }
    if (cut) {
        char *t = xstrndup(nm, cut);
        html_esc(h, t);
        free(t);
        buf_puts(h, "&#8230;");
    } else {
        html_esc(h, nm);
    }
    buf_puts(h, "</span><br><span class=tags>");
    /* First three tags, then the country unless a heading already says it. */
    char *tags = xstrdup(jstr(st, "tags"));
    int nt = 0;
    for (char *t = strtok(tags, ","); t && nt < 3; t = strtok(NULL, ","), nt++) {
        if (nt)
            buf_puts(h, ", ");
        html_esc(h, t);
    }
    free(tags);
    const char *country = country_name(jstr(st, "country"));
    if (with_country && *country) {
        buf_puts(h, nt ? " &middot; " : "");
        html_esc(h, country);
    }
    buf_puts(h, "</span></td><td width=40 align=right valign=top class=br>");
    if (jint(st, "bitrate") > 0)
        buf_printf(h, "%dK", jint(st, "bitrate"));
    buf_puts(h, "</td></tr>");
}

static void hidden_field(buf_t *h, const char *name, const char *value)
{
    if (!value || !*value)
        return;
    buf_printf(h, "<input type=hidden name=%s value=\"", name);
    html_esc(h, value);
    buf_puts(h, "\">");
}

static char *build(const char *tag, const char *query, int by_cty)
{
    buf_t path = {0};
    buf_puts(&path, "/json/stations/search?codec=MP3&order=clickcount&reverse=true"
                    "&hidebroken=true&limit=150");
    const char *label = "Featured";
    if (query && *query) {
        buf_puts(&path, "&name=");
        url_enc(&path, query);
    } else if (tag && *tag) {
        buf_puts(&path, "&tagExact=true&tag=");
        url_enc(&path, tag);
        for (int i = 0; genres[i][0]; i++)
            if (!_stricmp(genres[i][0], tag))
                label = genres[i][1];
    }
    cJSON *list = rb_get(buf_cstr(&path));
    buf_free(&path);

    /* The 40 most popular playable stations, then sorted if asked. */
    entry_t ent[LIST_MAX];
    int n = 0;
    const cJSON *st;
    cJSON_ArrayForEach(st, list)
    {
        if (n >= LIST_MAX)
            break;
        /* The search already asks for MP3; also skip HLS and failed checks. */
        if (jint(st, "hls") || !jint(st, "lastcheckok") || _stricmp(jstr(st, "codec"), "MP3"))
            continue;
        if (!valid_id(jstr(st, "stationuuid")) || hidden_country(jstr(st, "countrycode")))
            continue;
        ent[n].st = st;
        ent[n].rank = n;
        n++;
    }
    if (by_cty)
        qsort(ent, (size_t)n, sizeof *ent, by_country);

    buf_t h = {0};
    html_page_start(&h, "Radio Tuner", "Radio", "Tuner", css);

    /* Find More Stations: genre and sort menus, name search. Plain GET forms;
     * each carries the other's settings in hidden fields. */
    buf_puts(&h, "<table width=\"100%\" cellpadding=0 cellspacing=0 border=0 bgcolor=#003399 "
                 "background=\"/wmpmeta/static/dots.gif\"><tr><td style=\"padding:4px\">"
                 "<table class=frm cellpadding=0 cellspacing=0 border=0>"
                 "<tr><form action=\"/redir/radiotuner.asp\" method=get>"
                 "<td>Genre:</td><td><select name=tag>");
    for (int i = 0; genres[i][0]; i++) {
        int sel = (!query || !*query) && tag && !_stricmp(tag, genres[i][0]);
        if (!tag && !genres[i][0][0])
            sel = 1;
        buf_puts(&h, "<option value=\"");
        html_esc(&h, genres[i][0]);
        buf_printf(&h, "\"%s>", sel ? " selected" : "");
        html_esc(&h, genres[i][1]);
        buf_puts(&h, "</option>");
    }
    buf_puts(&h, "</select>");
    if (by_cty)
        hidden_field(&h, "sort", "country");
    buf_puts(&h, "</td><td><input type=submit value=\"Go\"></td></form></tr>"
                 "<tr><form action=\"/redir/radiotuner.asp\" method=get>"
                 "<td>Search:</td><td><input type=text name=q size=18 value=\"");
    html_esc(&h, query ? query : "");
    buf_puts(&h, "\">");
    if (by_cty)
        hidden_field(&h, "sort", "country");
    buf_puts(&h, "</td><td><input type=submit value=\"Go\"></td></form></tr>"
                 "<tr><td>Sort:</td><td colspan=2>");
    /* Two links that keep the current genre or search. */
    for (int k = 0; k < 2; k++) {
        if (k)
            buf_puts(&h, " | ");
        if (k == by_cty) {
            buf_puts(&h, k ? "<b>Country</b>" : "<b>Most popular</b>");
            continue;
        }
        buf_puts(&h, "<a class=srt href=\"/redir/radiotuner.asp?");
        if (query && *query) {
            buf_puts(&h, "q=");
            url_enc(&h, query);
        } else {
            buf_puts(&h, "tag=");
            url_enc(&h, tag ? tag : "");
        }
        buf_puts(&h, k ? "&amp;sort=country\">Country</a>" : "\">Most popular</a>");
    }
    buf_puts(&h, "</td></tr></table></td></tr></table>");

    buf_puts(&h, "<div class=bar>");
    if (query && *query) {
        buf_puts(&h, "Stations matching &#8220;");
        html_esc(&h, query);
        buf_puts(&h, "&#8221;");
    } else {
        html_esc(&h, label);
        buf_puts(&h, " Stations");
    }
    if (by_cty)
        buf_puts(&h, " by Country");
    buf_puts(&h, "</div><table width=\"100%\" cellpadding=4 cellspacing=0 border=0>");

    const char *last = NULL;
    int stripe = 0;
    for (int i = 0; i < n; i++) {
        const char *cty = country_name(jstr(ent[i].st, "country"));
        if (by_cty && (!last || _stricmp(last, cty))) {
            buf_puts(&h, "<tr><td colspan=3 class=cty>");
            html_esc(&h, *cty ? cty : "Other");
            buf_puts(&h, "</td></tr>");
            last = cty;
            stripe = 0;
        }
        station_row(&h, ent[i].st, stripe++ % 2, !by_cty);
    }
    if (!n)
        buf_puts(&h, list ? "<tr><td>No stations found. Try another genre or search.</td></tr>"
                          : "<tr><td>The station directory didn't answer. Check that this PC "
                            "can reach the internet, then try again.</td></tr>");
    buf_puts(&h, "</table><div class=foot>Stations from radio-browser.info, a community-run "
                 "directory. Only MP3 stations are listed, because Windows Media Player 9 "
                 "plays MP3 without extra codecs. Streams play through wmpmeta.</div>"
                 "</body></html>");
    cJSON_Delete(list);
    return buf_cstr(&h);
}

char *radio_page(const char *tag, const char *query, const char *sort)
{
    int by_cty = sort && !_stricmp(sort, "country");
    buf_t key = {0};
    buf_printf(&key, "%s|%s|%d", tag ? tag : "", query ? query : "", by_cty);
    EnterCriticalSection(&radio_lock);
    rcache_t *c;
    for (c = cache; c; c = c->next)
        if (!strcmp(c->key, key.p))
            break;
    if (!c || GetTickCount() - c->at > CACHE_MS) {
        char *html = build(tag, query, by_cty);
        if (!c) {
            c = xmalloc(sizeof *c);
            c->key = xstrdup(key.p);
            c->html = NULL;
            c->next = cache;
            cache = c;
        }
        free(c->html);
        c->html = html;
        c->at = GetTickCount();
    }
    char *r = xstrdup(c->html);
    LeaveCriticalSection(&radio_lock);
    buf_free(&key);
    return r;
}

/* ASX text. WMP9 reads an ASX as ANSI text, not XML: UTF-8 shows up as
 * mojibake and entities like &amp; are shown as typed. So convert to the
 * system code page and only swap out the characters that would break the
 * markup. */
static void asx_text(buf_t *b, const char *s)
{
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    char *a = NULL;
    if (wn > 0) {
        WCHAR *w = xmalloc((size_t)wn * sizeof *w);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
        int an = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
        if (an > 0) {
            a = xmalloc((size_t)an);
            WideCharToMultiByte(CP_ACP, 0, w, -1, a, an, NULL, NULL);
        }
        free(w);
    }
    for (const char *p = a ? a : s; *p; p++)
        buf_add(b, *p == '<' ? "(" : *p == '>' ? ")" : *p == '"' ? "'" : p, 1);
    free(a);
}

char *radio_asx(const char *id)
{
    if (!valid_id(id))
        return NULL;
    buf_t path = {0};
    buf_printf(&path, "/json/stations/byuuid/%s", id);
    cJSON *j = rb_get(buf_cstr(&path));
    buf_free(&path);
    const cJSON *st = cJSON_GetArrayItem(j, 0);
    buf_t a = {0};
    buf_puts(&a, "<ASX VERSION=\"3.0\"><TITLE>");
    asx_text(&a, st ? jstr(st, "name") : "Radio");
    buf_puts(&a, "</TITLE><ENTRY><TITLE>");
    asx_text(&a, st ? jstr(st, "name") : "Radio");
    /* No AUTHOR: WMP shows it as the artist on every song. The station's
     * genres and country go in the description instead. */
    buf_puts(&a, "</TITLE>");
    const char *tags = st ? jstr(st, "tags") : "", *cty = st ? jstr(st, "country") : "";
    if (*tags || *cty) {
        buf_puts(&a, "<ABSTRACT>");
        asx_text(&a, tags);
        buf_puts(&a, *tags && *cty ? " - " : "");
        asx_text(&a, cty);
        buf_puts(&a, "</ABSTRACT>");
    }
    buf_printf(&a, "<REF HREF=\"http://127.0.0.1/wmpmeta/stream?id=%s\"/></ENTRY></ASX>", id);
    cJSON_Delete(j);
    return buf_cstr(&a);
}

void radio_stream(SOCKET client, const char *id)
{
    static const char bad[] = "HTTP/1.0 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
    if (!valid_id(id)) {
        sock_send_all(client, bad, sizeof bad - 1);
        return;
    }
    /* /json/url/<id> returns the stream URL and counts the click. WMP opens
     * a stream two or three times before it settles (it probes the first
     * few KB), so the answer is kept for a minute: one play, one click. */
    static char  last_id[37], *last_url;
    static DWORD last_at;
    char *url = NULL;
    EnterCriticalSection(&radio_lock);
    if (last_url && !strcmp(last_id, id) && GetTickCount() - last_at < 60000)
        url = xstrdup(last_url);
    LeaveCriticalSection(&radio_lock);
    if (!url) {
        buf_t path = {0};
        buf_printf(&path, "/json/url/%s", id);
        cJSON *j = rb_get(buf_cstr(&path));
        buf_free(&path);
        url = j && *jstr(j, "url") ? xstrdup(jstr(j, "url")) : NULL;
        cJSON_Delete(j);
        if (url) {
            EnterCriticalSection(&radio_lock);
            free(last_url);
            last_url = xstrdup(url);
            strcpy(last_id, id);
            last_at = GetTickCount();
            LeaveCriticalSection(&radio_lock);
        }
    }
    if (!url) {
        log_msg("radio: no URL for station %s", id);
        sock_send_all(client, bad, sizeof bad - 1);
        return;
    }
    int status = 0;
    char *ctype, err[300];
    buf_t rest;
    upstream_t *u = fetch_open(url, &status, &ctype, &rest, err, sizeof err);
    if (!u || status != 200) {
        log_msg("radio: %s: %s", url, u ? "bad status" : err);
        if (u)
            up_close(u);
        free(ctype);
        buf_free(&rest);
        free(url);
        sock_send_all(client, bad, sizeof bad - 1);
        return;
    }
    const char *ct = ctype && !_strnicmp(ctype, "audio/", 6) ? ctype : "audio/mpeg";
    char hdr[256];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nCache-Control: no-cache\r\n"
                      "Pragma: no-cache\r\nConnection: close\r\n\r\n", ct);
    log_msg("radio: playing %s (%s)", url, ct);
    unsigned long long total = rest.n;
    int ok = !sock_send_all(client, hdr, (size_t)hl) &&
             (!rest.n || !sock_send_all(client, rest.p, rest.n));
    char tmp[16384];
    int n;
    while (ok && (n = up_read(u, tmp, sizeof tmp)) > 0) {
        if (sock_send_all(client, tmp, (size_t)n))
            break;
        total += (unsigned long long)n;
    }
    log_msg("radio: stopped %s after %llu KB", url, total / 1024);
    up_close(u);
    free(ctype);
    buf_free(&rest);
    free(url);
}
