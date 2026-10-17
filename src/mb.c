/* WMP TOC -> MusicBrainz release -> WMP9 MDR-CD XML.
 *
 * WMP sends "N+o1+o2+...+oN+leadout" in hex, offsets in CD frames with the
 * 150-frame pregap included, the same numbers MusicBrainz uses. The lookup
 * order is: exact disc ID, then the disc ID with the last track treated as an
 * Enhanced-CD data track (MusicBrainz leaves data tracks out and ends the
 * audio session 11400 frames before them), then MusicBrainz's fuzzy TOC match
 * for both. MusicBrainz allows one request per second, so calls are spaced. */
#include "wmpmeta.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

#define MB_HOST "https://musicbrainz.org"
#define MB_INC  "artist-credits+recordings+labels+release-groups+genres"

static CRITICAL_SECTION mb_lock;   /* rate limit and cache */
static DWORD            last_call;

typedef struct cache {
    char         *toc;
    char         *xml;  /* NULL caches a miss */
    struct cache *next;
} cache_t;
static cache_t *cache;

void mb_init(void)
{
    InitializeCriticalSection(&mb_lock);
}

typedef struct {
    int      n;                /* audio tracks */
    unsigned off[100];         /* off[1..n] */
    unsigned leadout;
} toc_t;

static int parse_toc(const char *s, toc_t *t, unsigned *all, int *nall)
{
    int k = 0;
    while (*s && k < 102) {
        if (*s == '%' && s[1] == '2' && (s[2] == 'B' || s[2] == 'b')) {
            s += 3;
            continue;
        }
        if (!isxdigit((unsigned char)*s)) {
            s++;
            continue;
        }
        char *e;
        all[k++] = (unsigned)strtoul(s, &e, 16);
        s = e;
    }
    /* count, n offsets, lead-out */
    if (k < 3 || all[0] < 1 || all[0] > 99 || (int)all[0] + 2 != k)
        return -1;
    t->n = (int)all[0];
    for (int i = 1; i <= t->n; i++)
        t->off[i] = all[i];
    t->leadout = all[k - 1];
    *nall = k;
    return 0;
}

static void disc_id(const toc_t *t, char out[32])
{
    char s[8 + 100 * 8 + 1];
    int p = sprintf(s, "%02X%02X%08X", 1, t->n, t->leadout);
    for (int i = 1; i < 100; i++)
        p += sprintf(s + p, "%08X", i <= t->n ? t->off[i] : 0);
    unsigned char h[20];
    mbedtls_sha1((const unsigned char *)s, (size_t)p, h);
    size_t olen;
    mbedtls_base64_encode((unsigned char *)out, 32, &olen, h, sizeof h);
    for (size_t i = 0; i < olen; i++)
        out[i] = out[i] == '+' ? '.' : out[i] == '/' ? '_' : out[i] == '=' ? '-' : out[i];
}

static void toc_param(const toc_t *t, buf_t *b)
{
    buf_printf(b, "1+%d+%u", t->n, t->leadout);
    for (int i = 1; i <= t->n; i++)
        buf_printf(b, "+%u", t->off[i]);
}

/* One rate-limited MusicBrainz call. Returns parsed JSON or NULL (404 too). */
static cJSON *mb_get(const char *path)
{
    EnterCriticalSection(&mb_lock);
    DWORD since = GetTickCount() - last_call;
    if (last_call && since < 1100)
        Sleep(1100 - since);
    fetch_t r;
    char err[300];
    char *url = xmalloc(strlen(MB_HOST) + strlen(path) + 1);
    strcpy(url, MB_HOST);
    strcat(url, path);
    int rc = fetch_url(url, &r, err, sizeof err);
    /* 503 is MusicBrainz's rate limit (shared by everything behind this
     * public IP): back off and try again. */
    for (int tries = 0; !rc && r.status == 503 && tries < 3; tries++) {
        log_msg("musicbrainz: 503, retrying");
        fetch_free(&r);
        Sleep(2000u << tries);
        rc = fetch_url(url, &r, err, sizeof err);
    }
    last_call = GetTickCount();
    LeaveCriticalSection(&mb_lock);
    if (rc) {
        log_msg("musicbrainz: %s", err);
        free(url);
        return NULL;
    }
    log_msg("musicbrainz: %d %s", r.status, url);
    free(url);
    cJSON *j = NULL;
    if (r.status == 200)
        j = cJSON_Parse(buf_cstr(&r.body));
    fetch_free(&r);
    return j;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void credit(const cJSON *o, buf_t *b)
{
    const cJSON *ac = cJSON_GetObjectItemCaseSensitive(o, "artist-credit"), *c;
    cJSON_ArrayForEach(c, ac)
    {
        buf_puts(b, jstr(c, "name"));
        buf_puts(b, jstr(c, "joinphrase"));
    }
}

static void xml_esc(buf_t *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': buf_puts(b, "&amp;"); break;
        case '<': buf_puts(b, "&lt;"); break;
        case '>': buf_puts(b, "&gt;"); break;
        case '"': buf_puts(b, "&quot;"); break;
        default:
            if ((unsigned char)*s >= 0x20 || *s == '\t')
                buf_add(b, s, 1);
        }
    }
}

static void tag(buf_t *b, const char *name, const char *val)
{
    if (!val || !*val) {
        buf_printf(b, "<%s />", name);
        return;
    }
    buf_printf(b, "<%s>", name);
    xml_esc(b, val);
    buf_printf(b, "</%s>", name);
}

static void tag_buf(buf_t *b, const char *name, buf_t *v)
{
    tag(b, name, v->n ? buf_cstr(v) : "");
}

static void guid(char out[40], const char *mbid)
{
    out[0] = 0;
    if (strlen(mbid) != 36)
        return;
    out[0] = '{';
    for (int i = 0; i < 36; i++)
        out[1 + i] = (char)toupper((unsigned char)mbid[i]);
    out[37] = '}';
    out[38] = 0;
}

static int medium_has_disc(const cJSON *m, const char *id)
{
    const cJSON *d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(m, "discs"))
    if (!strcmp(jstr(d, "id"), id))
        return 1;
    return 0;
}

static int medium_tracks(const cJSON *m)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(m, "track-count");
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

/* Picks the release and medium. With a disc ID, the medium that lists it;
 * otherwise the first medium with the right track count. */
/* A fuzzy TOC hit only counts when every track length is within 3 seconds
 * of the TOC; MusicBrainz's fuzzy search alone matches unrelated discs. */
static int lengths_match(const cJSON *m, const toc_t *t)
{
    const cJSON *tr;
    int i = 0;
    cJSON_ArrayForEach(tr, cJSON_GetObjectItemCaseSensitive(m, "tracks"))
    {
        if (++i > t->n)
            return 0;
        unsigned end = i < t->n ? t->off[i + 1] : t->leadout;
        long toc_ms = (long)(end - t->off[i]) * 1000 / 75;
        const cJSON *len = cJSON_GetObjectItemCaseSensitive(tr, "length");
        if (!cJSON_IsNumber(len) || labs((long)len->valuedouble - toc_ms) > 3000)
            return 0;
    }
    return i == t->n;
}

static int pick(const cJSON *releases, const char *id, const toc_t *t,
                const cJSON **rel, const cJSON **med, int *medno)
{
    const cJSON *r, *m;
    for (int pass = 0; pass < 2; pass++) {
        cJSON_ArrayForEach(r, releases)
        {
            int i = 0;
            cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(r, "media"))
            {
                i++;
                if (pass == 0 ? (id && medium_has_disc(m, id))
                              : medium_tracks(m) == t->n && lengths_match(m, t)) {
                    *rel = r;
                    *med = m;
                    *medno = i;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static void pad_date(const char *d, char out[16])
{
    /* "1999", "1999-03" -> "1999-01-01", "1999-03-01" */
    out[0] = 0;
    if (strlen(d) < 4)
        return;
    snprintf(out, 16, "%.4s-%.2s-%.2s", d, strlen(d) >= 7 ? d + 5 : "01",
             strlen(d) >= 10 ? d + 8 : "01");
}

static char *build_xml(const cJSON *rel, const cJSON *med, int medno, int wmp_tracks)
{
    buf_t x = {0}, t = {0};
    const cJSON *media = cJSON_GetObjectItemCaseSensitive(rel, "media");
    int nmedia = cJSON_GetArraySize(media);
    const cJSON *rg = cJSON_GetObjectItemCaseSensitive(rel, "release-group");
    char g[40];

    buf_puts(&x, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                 "<METADATA xmlns:sql=\"urn:schemas-microsoft-com:xml-sql\"><MDR-CD>"
                 "<version>4.0</version>");
    guid(g, jstr(rel, "id"));
    tag(&x, "WMCollectionID", g);
    guid(g, rg ? jstr(rg, "id") : "");
    tag(&x, "WMCollectionGroupID", g);
    tag(&x, "mdqRequestID", "");
    tag(&x, "uniqueFileID", "");

    buf_puts(&t, jstr(rel, "title"));
    if (nmedia > 1) {
        const char *mt = jstr(med, "title");
        buf_printf(&t, " (Disc %d%s%s)", medno, *mt ? ": " : "", mt);
    }
    tag_buf(&x, "albumTitle", &t);
    buf_free(&t);
    credit(rel, &t);
    tag_buf(&x, "albumArtist", &t);
    buf_free(&t);

    char date[16];
    pad_date(jstr(rel, "date"), date);
    tag(&x, "releaseDate", date);

    const cJSON *li = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(rel, "label-info"), 0);
    const cJSON *lab = li ? cJSON_GetObjectItemCaseSensitive(li, "label") : NULL;
    tag(&x, "label", lab ? jstr(lab, "name") : "");

    /* Most-voted genre of the release group, else of the release. */
    const char *genre = "";
    int best = -1;
    for (int k = 0; k < 2; k++) {
        const cJSON *gs = cJSON_GetObjectItemCaseSensitive(k ? rel : rg, "genres"), *e;
        cJSON_ArrayForEach(e, gs)
        {
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(e, "count");
            int cnt = cJSON_IsNumber(c) ? c->valueint : 0;
            if (cnt > best) {
                best = cnt;
                genre = jstr(e, "name");
            }
        }
        if (*genre)
            break;
    }
    char gbuf[64];
    snprintf(gbuf, sizeof gbuf, "%s", genre);
    if (gbuf[0])
        gbuf[0] = (char)toupper((unsigned char)gbuf[0]);
    tag(&x, "genre", gbuf);
    tag(&x, "providerStyle", gbuf);
    tag(&x, "publisherRating", "");
    tag(&x, "communityRating", "");
    tag(&x, "buyParams", "");

    const cJSON *caa = cJSON_GetObjectItemCaseSensitive(rel, "cover-art-archive");
    int front = caa && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(caa, "front"));
    tag(&x, "largeCoverParams", front ? jstr(rel, "id") : "");
    tag(&x, "smallCoverParams", front ? jstr(rel, "id") : "");
    tag(&x, "moreInfoParams", "");
    tag(&x, "dataProvider", "MusicBrainz");
    tag(&x, "dataProviderParams", "");
    tag(&x, "dataProviderLogo", "");

    const cJSON *tr;
    int idx = 0;
    cJSON_ArrayForEach(tr, cJSON_GetObjectItemCaseSensitive(med, "tracks"))
    {
        if (idx >= wmp_tracks)
            break;
        char num[16];
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(tr, "position");
        snprintf(num, sizeof num, "%d", cJSON_IsNumber(pos) ? pos->valueint : idx + 1);
        buf_puts(&x, "<track>");
        buf_printf(&x, "<trackRequestID>%d</trackRequestID>", idx);
        tag(&x, "trackTitle", jstr(tr, "title"));
        tag(&x, "trackNumber", num);
        credit(tr, &t);
        if (!t.n)
            credit(rel, &t);
        tag_buf(&x, "trackPerformer", &t);
        buf_free(&t);
        tag(&x, "trackComposer", "");
        tag(&x, "trackConductor", "");
        tag(&x, "period", "");
        buf_puts(&x, "</track>");
        idx++;
    }
    buf_puts(&x, "</MDR-CD></METADATA>");
    return buf_cstr(&x);
}

/* ---- Album choices made on the Find Album Info page ----
 * wmpmeta-albums.txt next to the exe holds "TOC<tab>release MBID" lines; a
 * TOC listed there skips the search and uses that release. */
static void albums_path(char *out, size_t n)
{
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    snprintf(s ? s + 1 : out, n - (size_t)(s ? s + 1 - out : 0), "wmpmeta-albums.txt");
}

/* The TOC as uppercase hex joined by '+', so every spelling of it matches. */
static int canon_toc(const char *toc, char *out, size_t n)
{
    toc_t t;
    unsigned all[102];
    int nall, p = 0;
    if (parse_toc(toc, &t, all, &nall))
        return -1;
    out[0] = 0;
    for (int i = 0; i < nall && p < (int)n - 10; i++)
        p += snprintf(out + p, n - (size_t)p, i ? "+%X" : "%X", all[i]);
    return 0;
}

static int valid_mbid(const char *s)
{
    if (strlen(s) != 36)
        return 0;
    for (int i = 0; i < 36; i++)
        if (i == 8 || i == 13 || i == 18 || i == 23 ? s[i] != '-' : !isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

static int override_get(const char *toc, char mbid[37])
{
    char key[1024], path[MAX_PATH], line[1200];
    if (canon_toc(toc, key, sizeof key))
        return -1;
    albums_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    int found = -1;
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab++ = 0;
        tab[strcspn(tab, "\r\n")] = 0;
        if (!strcmp(line, key) && valid_mbid(tab)) {
            strcpy(mbid, tab);
            found = 0;  /* keep reading: the last line for a TOC wins */
        }
    }
    fclose(f);
    return found;
}

static void override_set(const char *toc, const char *mbid)
{
    char key[1024], path[MAX_PATH];
    if (canon_toc(toc, key, sizeof key))
        return;
    albums_path(path, sizeof path);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\t%s\n", key, mbid);
        fclose(f);
    }
}

static cJSON *release_json(const char *mbid)
{
    char path[200];
    snprintf(path, sizeof path, "/ws/2/release/%s?inc=" MB_INC "+discids&fmt=json", mbid);
    return mb_get(path);
}

/* The medium of a release that best fits the TOC: one listing its disc ID,
 * then one whose track lengths match, then one with the right track count,
 * then the first. Data tracks count both ways (Enhanced CDs). */
static const cJSON *fit_medium(const cJSON *rel, const char *tocstr, int *medno)
{
    toc_t c[2];
    unsigned all[102];
    int nall, nc = 1;
    const cJSON *media = cJSON_GetObjectItemCaseSensitive(rel, "media"), *m;
    if (parse_toc(tocstr, &c[0], all, &nall))
        nc = 0;
    else if (c[0].n >= 2 && c[0].off[c[0].n] > 11400) {
        c[1] = c[0];
        c[1].n--;
        c[1].leadout = c[0].off[c[0].n] - 11400;
        nc = 2;
    }
    for (int pass = 0; pass < 3; pass++)
        for (int k = 0; k < nc; k++) {
            char id[32];
            disc_id(&c[k], id);
            int i = 0;
            cJSON_ArrayForEach(m, media)
            {
                i++;
                if (pass == 0 ? medium_has_disc(m, id)
                    : pass == 1 ? medium_tracks(m) == c[k].n && lengths_match(m, &c[k])
                                : medium_tracks(m) == c[k].n) {
                    *medno = i;
                    return m;
                }
            }
        }
    *medno = 1;
    return cJSON_GetArrayItem(media, 0);
}

static int toc_tracks(const char *tocstr)
{
    toc_t t;
    unsigned all[102];
    int nall;
    return parse_toc(tocstr, &t, all, &nall) ? 0 : t.n;
}

static char *release_xml(const char *mbid, const char *tocstr)
{
    cJSON *j = release_json(mbid);
    if (!j)
        return NULL;
    int medno;
    const cJSON *med = fit_medium(j, tocstr, &medno);
    char *xml = med ? build_xml(j, med, medno, toc_tracks(tocstr)) : NULL;
    if (xml)
        log_msg("lookup: chosen album %s - %s (%s, disc %d)",
                jstr(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "artist-credit"), 0), "name"),
                jstr(j, "title"), mbid, medno);
    cJSON_Delete(j);
    return xml;
}

static char *lookup(const char *tocstr)
{
    char chosen[37];
    if (!override_get(tocstr, chosen)) {
        char *xml = release_xml(chosen, tocstr);
        if (xml)
            return xml;
    }

    toc_t full, cand[2];
    unsigned all[102];
    int nall, ncand = 1;
    char *fuzzy = NULL;  /* used only when no candidate matches exactly */
    if (parse_toc(tocstr, &full, all, &nall)) {
        log_msg("lookup: can't parse TOC '%s'", tocstr);
        return NULL;
    }
    cand[0] = full;
    if (full.n >= 2 && full.off[full.n] > 11400) {  /* Enhanced CD guess */
        cand[1] = full;
        cand[1].n = full.n - 1;
        cand[1].leadout = full.off[full.n] - 11400;
        ncand = 2;
    }
    for (int c = 0; c < ncand; c++) {
        char id[32];
        disc_id(&cand[c], id);
        buf_t path = {0};
        buf_printf(&path, "/ws/2/discid/%s?toc=", id);
        toc_param(&cand[c], &path);
        buf_puts(&path, "&inc=" MB_INC "&fmt=json");
        log_msg("lookup: disc id %s (%d tracks)", id, cand[c].n);
        cJSON *j = mb_get(buf_cstr(&path));
        buf_free(&path);
        if (!j)
            continue;
        /* An exact hit lists the disc ID; a fuzzy TOC hit only has releases. */
        int exact = !strcmp(jstr(j, "id"), id);
        const cJSON *rel, *med;
        int medno;
        char *xml = NULL;
        if (!pick(cJSON_GetObjectItemCaseSensitive(j, "releases"), exact ? id : NULL,
                  &cand[c], &rel, &med, &medno)) {
            log_msg("lookup: %s match: %s - %s (%s, disc %d)", exact ? "exact" : "fuzzy",
                    jstr(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(rel, "artist-credit"), 0), "name"),
                    jstr(rel, "title"), jstr(rel, "id"), medno);
            xml = build_xml(rel, med, medno, full.n);
        }
        cJSON_Delete(j);
        if (xml && exact) {
            free(fuzzy);
            return xml;
        }
        if (xml && !fuzzy)
            fuzzy = xml;
        else
            free(xml);
    }
    if (!fuzzy)
        log_msg("lookup: no match for %s", tocstr);
    return fuzzy;
}

char *mb_lookup_toc(const char *rawtoc)
{
    char toc[1024];
    if (canon_toc(rawtoc, toc, sizeof toc))
        snprintf(toc, sizeof toc, "%s", rawtoc);
    EnterCriticalSection(&mb_lock);
    for (cache_t *c = cache; c; c = c->next)
        if (!strcmp(c->toc, toc)) {
            char *r = c->xml ? xstrdup(c->xml) : NULL;
            LeaveCriticalSection(&mb_lock);
            return r;
        }
    LeaveCriticalSection(&mb_lock);
    char *xml = lookup(toc);
    EnterCriticalSection(&mb_lock);
    cache_t *c = xmalloc(sizeof *c);
    c->toc = xstrdup(toc);
    c->xml = xml ? xstrdup(xml) : NULL;
    c->next = cache;
    cache = c;
    LeaveCriticalSection(&mb_lock);
    return xml;
}

int mb_cover(const char *mbid, fetch_t *r)
{
    char url[160], err[300];
    snprintf(url, sizeof url, "https://coverartarchive.org/release/%s/front-250", mbid);
    if (fetch_url(url, r, err, sizeof err)) {
        log_msg("cover: %s", err);
        return -1;
    }
    log_msg("cover: %d, %u bytes for %s", r->status, (unsigned)r->body.n, mbid);
    return r->status == 200 ? 0 : -1;
}

/* ---- For the Copy from CD album pages (album.c) ---- */

cJSON *mb_json(const char *path)
{
    return mb_get(path);
}

char *mb_use_album(const char *rawtoc, const char *mbid)
{
    char toc[1024];
    if (!valid_mbid(mbid) || canon_toc(rawtoc, toc, sizeof toc))
        return NULL;
    char *xml = release_xml(mbid, toc);
    if (!xml)
        return NULL;
    override_set(toc, mbid);
    EnterCriticalSection(&mb_lock);
    for (cache_t *c = cache; c; c = c->next)
        if (!strcmp(c->toc, toc)) {
            free(c->xml);
            c->xml = xstrdup(xml);
        }
    LeaveCriticalSection(&mb_lock);
    return xml;
}

int mb_current_mbid(const char *toc, char out[37])
{
    if (!override_get(toc, out))
        return 0;
    char *xml = mb_lookup_toc(toc);
    const char *p = xml ? strstr(xml, "<WMCollectionID>{") : NULL;
    int rc = -1;
    if (p && strlen(p) > 17 + 36) {
        for (int i = 0; i < 36; i++)
            out[i] = (char)tolower((unsigned char)p[17 + i]);
        out[36] = 0;
        rc = valid_mbid(out) ? 0 : -1;
    }
    free(xml);
    return rc;
}

int mb_parse_mbid(const char *s, char out[37])
{
    if (!s)
        return -1;
    while (*s == '{')
        s++;
    if (strlen(s) < 36)
        return -1;
    for (int i = 0; i < 36; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[36] = 0;
    return valid_mbid(out) ? 0 : -1;
}

cJSON *mb_release(const char *mbid, const char *toc, int *medno)
{
    cJSON *j = release_json(mbid);
    if (j && !fit_medium(j, toc, medno)) {
        cJSON_Delete(j);
        j = NULL;
    }
    return j;
}

int mb_toc_lengths(const char *tocstr, unsigned *len_ms)
{
    toc_t t;
    unsigned all[102];
    int nall;
    if (parse_toc(tocstr, &t, all, &nall))
        return 0;
    for (int i = 1; i <= t.n; i++)
        len_ms[i - 1] = ((i < t.n ? t.off[i + 1] : t.leadout) - t.off[i]) * 1000u / 75u;
    return t.n;
}

/* Releases that MusicBrainz ties to this TOC, by disc ID or fuzzy TOC match,
 * plain and as an Enhanced CD. A cJSON array, never NULL. */
cJSON *mb_disc_releases(const char *tocstr)
{
    cJSON *out = cJSON_CreateArray();
    toc_t c[2];
    unsigned all[102];
    int nall, nc = 1;
    if (parse_toc(tocstr, &c[0], all, &nall))
        return out;
    if (c[0].n >= 2 && c[0].off[c[0].n] > 11400) {
        c[1] = c[0];
        c[1].n--;
        c[1].leadout = c[0].off[c[0].n] - 11400;
        nc = 2;
    }
    for (int k = 0; k < nc; k++) {
        char id[32];
        disc_id(&c[k], id);
        buf_t path = {0};
        buf_printf(&path, "/ws/2/discid/%s?toc=", id);
        toc_param(&c[k], &path);
        buf_puts(&path, "&inc=artist-credits+labels+release-groups&fmt=json");
        cJSON *j = mb_get(buf_cstr(&path));
        buf_free(&path);
        const cJSON *r, *rels = j ? cJSON_GetObjectItemCaseSensitive(j, "releases") : NULL;
        cJSON_ArrayForEach(r, rels)
        {
            const cJSON *o;
            int dup = 0;
            cJSON_ArrayForEach(o, out)
            dup |= !strcmp(jstr(o, "id"), jstr(r, "id"));
            if (!dup)
                cJSON_AddItemToArray(out, cJSON_Duplicate(r, 1));
        }
        cJSON_Delete(j);
    }
    return out;
}
