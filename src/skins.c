/* The "More Skins" and "More Visualizations" galleries.
 *
 * WMP's More Skins button opens http://go.microsoft.com/fwlink/?LinkId=9705,
 * More Visualizations LinkId=9706 and More Plug-ins LinkId=7262; they went
 * to Microsoft's Windows Media Player 9 Series galleries. With
 * go.microsoft.com pointed here, wmpmeta serves its own gallery pages listing
 * the 75 skins and 14 visualizations those galleries offered (skins_data.h,
 * from the Internet Archive's copies of the gallery pages).
 *
 * No skin or visualization ships inside wmpmeta: they belong to Microsoft and
 * the studios that made them. The first time one or its preview is asked
 * for, wmpmeta gets it from download.microsoft.com (most are still there) or
 * the Internet Archive and keeps it in a "skins" folder next to the exe, so
 * after that it comes off the local disk. "wmpmeta -getskins" fills that
 * folder in one go. */
#include "wmpmeta.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name, *file, *url, *img_ts, *img, *size, *by, *note, *desc;
    const char *section;  /* plug-ins: the gallery's heading */
} skin_t;
#include "skins_data.h"

typedef struct {
    const skin_t *list;
    int           n;
    const char   *dir, *previews;  /* under the skins folder; dir may be NULL */
    const char   *route;           /* /wmpmeta/<route>/<file>, <route>img/<n> */
} gallery_t;

static const gallery_t galleries[] = {
    {skin_list, (int)(sizeof skin_list / sizeof skin_list[0]), NULL, "previews", "skin"},
    {viz_list, (int)(sizeof viz_list / sizeof viz_list[0]), "visualizations",
     "visualizations\\previews", "viz"},
    {plug_list, (int)(sizeof plug_list / sizeof plug_list[0]), "plugins", "plugins\\previews",
     "plug"},
};

static CRITICAL_SECTION sk_lock;  /* one download at a time */

void skins_init(void)
{
    InitializeCriticalSection(&sk_lock);
}

/* skins\<sub>\<file> next to the exe; sub may have one more level. */
static void cache_path(char *out, size_t n, const char *sub, const char *file)
{
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    size_t at = s ? (size_t)(s + 1 - out) : 0;
    snprintf(out + at, n - at, "skins");
    CreateDirectoryA(out, NULL);
    if (sub) {
        size_t base = strlen(out);
        snprintf(out + base, n - base, "\\%s", sub);
        for (char *p = out + base + 1; *p; p++)  /* parents first */
            if (*p == '\\') {
                *p = 0;
                CreateDirectoryA(out, NULL);
                *p = '\\';
            }
        CreateDirectoryA(out, NULL);
    }
    snprintf(out + strlen(out), n - strlen(out), "\\%s", file);
}

static int read_file(const char *path, buf_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    char tmp[16384];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_add(out, tmp, n);
    fclose(f);
    return out->n ? 0 : -1;
}

static void write_file(const char *path, const buf_t *b)
{
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    int ok = fwrite(b->p, 1, b->n, f) == b->n;
    ok &= !fclose(f);
    remove(path);
    if (!ok || rename(tmp, path))
        remove(tmp);
}

/* GET url; 0 only for a 200 that isn't an HTML error page. */
static int get(const char *url, buf_t *out)
{
    fetch_t r;
    char err[300];
    if (fetch_url(url, &r, err, sizeof err)) {
        log_msg("skins: %s", err);
        return -1;
    }
    int ok = r.status == 200 && r.body.n > 16 && r.body.p[0] != '<' &&
             !(r.ctype && strstr(r.ctype, "html"));
    log_msg("skins: %d, %u bytes%s from %s", r.status, (unsigned)r.body.n,
            ok ? "" : " (not used)", url);
    if (ok) {
        *out = r.body;
        r.body.p = NULL;
        r.body.n = 0;
    }
    fetch_free(&r);
    return ok ? 0 : -1;
}

static const gallery_t *gal(int kind)
{
    return &galleries[kind == SK_VIZ ? 1 : kind == SK_PLUG ? 2 : 0];
}

static const skin_t *find(const gallery_t *g, const char *file)
{
    for (int i = 0; i < g->n; i++)
        if (*g->list[i].url && !_stricmp(g->list[i].file, file))
            return &g->list[i];
    return NULL;
}

/* The .wmz or installer: disk, then Microsoft, then the archive's copy.
 * (Most .wmz are zips, but not all: Need for Speed Underground's has its own
 * header, so the check is only that it isn't an HTML error page.) */
static int skin_data(const gallery_t *g, const skin_t *s, buf_t *out)
{
    char path[MAX_PATH];
    memset(out, 0, sizeof *out);
    cache_path(path, sizeof path, g->dir, s->file);
    if (!read_file(path, out))
        return 0;
    buf_free(out);
    EnterCriticalSection(&sk_lock);
    int rc = read_file(path, out);  /* another thread may have just got it */
    if (rc) {
        buf_free(out);
        char url[600];
        snprintf(url, sizeof url, "https://%s", s->url + 7);
        rc = get(url, out);
        if (rc) {
            snprintf(url, sizeof url, "https://web.archive.org/web/2006id_/%s", s->url);
            rc = get(url, out);
        }
        if (!rc)
            write_file(path, out);
    }
    LeaveCriticalSection(&sk_lock);
    return rc;
}

int skins_file(int kind, const char *file, buf_t *out)
{
    const skin_t *s = find(gal(kind), file);
    memset(out, 0, sizeof *out);
    return s ? skin_data(gal(kind), s, out) : -1;
}

static const char *img_name(const skin_t *s)
{
    const char *b = strrchr(s->img, '/');
    return b ? b + 1 : s->img;
}

int skins_image(int kind, int idx, buf_t *out, const char **ctype)
{
    const gallery_t *g = gal(kind);
    memset(out, 0, sizeof *out);
    if (idx < 0 || idx >= g->n)
        return -1;
    const skin_t *s = &g->list[idx];
    const char *name = img_name(s), *dot = strrchr(name, '.');
    *ctype = dot && !_stricmp(dot, ".gif") ? "image/gif" : "image/jpeg";
    char path[MAX_PATH], fname[200];
    snprintf(fname, sizeof fname, "%d-%s", idx, name);
    cache_path(path, sizeof path, g->previews, fname);
    if (!read_file(path, out))
        return 0;
    buf_free(out);
    char url[600];
    snprintf(url, sizeof url, "https://web.archive.org/web/%sim_/%s", s->img_ts, s->img);
    if (get(url, out))
        return -1;
    if (out->n < 4 || (memcmp(out->p, "GIF8", 4) && memcmp(out->p, "\xff\xd8", 2))) {
        buf_free(out);
        return -1;
    }
    write_file(path, out);
    return 0;
}

int skins_fetch_all(void)
{
    int got = 0, all = 0;
    for (int k = 0; k < 3; k++) {
        const gallery_t *g = gal(k);
        for (int i = 0; i < g->n; i++) {
            buf_t b;
            const char *ct;
            if (!skins_image(k, i, &b, &ct))
                buf_free(&b);
            if (!*g->list[i].url)  /* listed, but no download survives */
                continue;
            all++;
            if (!skin_data(g, &g->list[i], &b))
                got++;
            else
                log_msg("skins: couldn't get %s", g->list[i].name);
            buf_free(&b);
        }
    }
    log_msg("skins: %d of %d skins, visualizations and plug-ins in the skins folder", got, all);
    return got == all ? 0 : 1;
}

static const char css[] =
    ".pad{padding:8px 10px}"
    ".intro{font-size:11px;color:#333;padding:8px 10px 0 10px}"
    ".sn{font-size:11px;font-weight:bold;color:#000099}"
    ".sk{color:#666}"
    ".sd{color:#333}"
    ".cell{padding:6px 0;border-bottom:1px solid #e8eef8}"
    ".thumb{border:1px solid #99ccff}"
    ".sec{font-family:Trebuchet MS,Verdana,sans-serif;font-size:13px;font-weight:bold;"
    "color:#003399;padding:12px 0 2px 0;border-bottom:2px solid #0066cc}";

static void cell(buf_t *h, const gallery_t *g, int i)
{
    const skin_t *s = &g->list[i];
    int dl = *s->url != 0;
    buf_puts(h, "<td width=88 valign=top class=cell>");
    if (dl)
        buf_printf(h, "<a href=\"/wmpmeta/%s/%s\">", g->route, s->file);
    buf_printf(h, "<img class=thumb src=\"/wmpmeta/%simg/%d\" width=82 height=82 alt=\"",
               g->route, i);
    html_esc(h, s->name);
    buf_puts(h, dl ? "\"></a>" : "\">");
    buf_puts(h, "</td><td valign=top class=cell width=\"50%\">");
    if (dl)
        buf_printf(h, "<a class=sn href=\"/wmpmeta/%s/%s\">", g->route, s->file);
    else
        buf_puts(h, "<span class=sn style=\"color:#666\">");
    html_esc(h, s->name);
    buf_puts(h, dl ? "</a>" : "</span>");
    buf_puts(h, "<br><span class=sk>");
    if (*s->size) {
        buf_puts(h, "File size: ");
        html_esc(h, s->size);
        buf_puts(h, "<br>");
    }
    if (*s->by) {
        buf_puts(h, "Created by: ");
        html_esc(h, s->by);
        buf_puts(h, "<br>");
    }
    if (*s->note) {
        buf_puts(h, "Made for: ");
        html_esc(h, s->note);
        buf_puts(h, "<br>");
    }
    if (!dl)
        buf_puts(h, "<b>No longer available.</b> It was sold from the maker's Web site.<br>");
    buf_puts(h, "</span><span class=sd>");
    html_esc(h, s->desc);
    buf_puts(h, "</span></td>");
}

char *skins_page(int kind)
{
    const gallery_t *g = gal(kind);
    buf_t h = {0};
    if (kind == SK_VIZ) {
        html_page_start(&h, "Visualizations - Windows Media Player", "Visualization", "Gallery", css);
        buf_puts(&h, "<div class=bar>Visualizations for Windows Media Player</div>"
                     "<div class=intro>Add new patterns to Now Playing. Click a visualization to "
                     "download its setup program, then choose <b>Open</b> (or <b>Run</b>) and "
                     "follow the setup. The new visualizations appear in the Player's "
                     "Visualizations list. These are from Microsoft's Windows Media Player 9 "
                     "Series visualization gallery; a few were made for Windows XP and may not "
                     "install on Windows 2000.</div>");
    } else if (kind == SK_PLUG) {
        html_page_start(&h, "Plug-ins - Windows Media Player", "Plug-in", "Gallery", css);
        buf_puts(&h, "<div class=bar>Plug-ins for Windows Media Player</div>"
                     "<div class=intro>The plug-ins from Microsoft's Windows Media Player 9 "
                     "Series plug-in gallery. Microsoft's free PowerToys can still be "
                     "downloaded: click one, then choose <b>Open</b> (or <b>Run</b>). The others "
                     "were trials and paid products sold from their makers' Web sites, which "
                     "are gone, so they are listed for reference only. Most were made for "
                     "Windows XP.</div>");
    } else {
        html_page_start(&h, "Skins - Windows Media Player", "Skin", "Gallery", css);
        buf_puts(&h, "<div class=bar>Skins for Windows Media Player</div>"
                     "<div class=intro>Change the look of your Player. Click a skin to download it, "
                     "then choose <b>Open</b> and Windows Media Player installs it and switches to "
                     "skin mode. These are the skins from Microsoft's Windows Media Player 9 Series "
                     "skin gallery.</div>");
    }
    buf_puts(&h, "<div class=pad><table width=\"100%\" cellpadding=0 cellspacing=0 border=0>");
    /* Two per row; a new section heading (plug-ins) starts a new row. */
    for (int i = 0; i < g->n;) {
        const char *sec = g->list[i].section;
        if (*sec && (i == 0 || strcmp(sec, g->list[i - 1].section))) {
            buf_puts(&h, "<tr><td colspan=5 class=sec>");
            html_esc(&h, sec);
            buf_puts(&h, "</td></tr>");
        }
        buf_puts(&h, "<tr>");
        cell(&h, g, i);
        buf_puts(&h, "<td width=10></td>");
        if (i + 1 < g->n && !strcmp(g->list[i + 1].section, sec)) {
            cell(&h, g, i + 1);
            i += 2;
        } else {
            buf_puts(&h, "<td></td><td></td>");
            i++;
        }
        buf_puts(&h, "</tr>");
    }
    buf_puts(&h, "</table></div><div class=foot>List and previews from the Internet Archive's "
                 "copies of the Windows Media Player galleries. Downloads come from "
                 "download.microsoft.com or the Internet Archive the first time and are then "
                 "kept on this computer. Served by wmpmeta " WM_VERSION ".</div></body></html>");
    return buf_cstr(&h);
}
