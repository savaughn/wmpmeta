/* wmpmeta: stands in for windowsmedia.com's CD metadata service so Windows
 * Media Player 9 on Windows 2000 gets album and track names from MusicBrainz.
 * TLS, sockets and buffers are in tlsnet.h. */
#ifndef WMPMETA_H
#define WMPMETA_H

#include "tlsnet.h"

#define WM_VERSION "1.0.2"
#define WM_URL     "https://github.com/savaughn/wmpmeta"

/* ---- fetch.c: HTTPS GET with redirects ---- */
typedef struct {
    int    status;
    char  *ctype;   /* Content-Type, malloc'd, may be NULL */
    buf_t  body;
} fetch_t;

/* Fetches an http:// or https:// URL, following up to 5 redirects.
 * Returns 0 and fills *r (caller frees with fetch_free) or -1 with err. */
int  fetch_url(const char *url, fetch_t *r, char *err, size_t errlen);
void fetch_free(fetch_t *r);
char *fetch_location(const char *url, int *status, char *err, size_t errlen);
/* Streaming GET: the open connection, positioned at the body (see fetch.c). */
upstream_t *fetch_open(const char *url, int *status, char **ctype, buf_t *rest,
                       char *err, size_t errlen);

/* ---- mb.c: TOC -> MusicBrainz -> MDR-CD XML ---- */
/* toc is WMP's form: "E+96+44E0+...+19BAB" (track count, start offsets
 * including the 150-frame pregap, lead-out; all hex). Separators may be
 * '+', ' ' or "%2B". Returns malloc'd XML, or NULL when nothing matched. */
char *mb_lookup_toc(const char *toc);
/* Front cover (250 px JPEG) of a release from the Cover Art Archive. */
int   mb_cover(const char *mbid, fetch_t *r);
void  mb_init(void);
/* For the album pages. cJSON trees are the caller's to cJSON_Delete. */
struct cJSON;
struct cJSON *mb_json(const char *path);       /* rate-limited GET of /ws/2/... */
struct cJSON *mb_release(const char *mbid, const char *toc, int *medno);
struct cJSON *mb_disc_releases(const char *toc);
int   mb_current_mbid(const char *toc, char out[37]);
int   mb_parse_mbid(const char *s, char out[37]);   /* "{GUID}" or MBID */
int   mb_toc_lengths(const char *toc, unsigned *len_ms);  /* track count */
/* Saves mbid as this CD's album and returns its MDR-CD XML (or NULL). */
char *mb_use_album(const char *toc, const char *mbid);

/* ---- album.c: Copy from CD "View/Find Album Info" pages ---- */
char *album_view(const char *toc, const char *wmid, const char *use);  /* use: chosen MBID */
char *album_find(const char *toc, const char *wmid, const char *artist, const char *title);

/* ---- guide.c: Media Guide page ---- */
void  guide_init(void);
char *guide_page(void);                          /* malloc'd HTML */
int   guide_image(const char *url, fetch_t *r);  /* 0 and an image, or -1 */
int   guide_static(const char *name, const unsigned char **data, size_t *len);
/* Shared by the generated pages. */
void  html_esc(buf_t *b, const char *s);
int   w2k_fold(const char *s, const char **rep);  /* chars Win2k fonts lack */
void  html_page_start(buf_t *h, const char *title, const char *w1, const char *w2,
                      const char *extra_css);

/* ---- radio.c: Radio Tuner page and stream relay ---- */
void  radio_init(void);
char *radio_page(const char *tag, const char *query, const char *sort);  /* malloc'd HTML */
char *radio_asx(const char *id);                        /* malloc'd ASX or NULL */
void  radio_stream(SOCKET client, const char *id);      /* relays until either side closes */

/* ---- wayback.c: Premium Services tab (archived 2003 page) ---- */
void  wayback_init(void);
char *wayback_services(void);  /* malloc'd HTML, or NULL if not available */

/* ---- skins.c: More Skins / Visualizations / Plug-ins galleries ---- */
enum { SK_SKINS, SK_VIZ, SK_PLUG };
void  skins_init(void);
char *skins_page(int kind);                                /* malloc'd HTML */
int   skins_file(int kind, const char *file, buf_t *out);  /* from disk or fetched */
int   skins_image(int kind, int idx, buf_t *out, const char **ctype);
int   skins_fetch_all(void);                               /* wmpmeta -getskins */

#endif
