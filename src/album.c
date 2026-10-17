/* The Copy from CD tab's "View Album Info" and "Find Album Info" pages.
 *
 * WMP opens them with the CD's TOC and the WMCollectionID it got from
 * GetMDRCD (the release MBID here):
 *   /redir/QueryTOCExt.asp?cd=<TOC>&wmid=<GUID>   View Album Info
 *   /redir/submittoc.asp?cd=<TOC>&wmid=<GUID>     Find Album Info
 * Find Album Info lists the releases MusicBrainz ties to the disc, or the
 * results of an artist/album search. "Use this album" saves the choice
 * (mb_use_album) and hands the new names to WMP through the wizard's
 * window.external.WriteNames, the way the original page did. */
#include "wmpmeta.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static const char css[] =
    ".pad{padding:8px}"
    ".at{font-family:Trebuchet MS,Verdana,sans-serif;font-size:16px;font-weight:bold;color:#000099}"
    ".aa{font-size:12px;font-weight:bold;color:#333}"
    ".k{color:#666}"
    ".th{background:#e8eef8;color:#003399;font-weight:bold;padding:2px 4px}"
    ".tr td{padding:2px 4px;border-bottom:1px solid #e8eef8}"
    ".cur{background:#ffffcc}"
    ".btn{font-family:Verdana,Arial,sans-serif;font-size:10px}"
    ".inp{font-family:Verdana,Arial,sans-serif;font-size:10px;width:130px}"
    ".msg{color:#cc0000;font-weight:bold}"
    ".lnk{color:#000099;font-weight:bold}";

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void credit(const cJSON *o, buf_t *b)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(o, "artist-credit"))
    {
        html_esc(b, jstr(c, "name"));
        html_esc(b, jstr(c, "joinphrase"));
    }
}

static void plain_credit(const cJSON *o, buf_t *b)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(o, "artist-credit"))
    {
        buf_puts(b, jstr(c, "name"));
        buf_puts(b, jstr(c, "joinphrase"));
    }
}

static void mmss(buf_t *b, unsigned ms)
{
    unsigned s = (ms + 500) / 1000;
    buf_printf(b, "%u:%02u", s / 60, s % 60);
}

/* The TOC with only hex digits and '+', as WMP sent it. */
static char *clean_toc(const char *toc)
{
    buf_t b = {0};
    for (; toc && *toc; toc++)
        if (isxdigit((unsigned char)*toc) || *toc == '+')
            buf_add(&b, toc, 1);
        else if (*toc == ' ')
            buf_add(&b, "+", 1);
    return buf_cstr(&b);
}

/* For a URL: '+' must reach us as %2B or it turns into a space. */
static void toc_q(buf_t *b, const char *toc)
{
    for (; *toc; toc++)
        if (*toc == '+')
            buf_puts(b, "%2B");
        else
            buf_add(b, toc, 1);
}

static void urlenc(buf_t *b, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.')
            buf_add(b, s, 1);
        else
            buf_printf(b, "%%%02X", c);
    }
}

static void label_of(const cJSON *rel, buf_t *b)
{
    const cJSON *li = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(rel, "label-info"), 0);
    const cJSON *lab = li ? cJSON_GetObjectItemCaseSensitive(li, "label") : NULL;
    html_esc(b, jstr(lab, "name"));
    const char *cat = jstr(li, "catalog-number");
    if (*cat) {
        buf_puts(b, lab ? " &middot; " : "");
        html_esc(b, cat);
    }
}

static void find_link(buf_t *h, const char *toc, const char *mbid, const char *text)
{
    buf_puts(h, "<a class=lnk href=\"/redir/submittoc.asp?cd=");
    toc_q(h, toc);
    buf_printf(h, "&wmid=%s\">%s</a>", mbid ? mbid : "", text);
}

/* JS string literal: XML can hold quotes, backslashes and "</script>". */
static void js_str(buf_t *b, const char *s)
{
    buf_puts(b, "'");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\'' || c == '\\' || c == '<' || c == '>' || c == '&' || c < 0x20)
            buf_printf(b, "\\x%02x", c);
        else
            buf_add(b, s, 1);
    }
    buf_puts(b, "'");
}

char *album_view(const char *rawtoc, const char *wmid, const char *use)
{
    char *toc = clean_toc(rawtoc);
    char mbid[37];
    char *chosen = use && *use ? mb_use_album(toc, use) : NULL;  /* "Use this" */
    int have = !mb_current_mbid(toc, mbid) || !mb_parse_mbid(wmid, mbid);
    int medno = 1;
    cJSON *rel = have ? mb_release(mbid, toc, &medno) : NULL;
    unsigned lens[100];
    int n = mb_toc_lengths(toc, lens);
    buf_t h = {0};

    html_page_start(&h, "Album Information", "Album", "Info", css);
    buf_puts(&h, "<div class=bar>Album Information</div><div class=pad>");
    if (use && *use && !chosen)
        buf_puts(&h, "<p class=msg>MusicBrainz didn't answer, so the album wasn't changed. "
                     "Try again in a moment.</p>");
    if (chosen) {
        /* Hand the names to WMP as the original page did; if the Player won't
         * take them, the choice is saved and used at the next lookup. */
        buf_puts(&h, "<p id=st class=msg>Saved. Remove and reinsert the CD to see the new "
                     "names in the Player.</p><script type=\"text/javascript\">var TOC=");
        js_str(&h, toc);
        buf_puts(&h, ";var X=");
        js_str(&h, chosen);
        buf_puts(&h, ";var ok=0;"
                     "try{window.external.WriteNames(TOC,X);ok=1;}catch(e){}"
                     "if(!ok){try{window.external.WriteNamesEx(0,TOC,X,false);ok=1;}catch(e){}}"
                     "if(ok){document.getElementById('st').innerHTML='Saved. The Player has the new names.';"
                     "try{window.external.ReturnToMainTask();}catch(e){}}"
                     "</script>");
        free(chosen);
    }
    if (!rel) {
        unsigned total = 0;
        for (int i = 0; i < n; i++)
            total += lens[i];
        buf_printf(&h, "<p class=aa>No album information was found for this CD.</p>"
                       "<p>The CD has %d tracks and plays for ", n);
        mmss(&h, total);
        buf_puts(&h, ".</p><p>");
        find_link(&h, toc, NULL, "Find Album Info");
        buf_puts(&h, " to search MusicBrainz by artist or album name.</p>");
    } else {
        const cJSON *media = cJSON_GetObjectItemCaseSensitive(rel, "media");
        const cJSON *med = cJSON_GetArrayItem(media, medno - 1);
        const cJSON *caa = cJSON_GetObjectItemCaseSensitive(rel, "cover-art-archive");
        int front = caa && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(caa, "front"));
        buf_puts(&h, "<table cellpadding=0 cellspacing=0 border=0><tr><td valign=top width=160>");
        if (front)
            buf_printf(&h, "<img src=\"/cover/%s\" width=150 height=150 border=1 "
                           "style=\"border-color:#99ccff\" alt=\"\">", mbid);
        else
            buf_puts(&h, "<table width=150 height=150 cellpadding=0 cellspacing=0 bgcolor=#0066cc>"
                         "<tr><td align=center style=\"color:#99ccff\">No cover</td></tr></table>");
        buf_puts(&h, "</td><td valign=top><div class=at>");
        html_esc(&h, jstr(rel, "title"));
        buf_puts(&h, "</div><div class=aa>");
        credit(rel, &h);
        buf_puts(&h, "</div><br><table cellpadding=1 cellspacing=0 border=0>");
        const char *date = jstr(rel, "date"), *cty = jstr(rel, "country");
        if (*date || *cty) {
            buf_puts(&h, "<tr><td class=k>Released:</td><td>");
            html_esc(&h, date);
            buf_puts(&h, *date && *cty ? " (" : "");
            html_esc(&h, cty);
            buf_puts(&h, *date && *cty ? ")" : "");
            buf_puts(&h, "</td></tr>");
        }
        buf_t lb = {0};
        label_of(rel, &lb);
        if (lb.n)
            buf_printf(&h, "<tr><td class=k>Label:</td><td>%s</td></tr>", buf_cstr(&lb));
        buf_free(&lb);
        if (cJSON_GetArraySize(media) > 1)
            buf_printf(&h, "<tr><td class=k>Disc:</td><td>%d of %d</td></tr>", medno,
                       cJSON_GetArraySize(media));
        const char *fmt = jstr(med, "format");
        if (*fmt) {
            buf_puts(&h, "<tr><td class=k>Format:</td><td>");
            html_esc(&h, fmt);
            buf_puts(&h, "</td></tr>");
        }
        buf_puts(&h, "</table><br>");
        find_link(&h, toc, mbid, "Wrong album? Find Album Info");
        buf_puts(&h, "</td></tr></table><br>");

        buf_t ra = {0};
        plain_credit(rel, &ra);
        const cJSON *tr;
        int i = 0, va = 0;  /* a column for track artists only when some differ */
        cJSON_ArrayForEach(tr, cJSON_GetObjectItemCaseSensitive(med, "tracks"))
        {
            buf_t ta = {0};
            plain_credit(tr, &ta);
            va |= ta.n && strcmp(buf_cstr(&ta), buf_cstr(&ra));
            buf_free(&ta);
        }
        buf_printf(&h, "<table width=\"100%%\" cellpadding=0 cellspacing=0 border=0>"
                       "<tr><td class=th width=20>#</td><td class=th>Title</td>%s"
                       "<td class=th align=right>Length</td></tr>",
                   va ? "<td class=th>Artist</td>" : "");
        cJSON_ArrayForEach(tr, cJSON_GetObjectItemCaseSensitive(med, "tracks"))
        {
            buf_t ta = {0};
            plain_credit(tr, &ta);
            const cJSON *len = cJSON_GetObjectItemCaseSensitive(tr, "length");
            buf_printf(&h, "<tr class=tr><td>%s</td><td>", jstr(tr, "number"));
            html_esc(&h, jstr(tr, "title"));
            if (va) {
                buf_puts(&h, "</td><td>");
                if (ta.n && strcmp(buf_cstr(&ta), buf_cstr(&ra)))
                    html_esc(&h, buf_cstr(&ta));
            }
            buf_puts(&h, "</td><td align=right>");
            if (cJSON_IsNumber(len))
                mmss(&h, (unsigned)len->valuedouble);
            else if (i < n)
                mmss(&h, lens[i]);
            buf_puts(&h, "</td></tr>");
            buf_free(&ta);
            i++;
        }
        buf_free(&ra);
        buf_puts(&h, "</table>");
        cJSON_Delete(rel);
    }
    buf_puts(&h, "</div><div class=foot>Album information from MusicBrainz.org, "
                 "served by wmpmeta " WM_VERSION ".</div></body></html>");
    free(toc);
    return buf_cstr(&h);
}

/* Lucene query text: letters, digits and spaces only. */
static void lucene_words(buf_t *b, const char *s)
{
    for (; *s; s++)
        buf_add(b, strchr("\"\\:+-!(){}[]^~*?/&|", *s) ? " " : s, 1);
}

static cJSON *search(const char *artist, const char *title, int ntracks)
{
    for (int pass = 0; pass < 2; pass++) {
        buf_t q = {0}, path = {0};
        if (*title) {
            buf_puts(&q, "release:(");
            lucene_words(&q, title);
            buf_puts(&q, ")");
        }
        if (*artist) {
            buf_puts(&q, q.n ? " AND artist:(" : "artist:(");
            lucene_words(&q, artist);
            buf_puts(&q, ")");
        }
        if (pass == 0 && ntracks)
            buf_printf(&q, " AND tracksmedium:%d", ntracks);
        buf_puts(&path, "/ws/2/release/?limit=25&fmt=json&query=");
        urlenc(&path, buf_cstr(&q));
        buf_free(&q);
        cJSON *j = mb_json(buf_cstr(&path));
        buf_free(&path);
        cJSON *rels = j ? cJSON_DetachItemFromObjectCaseSensitive(j, "releases") : NULL;
        cJSON_Delete(j);
        if (rels && cJSON_GetArraySize(rels))
            return rels;
        cJSON_Delete(rels);
    }
    return cJSON_CreateArray();
}

static void release_row(buf_t *h, const cJSON *r, const char *cur, int ntracks, const char *toc)
{
    const char *id = jstr(r, "id");
    char mbid[37];
    if (mb_parse_mbid(id, mbid))
        return;
    int is_cur = !strcmp(mbid, cur);
    buf_printf(h, "<tr class=\"tr%s\"><td valign=top>", is_cur ? " cur" : "");
    /* A plain form: WMP's pane won't run XMLHTTP, so the choice goes to
     * the server as a page load (album_view with use=). */
    buf_printf(h, "<form method=get action=\"/redir/QueryTOCExt.asp\" style=\"margin:0\">"
                  "<input type=hidden name=cd value=\"%s\"><input type=hidden name=wmid value=\"%s\">"
                  "<input type=hidden name=use value=\"%s\">"
                  "<input type=submit class=btn value=\"%s\"></form>",
               toc, mbid, mbid, is_cur ? "Current" : "Use this");
    buf_puts(h, "</td><td valign=top><b>");
    html_esc(h, jstr(r, "title"));
    buf_puts(h, "</b>");
    const char *dis = jstr(r, "disambiguation");
    if (*dis) {
        buf_puts(h, " <span class=k>(");
        html_esc(h, dis);
        buf_puts(h, ")</span>");
    }
    buf_puts(h, "<br>");
    credit(r, h);
    buf_puts(h, "</td><td valign=top>");
    html_esc(h, jstr(r, "date"));
    buf_puts(h, " ");
    html_esc(h, jstr(r, "country"));
    buf_puts(h, "<br><span class=k>");
    label_of(r, h);
    buf_puts(h, "</span></td><td valign=top nowrap>");
    const cJSON *m;
    int first = 1;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(r, "media"))
    {
        const cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "track-count");
        int t = cJSON_IsNumber(tc) ? tc->valueint : 0;
        const char *fmt = jstr(m, "format");
        buf_puts(h, first ? "" : "<br>");
        buf_puts(h, t == ntracks || t == ntracks - 1 ? "<b>" : "");
        html_esc(h, *fmt ? fmt : "Disc");
        buf_printf(h, ", %d tracks", t);
        buf_puts(h, t == ntracks || t == ntracks - 1 ? "</b>" : "");
        first = 0;
    }
    buf_puts(h, "</td></tr>");
}

char *album_find(const char *rawtoc, const char *wmid, const char *artist, const char *title)
{
    char *toc = clean_toc(rawtoc);
    char cur[37] = "";
    if (mb_current_mbid(toc, cur) && mb_parse_mbid(wmid, cur))
        cur[0] = 0;
    unsigned lens[100];
    int n = mb_toc_lengths(toc, lens);
    artist = artist ? artist : "";
    title = title ? title : "";
    int searching = *artist || *title;
    cJSON *rels = searching ? search(artist, title, n) : mb_disc_releases(toc);

    /* Prefill the search boxes with the current album. */
    buf_t pa = {0}, pt = {0};
    if (searching) {
        buf_puts(&pa, artist);
        buf_puts(&pt, title);
    } else {
        const cJSON *r;
        cJSON_ArrayForEach(r, rels)
        if (!pt.n && (!cur[0] || !strcmp(jstr(r, "id"), cur))) {
            plain_credit(r, &pa);
            buf_puts(&pt, jstr(r, "title"));
        }
    }

    /* Only the current album (or nothing) fits the TOC: also list other
     * editions found by name, so there is something to choose. */
    int widened = 0;
    if (!searching && cJSON_GetArraySize(rels) <= 1 && (pa.n || pt.n)) {
        cJSON *more = search(pa.n ? buf_cstr(&pa) : "", pt.n ? buf_cstr(&pt) : "", n);
        cJSON *r;
        while ((r = cJSON_DetachItemFromArray(more, 0)) != NULL) {
            const cJSON *o;
            int dup = 0;
            cJSON_ArrayForEach(o, rels)
            dup |= !strcmp(jstr(o, "id"), jstr(r, "id"));
            if (dup)
                cJSON_Delete(r);
            else
                cJSON_AddItemToArray(rels, r);
        }
        cJSON_Delete(more);
        widened = 1;
    }

    buf_t h = {0};
    html_page_start(&h, "Find Album Information", "Album", "Info", css);
    buf_puts(&h, "<div class=bar>Find Album Information</div><div class=pad>");
    buf_printf(&h, "<form method=get action=\"/redir/submittoc.asp\" style=\"margin:0\">"
                   "<input type=hidden name=cd value=\"%s\"><input type=hidden name=wmid value=\"%s\">"
                   "<table cellpadding=2 cellspacing=0 border=0><tr>"
                   "<td>Artist:</td><td><input class=inp name=artist value=\"",
               toc, cur);
    html_esc(&h, pa.n ? buf_cstr(&pa) : "");
    buf_puts(&h, "\"></td><td>Album:</td><td><input class=inp name=album value=\"");
    html_esc(&h, pt.n ? buf_cstr(&pt) : "");
    buf_puts(&h, "\"></td><td><input type=submit class=btn value=\"Search\"></td></tr></table></form>");
    buf_free(&pa);
    buf_free(&pt);

    buf_printf(&h, "<p>This CD has %d tracks. ", n);
    if (searching)
        buf_puts(&h, "These MusicBrainz releases match your search; ones with the same number "
                     "of tracks as the CD are listed first.");
    else if (widened)
        buf_puts(&h, "MusicBrainz has only the current album for this CD's exact track layout, "
                     "so other editions with the same name are listed too.");
    else
        buf_puts(&h, "These MusicBrainz releases match the CD's track layout. If yours isn't "
                     "here, search by artist and album name.");
    buf_puts(&h, " Pick one to use its names for this CD.</p>");

    if (!cJSON_GetArraySize(rels)) {
        buf_puts(&h, "<p><b>No releases found.</b> Try fewer or different words.</p>");
    } else {
        buf_puts(&h, "<table width=\"100%\" cellpadding=0 cellspacing=0 border=0>"
                     "<tr><td class=th></td><td class=th>Album</td><td class=th>Released</td>"
                     "<td class=th>Discs</td></tr>");
        /* Same track count first, then the rest. */
        for (int pass = 0; pass < 2; pass++) {
            const cJSON *r, *m;
            cJSON_ArrayForEach(r, rels)
            {
                int fits = 0;
                cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(r, "media"))
                {
                    const cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "track-count");
                    int t = cJSON_IsNumber(tc) ? tc->valueint : 0;
                    fits |= t == n || t == n - 1;
                }
                if (fits == (pass == 0))
                    release_row(&h, r, cur, n, toc);
            }
        }
        buf_puts(&h, "</table>");
    }
    if (cur[0]) {
        buf_puts(&h, "<p><a class=lnk href=\"/redir/QueryTOCExt.asp?cd=");
        toc_q(&h, toc);
        buf_printf(&h, "&wmid=%s\">Back to Album Info</a></p>", cur);
    }
    buf_puts(&h, "</div><div class=foot>Album information from MusicBrainz.org. "
                 "Missing your CD? Add it at musicbrainz.org.</div></body></html>");
    cJSON_Delete(rels);
    free(toc);
    return buf_cstr(&h);
}
