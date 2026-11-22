/* RetroSqueeze - Lyrion Music Server controller and player for Windows 98 up to Windows 11.

   Copyright (C) 2026 Sandro Miisamm.  This program is free software: you can
   redistribute it and/or modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.  See LICENSE.

   Talks to LMS over its plain-text CLI (TCP 9090): one connection polls the
   selected player's status once a second, a second one runs browse requests
   and commands.  Covers come straight from LMS's web port as small baseline
   JPEGs (LMS resizes and converts them, radio logos too); jpeg.c decodes them.

   Settings: RETROSQ.INI beside the EXE
     [retrosqueeze]
     lms=host:9090              (CLI; empty = find the server on the LAN at start)
     web=9009                   (LMS web port, for the covers)
     player=<mac of the last used player>

   Build (Open Watcom):
     make (see Makefile: multithreaded CRT -bm, slimplay.c/minimp3 with -za99)
     wrc -q -bt=nt -r retrosq.rc -fo=retrosq.res && wrc -q -bt=nt retrosq.res RETROSQ.EXE */

#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "slimplay.h"
#include "dm5x8.h"

#define W_FULL     758
#define W_CLIENT   wcl                  /* W_FULL, less nx on small screens (layout()) */
#define H_CLIENT   380
#define H_COMPACT  80                   /* folded: just the deck, as slim as the original */
#define ART        68                   /* album art, px */

#define ID_PREV    201
#define ID_PLAY    202
#define ID_STOP    203
#define ID_NEXT    204
#define ID_PAUSE   205
#define ID_REW     206
#define ID_PLAYER  207
#define ID_TAB0    210                  /* 210..216 = the seven browse tabs */
#define ID_QUERY   220
#define ID_GO      221
#define ID_BACK    222
#define ID_LIST    223
#define ID_ADD     224
#define ID_FF      225
#define ID_CLOSE   226
#define ID_FOLD    227
#define ID_MINI    228
#define ID_BANK    229
#define ID_NUM0    230                  /* 230..239 = the track number keys */

#define WM_STATUS  (WM_APP + 1)         /* status thread: new player status */
#define WM_REPLY   (WM_APP + 2)         /* worker: a browse reply is ready */
#define WM_PLAYERS (WM_APP + 3)         /* status thread: player list changed */
#define WM_PLUGINS (WM_APP + 4)         /* status thread: which server plugins exist (has_spotty/has_podcasts) */

#define T_ARTISTS 0
#define T_ALBUMS  1
#define T_SEARCH  2
#define T_QUEUE   3
#define T_RADIO   4
#define T_SPOTIFY 5
#define T_PODCAST 6
#define NTABS     7

/* ---- colours: Win98's static palette only (crisp in 256 colours) ---- */
#define C_FACE    RGB(192, 192, 192)
#define C_HI      RGB(255, 255, 255)
#define C_SH      RGB(128, 128, 128)
#define C_DK      RGB(0, 0, 0)
#define C_LCD     RGB(0, 0, 0)
#define C_CYAN    RGB(0, 255, 255)
#define C_TEAL    RGB(0, 128, 128)
#define C_TEAL2   RGB(0, 192, 192)
#define C_LED     RGB(0, 255, 0)
#define C_LEDOFF  RGB(0, 128, 0)
#define C_YEL     RGB(255, 255, 0)

static HINSTANCE inst;
static HWND wmain_, wplayer, wquery, wgo, wback, wlist, wadd, wtab[NTABS], wbtn[10], wnum[10];
static HFONT f_ui, f_lcd_big, f_lcd, f_label, f_btn, f_btnb, f_logo, f_logo2;
static HBRUSH b_list;
static int compact;                     /* browser folded away (INI compact=1) */
static int bank, bank_index = -1;       /* number keys show tracks bank*10+1 .. +10 */
static char lms_host[64] = "", ini[MAX_PATH];
static int lms_port = 9090, web_port = 9000;
static volatile int has_spotty, has_podcasts;   /* the server's plugins (status thread, CLI "can") */

unsigned char *jpeg_to_dib(const unsigned char *jpg, int len, int *w, int *h);   /* jpeg.c */

/* ------------------------------------------------------------------ text */

/* UTF-8 -> Windows-1252 (Latin-1 + the 0x80-0x9F extras), unknown -> '?' */
static void utf8_to_1252(char *s)
{
    static const unsigned short x[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
        0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
    unsigned char *i = (unsigned char *)s, *o = (unsigned char *)s;
    while (*i) {
        unsigned c = *i++;
        if (c >= 0x80) {
            int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, k;
            c &= n == 3 ? 7 : n == 2 ? 15 : 31;
            for (k = 0; k < n && (*i & 0xC0) == 0x80; k++) c = (c << 6) | (*i++ & 63);
            if (c >= 0x100 || (c >= 0x80 && c < 0xA0)) {
                for (k = 0; k < 32 && x[k] != c; k++) ;
                c = k < 32 ? 0x80 + k : c == 0x2212 ? '-' : '?';
            }
        }
        *o++ = (unsigned char)c;
    }
    *o = 0;
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            char h[3];
            h[0] = s[1]; h[1] = s[2]; h[2] = 0;
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else
            *o++ = *s;
    }
    *o = 0;
}

/* CLI argument: escape everything but plain letters/digits */
static char *esc(char *o, const char *s)
{
    for (; *s; s++) {
        unsigned char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            *o++ = c;
        else
            o += sprintf(o, "%%%02X", c);
    }
    *o = 0;
    return o;
}

/* 1252 -> UTF-8 percent-escaped (for search terms typed in the app) */
static void esc_1252(char *o, const char *s)
{
    char u[600], *p = u;
    for (; *s && p < u + sizeof u - 4; s++) {
        unsigned char c = *s;
        if (c < 0x80) *p++ = c;
        else { *p++ = (char)(0xC0 | (c >> 6)); *p++ = (char)(0x80 | (c & 63)); }
    }
    *p = 0;
    esc(o, u);
}

/* ------------------------------------------------------------- network */

typedef struct { SOCKET s; char *buf; int len, cap; } conn;

static int resolve(const char *host, struct sockaddr_in *a, int port)
{
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons((u_short)port);
    a->sin_addr.s_addr = inet_addr(host);
    if (a->sin_addr.s_addr == INADDR_NONE) {
        struct hostent *h = gethostbyname(host);
        if (!h) return 0;
        memcpy(&a->sin_addr, h->h_addr, 4);
    }
    return 1;
}

static int c_open(conn *c)
{
    struct sockaddr_in a;
    if (c->s != INVALID_SOCKET) return 1;
    if (!resolve(lms_host, &a, lms_port)) return 0;
    c->s = socket(AF_INET, SOCK_STREAM, 0);
    if (c->s == INVALID_SOCKET) return 0;
    if (connect(c->s, (struct sockaddr *)&a, sizeof a)) { closesocket(c->s); c->s = INVALID_SOCKET; return 0; }
    c->len = 0;
    return 1;
}

static void c_close(conn *c)
{
    if (c->s != INVALID_SOCKET) closesocket(c->s);
    c->s = INVALID_SOCKET;
    c->len = 0;
}

/* send one CLI line, return the reply line (in c->buf, valid until next call) */
static char *c_cmd(conn *c, const char *line)
{
    int n, tries;
    for (tries = 0; tries < 2; tries++) {
        if (!c_open(c)) return NULL;
        n = strlen(line);
        if (send(c->s, line, n, 0) != n || send(c->s, "\n", 1, 0) != 1) { c_close(c); continue; }
        for (;;) {
            char *nl = c->len ? memchr(c->buf, '\n', c->len) : NULL;
            if (nl) {
                *nl = 0;
                if (nl > c->buf && nl[-1] == '\r') nl[-1] = 0;
                /* one command = one reply line; anything after it is dropped */
                c->len = 0;
                return c->buf;
            }
            if (c->cap - c->len < 65536) {
                c->cap = c->cap ? c->cap * 2 : 262144;
                c->buf = realloc(c->buf, c->cap + 1);
            }
            n = recv(c->s, c->buf + c->len, c->cap - c->len, 0);
            if (n <= 0) { c_close(c); break; }
            c->len += n;
        }
    }
    return NULL;
}

/* split a reply into decoded tokens; returns count */
static int tokens(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        char *e = strchr(p, ' ');
        if (e) *e = 0;
        unescape(p);
        utf8_to_1252(p);
        tok[n++] = p;
        if (!e) break;
        p = e + 1;
    }
    return n;
}

static const char *val(char *t, const char *key)
{
    int k = strlen(key);
    return !strncmp(t, key, k) && t[k] == ':' ? t + k + 1 : NULL;
}

/* plain HTTP GET from the LMS web port; returns malloc'd body or NULL */
static char *http_get(const char *path, int *size)
{
    struct sockaddr_in a;
    SOCKET s;
    char req[1200], *data = NULL, *body;
    int len = 0, cap = 0, n;
    if (!resolve(lms_host, &a, web_port)) return NULL;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return NULL;
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { closesocket(s); return NULL; }
    n = sprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, lms_host);
    send(s, req, n, 0);
    for (;;) {
        if (cap - len < 16384) { cap = cap ? cap * 2 : 65536; data = realloc(data, cap + 1); }
        n = recv(s, data + len, cap - len, 0);
        if (n <= 0) break;
        len += n;
    }
    closesocket(s);
    if (!data) return NULL;
    data[len] = 0;
    body = strstr(data, "\r\n\r\n");
    if (strncmp(data, "HTTP/1.", 7) || !body || strncmp(data + 9, "200", 3)) { free(data); return NULL; }
    body += 4;
    *size = len - (body - data);
    memmove(data, body, *size);
    return data;
}

/* ------------------------------------------------------- player status */

typedef struct {
    int valid, connected, power, mode;  /* mode: 0 stop, 1 play, 2 pause */
    double time, duration;
    int volume, shuffle, repeat, index, tracks, remote, own;
    char quality[40];                   /* "FLAC 16/44.1", "MP3 320k", "SPOTIFY 320k" */
    char codec[8], rate[16];            /* the same split for the LCD: "MP3" + "320k" */
    char title[256], artist[256], album[256], year[8], art[600], player[64];
} status_t;

static CRITICAL_SECTION lock;
static status_t st;                     /* guarded by lock; UI copies it */
static char cur_player[24];             /* mac of the selected player (lock) */
static char own_name[64], own_mac[24];  /* the built-in player (slimplay.c) */
static struct { char id[24], name[64]; int connected; } players[16];
static int nplayers;
static HBITMAP cover;                   /* UI thread only */
static char cover_key[600];
static unsigned char *cover_dib;        /* decoded cover, 24-bit bottom-up (lock) */
static int cover_new, cover_w, cover_h;

static void parse_status(char *line, status_t *s)
{
    char *tok[200];
    int n = tokens(line, tok, 200), i, inloop = 0, kbps = 0, rate = 0, bits = 0;
    char type[64] = "";
    const char *v;
    memset(s, 0, sizeof *s);
    s->valid = 1;
    s->volume = -1;
    for (i = 0; i < n; i++) {
        char *t = tok[i];
        if ((v = val(t, "player_name"))) lstrcpyn(s->player, v, sizeof s->player);
        else if ((v = val(t, "player_connected"))) s->connected = atoi(v);
        else if ((v = val(t, "power"))) s->power = atoi(v);
        else if ((v = val(t, "mode"))) s->mode = !strcmp(v, "play") ? 1 : !strcmp(v, "pause") ? 2 : 0;
        else if ((v = val(t, "time"))) s->time = atof(v);
        else if ((v = val(t, "mixer volume"))) s->volume = atoi(v);
        else if ((v = val(t, "playlist shuffle"))) s->shuffle = atoi(v);
        else if ((v = val(t, "playlist repeat"))) s->repeat = atoi(v);
        else if ((v = val(t, "playlist_cur_index"))) s->index = atoi(v);
        else if ((v = val(t, "playlist_tracks"))) s->tracks = atoi(v);
        else if ((v = val(t, "remote"))) s->remote = atoi(v);
        else if ((v = val(t, "playlist index"))) inloop = 1;
        else if (inloop && (v = val(t, "title"))) lstrcpyn(s->title, v, sizeof s->title);
        else if (inloop && (v = val(t, "artist"))) lstrcpyn(s->artist, v, sizeof s->artist);
        else if (inloop && (v = val(t, "album"))) lstrcpyn(s->album, v, sizeof s->album);
        else if (inloop && (v = val(t, "year"))) lstrcpyn(s->year, v, sizeof s->year);
        else if (inloop && (v = val(t, "duration"))) s->duration = atof(v);
        else if (inloop && (v = val(t, "type"))) lstrcpyn(type, v, sizeof type);
        else if (inloop && (v = val(t, "bitrate"))) kbps = atoi(v);
        else if (inloop && (v = val(t, "samplerate"))) rate = atoi(v);
        else if (inloop && (v = val(t, "samplesize"))) bits = atoi(v);
        else if (inloop && (v = val(t, "remote_title")) && !s->album[0]) lstrcpyn(s->album, v, sizeof s->album);
        else if (inloop && (v = val(t, "coverid")) && !s->art[0]) sprintf(s->art, "/music/%.40s/cover.jpg", v);
        else if (inloop && (v = val(t, "artwork_url"))) {
            /* radio/remote: LMS-relative path, or a full URL that LMS can proxy */
            if (v[0] == '/') lstrcpyn(s->art, v, sizeof s->art);
            else if (!strncmp(v, "http", 4) && strlen(v) < 180) {
                char *o = s->art + sprintf(s->art, "/imageproxy/");
                o = esc(o, v);
                strcpy(o, "/image.png");
            }
        }
    }
    if (type[0]) {                      /* source format for the LCD */
        static const char *names[][2] = { { "flc", "FLAC" }, { "alc", "ALAC" }, { "aac", "AAC" }, { "mp4", "AAC" },
                                          { "ogg", "OGG" }, { "ops", "OPUS" }, { "wav", "WAV" }, { "aif", "AIFF" },
                                          { "wma", "WMA" }, { "mp3", "MP3" }, { "dsf", "DSD" } };
        char name[16] = "";
        int k, lossless;
        if (strstr(type, "Spotify") || !strcmp(type, "spt")) lstrcpy(name, "SPOTIFY");
        else if (strchr(type, ' ')) *strchr(type, ' ') = 0;   /* "MP3 Radio" */
        for (k = 0; !name[0] && k < (int)(sizeof names / sizeof names[0]); k++)
            if (!lstrcmpi(type, names[k][0])) lstrcpy(name, names[k][1]);
        if (!name[0]) { lstrcpyn(name, type, 6); CharUpper(name); }
        lossless = strstr("FLAC ALAC WAV AIFF DSD", name) != NULL;
        if (lossless && rate)
            sprintf(s->rate, "%d/%d.%d", bits ? bits : 16, rate / 1000, rate % 1000 / 100);
        else if (kbps)
            sprintf(s->rate, "%dk", kbps);
        sprintf(s->quality, s->rate[0] ? "%s %s" : "%s", name, s->rate);
        lstrcpyn(s->codec, !strcmp(name, "SPOTIFY") ? "SPOT" : name, 5);
    }
    if (!s->title[0] && s->remote && s->album[0]) { lstrcpyn(s->title, s->album, sizeof s->title); s->album[0] = 0; }
}

static void load_players(conn *c)
{
    char *r = c_cmd(c, "players 0 16"), *tok[400];
    int n, i, k = -1;
    if (!r) return;
    n = tokens(r, tok, 400);
    EnterCriticalSection(&lock);
    nplayers = 0;
    for (i = 0; i < n; i++) {
        const char *v;
        if ((v = val(tok[i], "playerid")) && nplayers < 16) {
            k = nplayers++;
            lstrcpyn(players[k].id, v, sizeof players[k].id);
            players[k].name[0] = 0;
            players[k].connected = 1;
        } else if (k >= 0 && (v = val(tok[i], "name"))) lstrcpyn(players[k].name, v, sizeof players[k].name);
        else if (k >= 0 && (v = val(tok[i], "connected"))) players[k].connected = atoi(v);
    }
    LeaveCriticalSection(&lock);
}

static unsigned __stdcall status_thread(void *arg)
{
    conn c = { INVALID_SOCKET, NULL, 0, 0 };
    char line[200], pid[24], art[600] = "", path[800];
    int tick = 0;
    status_t s;
    (void)arg;
    for (;; tick++) {
        if (tick % 15 == 0) {
            char *r;
            int sp = 0, pc = 0, ok;
            load_players(&c);
            PostMessage(wmain_, WM_PLAYERS, 0, 0);
            /* Spotify and Podcasts tabs only when the server has those plugins (Spotty, Podcasts) */
            if ((r = c_cmd(&c, "can spotty items ?")) != NULL) sp = !strcmp(r, "can spotty items 1");
            if ((r = c_cmd(&c, "can podcasts items ?")) != NULL) pc = !strcmp(r, "can podcasts items 1");
            ok = r != NULL;
            /* the web port (covers) straight from the server, so a wrong or old web= in the INI heals itself */
            if ((r = c_cmd(&c, "pref httpport ?")) != NULL && !strncmp(r, "pref httpport ", 14) &&
                atoi(r + 14) > 0 && atoi(r + 14) != web_port) {
                char v[12];
                web_port = atoi(r + 14);
                sprintf(v, "%d", web_port);
                WritePrivateProfileString("retrosqueeze", "web", v, ini);
            }
            if (ok && (sp != has_spotty || pc != has_podcasts || tick == 0)) {
                has_spotty = sp;
                has_podcasts = pc;
                PostMessage(wmain_, WM_PLUGINS, 0, 0);
            }
        }
        EnterCriticalSection(&lock);
        lstrcpy(pid, cur_player);
        LeaveCriticalSection(&lock);
        if (pid[0]) {
            char *r;
            sprintf(line, "%s status - 1 tags:aldyKcNorTI", pid);
            r = c_cmd(&c, line);
            if (r) parse_status(r, &s);
            else memset(&s, 0, sizeof s);
            s.own = !lstrcmpi(pid, own_mac);
            /* a new cover? fetch it here so the UI never waits */
            if (strcmp(s.art, art)) {
                unsigned char *dib = NULL;
                int size = 0, w = 0, h = 0;
                lstrcpy(art, s.art);
                if (art[0]) {
                    /* ".../cover.jpg" or ".../image.png" -> "..._120x120_o.jpg": LMS resizes
                       (keeping the aspect ratio) and always answers with a baseline JPEG */
                    char *jpg, *dot;
                    lstrcpyn(path, art, sizeof path - 20);
                    dot = strrchr(path, '.');
                    if (dot && dot > strrchr(path, '/')) *dot = 0;
                    strcat(path, "_68x68_o.jpg");
                    jpg = http_get(path, &size);
                    if (jpg) {
                        dib = jpeg_to_dib((unsigned char *)jpg, size, &w, &h);
                        free(jpg);
                        if (dib && (w > ART || h > ART)) { free(dib); dib = NULL; }
                    }
                }
                EnterCriticalSection(&lock);
                if (cover_dib) free(cover_dib);
                cover_dib = dib;
                cover_w = w;
                cover_h = h;
                cover_new = 1;
                LeaveCriticalSection(&lock);
            }
            EnterCriticalSection(&lock);
            st = s;
            LeaveCriticalSection(&lock);
            PostMessage(wmain_, WM_STATUS, 0, 0);
        }
        Sleep(1000);
    }
}

/* ------------------------------------------------------ worker (browse) */

typedef struct { char kind; char id[128]; int num; char *text; char *sub; } entry;

static entry *ents;                     /* UI thread only once handed over */
static int nents, capents;
static char *pool;                      /* text storage for ents */
static int poolsize, poolcap;

static struct { char cmd[700]; int view; } job[16];
static int jobhead, jobtail;
static HANDLE jobevent;
static char *reply;                     /* worker -> UI, handed over with WM_REPLY */
static int replyview;

static void post_job(const char *cmd, int view)
{
    EnterCriticalSection(&lock);
    if ((jobtail + 1) % 16 != jobhead) {
        lstrcpyn(job[jobtail].cmd, cmd, sizeof job[0].cmd);
        job[jobtail].view = view;
        jobtail = (jobtail + 1) % 16;
    }
    LeaveCriticalSection(&lock);
    SetEvent(jobevent);
}

static unsigned __stdcall worker_thread(void *arg)
{
    conn c = { INVALID_SOCKET, NULL, 0, 0 };
    (void)arg;
    for (;;) {
        char cmd[700];
        int view, have = 0;
        WaitForSingleObject(jobevent, INFINITE);
        for (;;) {
            char *r;
            EnterCriticalSection(&lock);
            have = jobhead != jobtail;
            if (have) {
                lstrcpy(cmd, job[jobhead].cmd);
                view = job[jobhead].view;
                jobhead = (jobhead + 1) % 16;
            }
            LeaveCriticalSection(&lock);
            if (!have) break;
            r = c_cmd(&c, cmd);
            if (view >= 0) {            /* a browse request: hand the line to the UI */
                char *copy = r ? strdup(r) : strdup("");
                EnterCriticalSection(&lock);
                if (reply) free(reply);
                reply = copy;
                replyview = view;
                LeaveCriticalSection(&lock);
                PostMessage(wmain_, WM_REPLY, 0, 0);
            }
        }
    }
}

/* ----------------------------------------------------------- browsing */

/* the current list view: what was asked, so Back can go one level up */
static int tab = T_ARTISTS;
static char view_cmd[700], back_cmd[4][700];
static int back_view[4], nback;
static int cur_view;                    /* V_* below */
static char cur_album[48];              /* album shown in V_TRACKS */
static char crumb[200];
static char opml_cmd[32];               /* CLI command of the OPML menu shown (from view_cmd) */
static char find_cmd[32], find_id[128]; /* what the search box searches on the Spotify/Radio tabs */

#define V_ARTISTS 0
#define V_ALBUMS  1
#define V_TRACKS  2
#define V_SEARCH  3
#define V_QUEUE   4
#define V_RADIO   5
#define V_OPML    6                    /* a plugin's OPML menu: Spotty, TuneIn, RadioNet ... */
#define V_RADIOS  7

static char *pool_add(const char *s)
{
    int n = strlen(s) + 1;
    char *old = pool;
    if (poolsize + n > poolcap) {
        int i;
        poolcap = (poolcap + n) * 2 + 65536;
        pool = realloc(pool, poolcap);
        for (i = 0; i < nents; i++) {   /* re-point after realloc */
            if (ents[i].text) ents[i].text = pool + (ents[i].text - old);
            if (ents[i].sub) ents[i].sub = pool + (ents[i].sub - old);
        }
    }
    memcpy(pool + poolsize, s, n);
    poolsize += n;
    return pool + poolsize - n;
}

static entry *ent_add(char kind, const char *id, const char *text)
{
    entry *e;
    if (nents == capents) { capents = capents ? capents * 2 : 512; ents = realloc(ents, capents * sizeof *ents); }
    e = &ents[nents++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    lstrcpyn(e->id, id, sizeof e->id);
    e->text = pool_add(text);
    return e;
}

static void request(const char *cmd, int view, int push)
{
    if (push && view_cmd[0]) {
        if (nback == 4) { memmove(back_cmd[0], back_cmd[1], sizeof back_cmd[0] * 3); memmove(back_view, back_view + 1, sizeof back_view[0] * 3); nback--; }
        lstrcpy(back_cmd[nback], view_cmd);
        back_view[nback++] = cur_view;
    }
    if (!push) nback = 0;
    lstrcpyn(view_cmd, cmd, sizeof view_cmd);
    cur_view = view;
    SetWindowText(wback, "< Back");
    ShowWindow(wback, nback > 0 ? SW_SHOW : SW_HIDE);
    post_job(cmd, view);
}

/* one radio provider: 'R' opens its menu, 'Y' (TuneIn search) points the search box at it */
static void radio_flush(char *cmd, char *name, char *type)
{
    if (cmd[0] && name[0])
        ent_add(!strcmp(type, "xmlbrowser_search") ? 'Y' : 'R', cmd, name);
    cmd[0] = name[0] = type[0] = 0;
}

static void fill_list(char *r, int view)
{
    char *tok[1];
    char *p = r, *t;
    entry *e = NULL;
    const char *v;
    int section = 0, rf = 0;
    char rcmd[32] = "", rname[128] = "", rtype[32] = "";
    (void)tok;
    nents = 0;
    poolsize = 0;
    if (view == V_TRACKS) ent_add('P', cur_album, ">> Play the whole album");
    if (view == V_OPML) sscanf(view_cmd, "%*s %31s", opml_cmd);
    if (view == V_RADIOS) ent_add('V', "", "My favourites");
    /* walk tokens one by one (replies can be several hundred KB) */
    while (p && *p) {
        char *sp = strchr(p, ' ');
        if (sp) *sp = 0;
        t = p;
        p = sp ? sp + 1 : NULL;
        unescape(t);
        utf8_to_1252(t);
        switch (view) {
        case V_ARTISTS:
            if ((v = val(t, "id"))) e = ent_add('A', v, "");
            else if (e && (v = val(t, "artist"))) e->text = pool_add(v);
            break;
        case V_ALBUMS:
            if ((v = val(t, "id"))) e = ent_add('L', v, "");
            else if (e && (v = val(t, "album"))) e->text = pool_add(v);
            else if (e && (v = val(t, "artist"))) e->sub = pool_add(v);
            else if (e && (v = val(t, "year")) && atoi(v) >= 1900) e->num = atoi(v);
            break;
        case V_TRACKS:
            if ((v = val(t, "id"))) { e = ent_add('T', v, ""); e->num = nents - 2; }
            else if (e && (v = val(t, "title"))) e->text = pool_add(v);
            else if (e && (v = val(t, "duration"))) {
                char d[16]; int sec = (int)atof(v);
                sprintf(d, "%d:%02d", sec / 60, sec % 60);
                e->sub = pool_add(d);
            }
            break;
        case V_SEARCH:
            if (val(t, "contributors_count")) { ent_add('H', "", "- Artists -"); section = 1; }
            else if (val(t, "albums_count")) { ent_add('H', "", "- Albums -"); section = 2; }
            else if (val(t, "tracks_count")) { ent_add('H', "", "- Tracks -"); section = 3; }
            else if (section == 1 && (v = val(t, "contributor_id"))) e = ent_add('A', v, "");
            else if (section == 1 && e && (v = val(t, "contributor"))) e->text = pool_add(v);
            else if (section == 2 && (v = val(t, "album_id"))) e = ent_add('L', v, "");
            else if (section == 2 && e && (v = val(t, "album"))) e->text = pool_add(v);
            else if (section == 3 && (v = val(t, "track_id"))) e = ent_add('S', v, "");
            else if (section == 3 && e && (v = val(t, "track"))) e->text = pool_add(v);
            break;
        case V_QUEUE:
            if ((v = val(t, "playlist index"))) { char n[12]; e = ent_add('Q', itoa(atoi(v), n, 10), ""); e->num = atoi(v); }
            else if (e && (v = val(t, "title"))) e->text = pool_add(v);
            else if (e && (v = val(t, "artist"))) e->sub = pool_add(v);
            break;
        case V_RADIO:
            if ((v = val(t, "id"))) e = ent_add('F', v, "");
            else if (e && (v = val(t, "name"))) e->text = pool_add(v);
            else if (e && (v = val(t, "hasitems")) && atoi(v)) e->kind = 'G';   /* folder */
            break;
        case V_RADIOS:                  /* radio providers; item fields come in random order */
        {   /* a field seen twice starts the next provider */
            int bit = (v = val(t, "cmd")) ? 1 : (v = val(t, "name")) ? 2 : (v = val(t, "type")) ? 4 : (v = val(t, "weight")) ? 8 : 0;
            if (!bit) break;
            if (rf & bit) { radio_flush(rcmd, rname, rtype); rf = 0; }
            rf |= bit;
            if (bit == 1) lstrcpyn(rcmd, v, sizeof rcmd);
            if (bit == 2) lstrcpyn(rname, v, sizeof rname);
            if (bit == 4) lstrcpyn(rtype, v, sizeof rtype);
            break;
        }
        case V_OPML:                    /* OPML menus: folders, albums/playlists, tracks, stations */
            if ((v = val(t, "id"))) e = ent_add('H', v, "");
            else if (e && ((v = val(t, "name")) || (v = val(t, "title")))) e->text = pool_add(v);   /* episodes: title */
            else if (e && (v = val(t, "type"))) {
                if (!strcmp(v, "search")) e->kind = 'Z';
                else if (!strcmp(v, "playlist")) { e->kind = 'O'; e->num = 1; }   /* open it, or play it all */
            } else if (e && (v = val(t, "isaudio")) && atoi(v) && e->kind == 'H') e->kind = 'o';
            else if (e && (v = val(t, "hasitems")) && atoi(v) && e->kind == 'H') e->kind = 'O';
            break;
        }
    }
    if (view == V_RADIOS) {
        if (rf) radio_flush(rcmd, rname, rtype);
    }
    if (nents == 0 || (view == V_TRACKS && nents == 1))
        ent_add('H', "", r[0] ? "(nothing here)" : "(no answer from the music server)");
    if (view == V_SEARCH && has_spotty) ent_add('X', "", ">> Search Spotify for this");
    SendMessage(wlist, LB_SETCOUNT, nents, 0);
    SendMessage(wlist, LB_SETCURSEL, 0, 0);
    InvalidateRect(wlist, NULL, TRUE);
}

static void player_cmd(const char *what)
{
    char line[800], pid[24];
    EnterCriticalSection(&lock);
    lstrcpy(pid, cur_player);
    LeaveCriticalSection(&lock);
    if (!pid[0]) return;
    sprintf(line, "%s %s", pid, what);
    post_job(line, -1);
}

static void open_tab(int t);

/* plugin menus (Spotty, TuneIn, RadioNet ...) are OPML, browsed per player with "<cmd> items" */
static void opml_open(const char *what, const char *item_id, int push)
{
    char cmd[700], pid[24], q[400];
    EnterCriticalSection(&lock); lstrcpy(pid, cur_player); LeaveCriticalSection(&lock);
    if (item_id) { esc(q, item_id); sprintf(cmd, "%s %s items 0 300 item_id:%s", pid, what, q); }
    else sprintf(cmd, "%s %s items 0 300", pid, what);
    request(cmd, V_OPML, push);
}

/* the search box on the Spotify and Radio tabs: searches find_cmd (at node find_id) */
static void opml_search(void)
{
    char cmd[900], pid[24], q[200], eq[600], id[300] = "";
    GetWindowText(wquery, q, sizeof q);
    if (!q[0]) { SetFocus(wquery); return; }
    EnterCriticalSection(&lock); lstrcpy(pid, cur_player); LeaveCriticalSection(&lock);
    esc_1252(eq, q);
    sprintf(crumb, "%s: %.150s", tab == T_SPOTIFY ? "Spotify" : tab == T_PODCAST ? "Podcasts" : "Radio", q);
    if (find_id[0]) { strcpy(id, " item_id:"); esc(id + 9, find_id); }
    sprintf(cmd, "%s %s items 0 100%s search:%s", pid, find_cmd, id, eq);
    request(cmd, V_OPML, 0);
    InvalidateRect(wmain_, NULL, FALSE);
}

static void find_default(int t)
{
    /* Spotify: item 0 of Spotty's menu is Search, 0.0 its "New Search" prompt; Podcasts: item 0 is
       "Search feeds"; Radio: TuneIn's search */
    lstrcpy(find_cmd, t == T_SPOTIFY ? "spotty" : t == T_PODCAST ? "podcasts" : "search");
    lstrcpy(find_id, t == T_SPOTIFY ? "0.0" : t == T_PODCAST ? "0" : "");
}

static void do_find(void)
{
    if (tab == T_SPOTIFY || tab == T_RADIO || tab == T_PODCAST) opml_search();
    else open_tab(T_SEARCH);
}

/* how: 0 = open/play (double-click, Enter), 1 = add to the queue, 2 = play now (menu) */
/* rows with a play key in front: albums, tracks, stations, playable Spotify lists */
#define PLAY_W 18
static int playable(entry *e)
{
    return strchr("LTSFoP", e->kind) || (e->kind == 'O' && e->num);
}

static void activate(int how_add);
static WNDPROC list_proc;
static LRESULT CALLBACK list_sub(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_LBUTTONDOWN && (short)LOWORD(lp) < 6 + PLAY_W) {
        int i = SendMessage(w, LB_ITEMFROMPOINT, 0, lp);
        if (HIWORD(i) == 0 && LOWORD(i) < nents && playable(&ents[LOWORD(i)])) {
            SendMessage(w, LB_SETCURSEL, LOWORD(i), 0);
            SetFocus(w);
            activate(2);                /* play now */
            return 0;
        }
    }
    return CallWindowProc(list_proc, w, m, wp, lp);
}

static void activate(int how_add)
{
    int i = SendMessage(wlist, LB_GETCURSEL, 0, 0), play_now = how_add == 2;
    char cmd[300], q[200];
    entry *e;
    if (i < 0 || i >= nents) return;
    if (play_now) how_add = 0;
    e = &ents[i];
    switch (e->kind) {
    case 'A':                           /* artist -> albums */
        if (how_add) { sprintf(cmd, "playlistcontrol cmd:add artist_id:%s", e->id); player_cmd(cmd); break; }
        sprintf(crumb, "Albums of %.150s", e->text);
        sprintf(cmd, "albums 0 500 artist_id:%s tags:lay sort:yearalbum", e->id);
        request(cmd, V_ALBUMS, 1);
        break;
    case 'L':                           /* album -> tracks */
        if (play_now) {
            player_cmd("power 1");
            sprintf(cmd, "playlistcontrol cmd:load album_id:%s", e->id);
            player_cmd(cmd);
            break;
        }
        if (how_add) { sprintf(cmd, "playlistcontrol cmd:add album_id:%s", e->id); player_cmd(cmd); break; }
        sprintf(crumb, "%.180s", e->text);
        lstrcpy(cur_album, e->id);
        sprintf(cmd, "titles 0 300 album_id:%s tags:d sort:tracknum", e->id);
        request(cmd, V_TRACKS, 1);
        break;
    case 'P':                           /* whole album */
        player_cmd("power 1");
        sprintf(cmd, "playlistcontrol cmd:%s album_id:%s", how_add ? "add" : "load", e->id);
        player_cmd(cmd);
        break;
    case 'T':                           /* album track: play the album from here */
        player_cmd("power 1");
        if (how_add) sprintf(cmd, "playlistcontrol cmd:add track_id:%s", e->id);
        else sprintf(cmd, "playlistcontrol cmd:load album_id:%s play_index:%d", cur_album, e->num);
        player_cmd(cmd);
        break;
    case 'S':                           /* single track from a search */
        player_cmd("power 1");
        sprintf(cmd, "playlistcontrol cmd:%s track_id:%s", how_add ? "add" : "load", e->id);
        player_cmd(cmd);
        break;
    case 'Q':                           /* queue entry: jump there */
        sprintf(cmd, "playlist index %d", e->num);
        player_cmd(cmd);
        break;
    case 'F':                           /* favourite / radio station */
        player_cmd("power 1");
        esc(q, e->id);
        sprintf(cmd, "favorites playlist %s item_id:%s", how_add ? "add" : "play", q);
        player_cmd(cmd);
        break;
    case 'G':                           /* favourites folder */
        esc(q, e->id);
        sprintf(crumb, "%.180s", e->text);
        sprintf(cmd, "favorites items 0 200 item_id:%s", q);
        request(cmd, V_RADIO, 1);
        break;
    case 'O':                           /* Spotify folder, album or playlist */
        if (how_add || (play_now && e->num)) {
            char big[300];
            esc(big, e->id);
            player_cmd("power 1");
            sprintf(cmd, "%s playlist %s item_id:%.250s", opml_cmd, how_add ? "add" : "play", big);
            player_cmd(cmd);
            break;
        }
        sprintf(crumb, "%.180s", e->text);
        opml_open(opml_cmd, e->id, 1);
        break;
    case 'o': {                         /* track or station; Spotify: touchToPlay plays the rest of its album/list too */
        char big[300];
        int rest = !how_add && !strcmp(opml_cmd, "spotty");
        esc(big, e->id);
        player_cmd("power 1");
        sprintf(cmd, "%s playlist %s item_id:%.250s%s", opml_cmd, how_add ? "add" : "play", big, rest ? " touchToPlay:1" : "");
        player_cmd(cmd);
        break;
    }
    case 'Z':                           /* a menu's search prompt: the box searches there now */
        lstrcpy(find_cmd, opml_cmd);
        lstrcpyn(find_id, e->id, sizeof find_id);
        sprintf(crumb, "Search %.150s: type in the box, then Find", e->text);
        SetFocus(wquery);
        break;
    case 'Y':                           /* "Search TuneIn" */
        find_default(T_RADIO);
        lstrcpy(crumb, "Search TuneIn: type in the box, then Find");
        SetFocus(wquery);
        break;
    case 'R':                           /* a radio provider */
        sprintf(crumb, "%.180s", e->text);
        opml_open(e->id, NULL, 1);
        break;
    case 'V':                           /* favourites */
        lstrcpy(crumb, "My favourites");
        request("favorites items 0 200", V_RADIO, 1);
        break;
    case 'X':                           /* from the library search: same words on Spotify */
        tab = T_SPOTIFY;
        for (i = 0; i < NTABS; i++) InvalidateRect(wtab[i], NULL, FALSE);
        find_default(T_SPOTIFY);
        opml_search();
        break;
    }
    if ((strchr("PTSQFo", e->kind) || play_now) && !how_add)
        SetFocus(wlist);
    InvalidateRect(wmain_, NULL, FALSE);
}

static void open_tab(int t)
{
    char cmd[700], q[200], eq[600];
    int i;
    tab = t;
    for (i = 0; i < NTABS; i++) InvalidateRect(wtab[i], NULL, FALSE);
    ShowWindow(wquery, t >= T_SEARCH && t != T_QUEUE ? SW_SHOW : SW_HIDE);
    ShowWindow(wgo, t >= T_SEARCH && t != T_QUEUE ? SW_SHOW : SW_HIDE);
    find_default(t);
    switch (t) {
    case T_ARTISTS:
        lstrcpy(crumb, "All artists");
        request("artists 0 5000", V_ARTISTS, 0);
        break;
    case T_ALBUMS:
        lstrcpy(crumb, "All albums");
        request("albums 0 5000 tags:la sort:artistalbum", V_ALBUMS, 0);
        break;
    case T_SEARCH:
        GetWindowText(wquery, q, sizeof q);
        if (!q[0]) {
            lstrcpy(crumb, "Type a name and press Enter");
            nents = 0; SendMessage(wlist, LB_SETCOUNT, 0, 0);
            nback = 0; view_cmd[0] = 0; ShowWindow(wback, SW_HIDE);
            SetFocus(wquery);
            break;
        }
        esc_1252(eq, q);
        sprintf(crumb, "Search: %.150s", q);
        sprintf(cmd, "search 0 60 term:%s", eq);
        request(cmd, V_SEARCH, 0);
        break;
    case T_QUEUE: {
        char pid[24];
        EnterCriticalSection(&lock); lstrcpy(pid, cur_player); LeaveCriticalSection(&lock);
        lstrcpy(crumb, "Now playing queue");
        sprintf(cmd, "%s status 0 500 tags:a", pid);
        request(cmd, V_QUEUE, 0);
        break;
    }
    case T_RADIO: {
        char pid[24];
        EnterCriticalSection(&lock); lstrcpy(pid, cur_player); LeaveCriticalSection(&lock);
        lstrcpy(crumb, "Radio (type in the box to search TuneIn)");
        sprintf(cmd, "%s radios 0 50", pid);
        request(cmd, V_RADIOS, 0);
        break;
    }
    case T_SPOTIFY:
        lstrcpy(crumb, "Spotify (type in the box to search)");
        opml_open("spotty", NULL, 0);
        break;
    case T_PODCAST:
        lstrcpy(crumb, "Podcasts (type in the box to search feeds)");
        opml_open("podcasts", NULL, 0);
        break;
    }
    InvalidateRect(wmain_, NULL, FALSE);
}

/* ------------------------------------------------------------- drawing */

/* The deck is modelled on the Creative Sound Blaster MIDI player of 1996 (CTMIDI32.EXE):
   same 80-pixel height; keys drawn in code, or the bitmaps loaded at run time from a Creative install
   (MIDRES32.DLL, INI skin=), hollow-free 2-pixel LCD digits drawn like the original.  Added:
   the album art between the left keys and the LCD, and a text block in a widened LCD.
   Without the DLL the keys are drawn in code in the same style. */

#define LCD_L     130                   /* LCD inner area */
#define LCD_R     565
#define DX        287                   /* everything right of the LCD moved this far vs. the original */
static RECT r_cover = { 52, 5, 124, 77 };
static RECT r_lcd = { LCD_L, 7, LCD_R, 57 };
static RECT r_prog = { 145, 50, 278, 54 };
static RECT r_vol = { 543, 13, 561, 46 };   /* 17 one-pixel lines, 2 px apart */
static RECT r_mode[3] = { { 505, 11, 540, 21 }, { 505, 24, 540, 34 }, { 505, 37, 540, 47 } };
static RECT r_text = { 288, 8, 498, 57 };   /* dot-matrix block: 3 lines of 21 characters */
static int wcl = W_FULL, nx;            /* nx: how much the dot matrix gives up so the deck fits the screen */
static int marquee;
static int remain;                      /* time display counts down (click on it) */

static HBITMAP sk_btn, sk_num, sk_mode[3][2];   /* skin bitmaps (green key already made silver) */
static int skin_ok;

static void fill(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void hline(HDC dc, int x0, int x1, int y, COLORREF c) { RECT r; SetRect(&r, x0, y, x1, y + 1); fill(dc, r, c); }
static void vline(HDC dc, int x, int y0, int y1, COLORREF c) { RECT r; SetRect(&r, x, y0, x + 1, y1); fill(dc, r, c); }

/* 1-pixel frame: sunken (grey/white) or raised (white/grey) */
static void frame1(HDC dc, RECT r, int in)
{
    hline(dc, r.left, r.right, r.top, in ? C_SH : C_HI);
    vline(dc, r.left, r.top, r.bottom, in ? C_SH : C_HI);
    hline(dc, r.left, r.right, r.bottom - 1, in ? C_HI : C_SH);
    vline(dc, r.right - 1, r.top, r.bottom, in ? C_HI : C_SH);
}

/* the 2-pixel well around the LCD and the art */
static void well(HDC dc, RECT r)
{
    frame1(dc, r, 1);
    InflateRect(&r, -1, -1);
    hline(dc, r.left, r.right, r.top, C_DK);
    vline(dc, r.left, r.top, r.bottom, C_DK);
}

/* ---- skin from the Creative DLL ---- */

/* a resource bitmap as 24-bit bottom-up pixels */
static unsigned char *skin_pixels(HMODULE m, int id, int *w, int *h, BITMAPINFO *bi)
{
    HBITMAP src = LoadBitmap(m, MAKEINTRESOURCE(id));
    BITMAP bm;
    unsigned char *px;
    HDC dc;
    if (!src) return NULL;
    GetObject(src, sizeof bm, &bm);
    *w = bm.bmWidth; *h = bm.bmHeight;
    memset(bi, 0, sizeof *bi);
    bi->bmiHeader.biSize = sizeof bi->bmiHeader;
    bi->bmiHeader.biWidth = *w;
    bi->bmiHeader.biHeight = *h;
    bi->bmiHeader.biPlanes = 1;
    bi->bmiHeader.biBitCount = 24;
    bi->bmiHeader.biCompression = BI_RGB;
    px = malloc(((*w * 3 + 3) & ~3) * *h);
    dc = GetDC(NULL);
    GetDIBits(dc, src, 0, *h, px, bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    DeleteObject(src);
    return px;
}

#define PX(px, w, h, x, y) ((px) + ((h) - 1 - (y)) * (((w) * 3 + 3) & ~3) + (x) * 3)   /* B,G,R */
#define IS_KEY(p) ((p)[0] == 0 && (p)[1] == 255 && (p)[2] == 0)

/* whole bitmap of cw x ch cells; the green key on each cell's 1-pixel rim shows black, as in the
   original (the keys sit on black; the power lamp inside the key is the same pure green and stays) */
static HBITMAP skin_load(HMODULE m, int id, int cw, int ch)
{
    BITMAPINFO bi;
    int w, h, x, y;
    HBITMAP out;
    HDC dc;
    unsigned char *px = skin_pixels(m, id, &w, &h, &bi);
    if (!px) return NULL;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned char *p = PX(px, w, h, x, y);
            int rim = x % cw == 0 || x % cw == cw - 1 || y % ch == 0 || y % ch == ch - 1;
            if (rim && IS_KEY(p)) p[0] = p[1] = p[2] = 0;
        }
    dc = GetDC(NULL);
    out = CreateDIBitmap(dc, &bi.bmiHeader, CBM_INIT, px, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    free(px);
    return out;
}

/* one 35x10 mode icon (green mask): black icon on teal, on cyan when the mode is on - as the original */
static HBITMAP skin_mode_icon(HMODULE m, int id, int cx, int cy, int on)
{
    BITMAPINFO bi, bo;
    int w, h, x, y;
    unsigned char *px = skin_pixels(m, id, &w, &h, &bi), out[40 * 3 * 10];
    HBITMAP bmp;
    HDC dc;
    if (!px) return NULL;
    for (y = 0; y < 10; y++)
        for (x = 0; x < 35; x++) {
            unsigned char *p = PX(px, w, h, cx + x, cy + y), *o = out + (9 - y) * 108 + x * 3;
            int icon = IS_KEY(p);
            o[0] = icon ? 0 : on ? 255 : 128;      /* B */
            o[1] = icon ? 0 : on ? 255 : 128;      /* G */
            o[2] = 0;                               /* R */
        }
    bo = bi;
    bo.bmiHeader.biWidth = 35;
    bo.bmiHeader.biHeight = 10;
    dc = GetDC(NULL);
    bmp = CreateDIBitmap(dc, &bo.bmiHeader, CBM_INIT, out, &bo, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    free(px);
    return bmp;
}

static void skin_init(void)
{
    char path[MAX_PATH];
    HMODULE m;
    /* key bitmaps from the user's own copy of Creative's MIDRES32.DLL (bitmaps 300/301/321/322), looked for
       1. next to RETROSQ.EXE, 2. INI skin=, 3. the standard Creative install path.  None of Creative's
       bitmaps ship with RetroSqueeze; without the DLL the keys are drawn in code.
       LoadLibrary, not as a datafile: Win98 can't LoadBitmap from a datafile module. */
    char *p;
    m = NULL;
    lstrcpy(path, ini);                         /* ini = <exe folder>\RETROSQ.INI */
    if ((p = strrchr(path, '\\')) != NULL) strcpy(p + 1, "MIDRES32.DLL");
    if (GetFileAttributes(path) != 0xFFFFFFFF) m = LoadLibrary(path);
    if (!m) {
        GetPrivateProfileString("retrosqueeze", "skin", "C:\\Program Files\\Creative\\CTSND\\PROGRAM\\MIDRES32.DLL",
                                path, sizeof path, ini);
        if (path[0] && GetFileAttributes(path) != 0xFFFFFFFF) m = LoadLibrary(path);
    }
    if (!m) m = GetModuleHandle(NULL);
    sk_btn = skin_load(m, 301, 42, 13);         /* 4 states x 11 keys, 42x13 */
    sk_num = skin_load(m, 300, 21, 13);         /* 4 states x 10 number keys, 21x13 */
    sk_mode[0][0] = skin_mode_icon(m, 322, 70, 0, 0);   /* repeat one: loop with a 1 */
    sk_mode[0][1] = skin_mode_icon(m, 322, 70, 0, 1);
    sk_mode[1][0] = skin_mode_icon(m, 322, 0, 0, 0);    /* repeat all: loop */
    sk_mode[1][1] = skin_mode_icon(m, 322, 0, 0, 1);
    sk_mode[2][0] = skin_mode_icon(m, 321, 0, 0, 0);    /* shuffle: crossed arrows */
    sk_mode[2][1] = skin_mode_icon(m, 321, 0, 0, 1);
    if (m != GetModuleHandle(NULL)) FreeLibrary(m);
    skin_ok = sk_btn && sk_num && sk_mode[2][1];
}

static void blit(HDC dc, HBITMAP b, int sx, int sy, int w, int h, int dx, int dy)
{
    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ o = SelectObject(m, b);
    BitBlt(dc, dx, dy, w, h, m, sx, sy, SRCCOPY);
    SelectObject(m, o);
    DeleteDC(m);
}

/* a key in the Creative style, drawn in code (text keys, or no DLL): stippled top and bottom rows */
static void ct_key(HDC dc, RECT r, int down, int en)
{
    int x;
    fill(dc, r, down ? C_SH : C_FACE);
    for (x = r.left + 1; x < r.right - 1; x += 2) {
        SetPixel(dc, x + (down ? 1 : 0), r.top + 1, down ? C_FACE : C_HI);
        SetPixel(dc, x + 1, r.top + 2, down ? C_FACE : C_HI);
        SetPixel(dc, x + (down ? 1 : 0), r.bottom - 3, down ? C_FACE : C_SH);
        SetPixel(dc, x + 1, r.bottom - 2, down ? C_FACE : C_SH);
    }
    hline(dc, r.left, r.right, r.top, down ? C_DK : C_HI);
    vline(dc, r.left, r.top, r.bottom, down ? C_DK : C_HI);
    hline(dc, r.left, r.right, r.bottom - 1, down ? C_HI : C_DK);
    vline(dc, r.right - 1, r.top, r.bottom, down ? C_HI : C_DK);
    (void)en;
}

/* LCD digit as Creative drew it: 2-pixel segments with cut corners */
static void seg7(HDC dc, int x, int y, int w, int h, int d)
{
    static const unsigned char m[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int bits = d >= 0 && d <= 9 ? m[d] : 0, mid = y + h / 2 - 1;
    if (bits & 0x01) { hline(dc, x + 2, x + w - 2, y, C_CYAN); hline(dc, x + 3, x + w - 3, y + 1, C_CYAN); }              /* a */
    if (bits & 0x40) { hline(dc, x + 2, x + w - 2, mid, C_CYAN); hline(dc, x + 2, x + w - 2, mid + 1, C_CYAN); }        /* g */
    if (bits & 0x08) { hline(dc, x + 3, x + w - 3, y + h - 2, C_CYAN); hline(dc, x + 2, x + w - 2, y + h - 1, C_CYAN); } /* d */
    if (bits & 0x20) { vline(dc, x, y + 2, mid, C_CYAN); vline(dc, x + 1, y + 3, mid - 1, C_CYAN); }                    /* f */
    if (bits & 0x02) { vline(dc, x + w - 1, y + 2, mid, C_CYAN); vline(dc, x + w - 2, y + 3, mid - 1, C_CYAN); }        /* b */
    if (bits & 0x10) { vline(dc, x, mid + 2, y + h - 2, C_CYAN); vline(dc, x + 1, mid + 3, y + h - 3, C_CYAN); }        /* e */
    if (bits & 0x04) { vline(dc, x + w - 1, mid + 2, y + h - 2, C_CYAN); vline(dc, x + w - 2, mid + 3, y + h - 3, C_CYAN); } /* c */
}

static void lcd_text(HDC dc, HFONT f, COLORREF c, int x, int y, int maxw, const char *s, int scroll)
{
    SIZE sz;
    RECT clip;
    SetRect(&clip, x, y - 1, x + maxw, y + 13);
    SelectObject(dc, f);
    SetTextColor(dc, c);
    GetTextExtentPoint32(dc, s, strlen(s), &sz);
    if (scroll && sz.cx > maxw) {       /* marquee: run through, pause at the start */
        int span = sz.cx + 40, off = marquee * 2 % (span + 60) - 60;
        if (off < 0) off = 0;
        ExtTextOut(dc, x - off, y, ETO_CLIPPED, &clip, s, strlen(s), NULL);
        ExtTextOut(dc, x - off + span, y, ETO_CLIPPED, &clip, s, strlen(s), NULL);
    } else
        ExtTextOut(dc, x, y, ETO_CLIPPED, &clip, s, strlen(s), NULL);
}

/* ---- dot matrix: 5x8 cells, 1-pixel dots on a 2-pixel pitch, unlit dots glowing faintly ---- */
#define DM_W      ((r_text.right - r_text.left) / 2)   /* dots across */
#define DM_ON     0x0000FFFFUL                          /* 0x00RRGGBB */
#define DM_OFF    0x00002A30UL
static unsigned long *dm_px;            /* DIB section pixels of the block */
static HBITMAP dm_bmp;

static int dm_char(unsigned char c)
{
    switch (c) {                        /* cp1252 extras the ISO-8859-1 font lacks */
    case 0x91: case 0x92: return '\'';
    case 0x93: case 0x94: return '"';
    case 0x96: case 0x97: return '-';
    case 0x85: return '.';
    }
    return c;
}

/* one line of text at dot row y0 (8 rows), scrolled by off dots */
static void dm_line(int y0, const char *s, int off, unsigned long on)
{
    int n = strlen(s), c, r, w = DM_W, pw = r_text.right - r_text.left;
    for (c = 0; c < w; c++) {
        int sc = c + off, ch = sc / 5, bit = sc % 5;
        const unsigned char *g = ch >= 0 && ch < n ? dm5x8[dm_char((unsigned char)s[ch])] : NULL;
        for (r = 0; r < 8; r++) {
            unsigned long *p = dm_px + (y0 + r * 2) * pw + c * 2;
            *p = g && (g[r] & (0x80 >> bit)) ? on : DM_OFF;
        }
    }
}

/* marquee offset in dots for a line of n characters: run through, pause at the start */
static int dm_scroll(int n)
{
    int span = n * 5 + 10, off;
    if (n * 5 <= DM_W) return 0;
    off = marquee % (span + 20) - 20;
    return off < 0 ? 0 : off;
}

static void dm_block(HDC dc, const char *l1, const char *l2, const char *l3, unsigned long c3)
{
    int pw = r_text.right - r_text.left, ph = r_text.bottom - r_text.top;
    HDC m;
    HGDIOBJ o;
    if (!dm_bmp) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = pw;
        bi.bmiHeader.biHeight = -ph;    /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dm_bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&dm_px, NULL, 0);
        if (!dm_bmp) return;
    }
    GdiFlush();
    memset(dm_px, 0, pw * ph * 4);
    dm_line(0, l1, dm_scroll(strlen(l1)), DM_ON);
    dm_line(17, l2, dm_scroll(strlen(l2)), DM_ON);
    dm_line(34, l3, dm_scroll(strlen(l3)), c3);
    m = CreateCompatibleDC(dc);
    o = SelectObject(m, dm_bmp);
    BitBlt(dc, r_text.left, r_text.top, pw, ph, m, 0, 0, SRCCOPY);
    SelectObject(m, o);
    DeleteDC(m);
}

/* dot-matrix block: title, artist, album (year) */
static void dm_status(HDC dc, status_t *s)
{
    char l3[300];
    sprintf(l3, atoi(s->year) >= 1900 ? "%s (%s)" : "%s", s->album, s->year);   /* tags sometimes say "1" */
    dm_block(dc, s->title[0] ? s->title : (s->power ? "- stopped -" : "- off -"), s->artist, l3, DM_ON);
}

static void paint_display(HDC dc, status_t *s)
{
    static const char *modes[3] = { "REP1", "REP", "SHUF" };
    RECT b;
    char buf[300];
    int sec, i;
    /* art well */
    b = r_cover; well(dc, b);
    InflateRect(&b, -2, -2);
    fill(dc, b, C_DK);
    if (cover) blit(dc, cover, 0, 0, ART, ART, b.left, b.top);
    /* LCD */
    b = r_lcd; InflateRect(&b, 2, 2); well(dc, b);
    fill(dc, r_lcd, C_LCD);
    SetBkMode(dc, TRANSPARENT);
    if (!s->valid || !s->connected) {
        lcd_text(dc, f_lcd, C_CYAN, 150, 26, 300, s->valid ? "Player is not connected" : "No connection to the music server", 0);
        return;
    }
    /* lamp + labels */
    SetRect(&b, 134, 11, 140, 17);
    fill(dc, b, s->mode == 1 || (s->mode == 2 && marquee / 6 % 2) ? C_CYAN : C_TEAL);
    lcd_text(dc, f_label, C_CYAN, 147, 9, 50, bank ? "Track" : "Track", 0);
    lcd_text(dc, f_label, C_CYAN, 198, 9, 40, s->time >= 6000 ? "Hour" : "Min", 0);
    /* annunciators: the codec lit in a box, then what reaches this PC (PCM from LMS, or MP3 decoded here) */
    if (s->codec[0] && s->mode) {
        SIZE sz;
        int x = 224, rate, f = s->own ? slimplay_format(&rate) : 0;
        SelectObject(dc, f_label);
        GetTextExtentPoint32(dc, s->codec, strlen(s->codec), &sz);
        SetRect(&b, x - 2, 9, x + sz.cx + 2, 18);
        fill(dc, b, C_CYAN);
        SetTextColor(dc, C_DK);
        TextOut(dc, x, 8, s->codec, strlen(s->codec));
        x += sz.cx + 6;
        if (f) lcd_text(dc, f_label, C_CYAN, x, 8, 287 - x, f == 'p' ? "PCM" : "DEC", 0);   /* up to the dot matrix (288) */
    }
    /* track number, time */
    i = s->tracks ? s->index + 1 : 0;
    if (i > 99) i = 99;
    seg7(dc, 148, 20, 16, 25, s->tracks ? i / 10 : -1);
    seg7(dc, 169, 20, 16, 25, s->tracks ? i % 10 : -1);
    sec = (int)s->time;
    if (remain && s->duration > 0) {    /* remaining time, with a minus in front */
        sec = (int)(s->duration - s->time + 0.999);
        if (sec < 0) sec = 0;
        SetRect(&b, 189, 33, 195, 35); fill(dc, b, C_CYAN);
    }
    if (sec >= 6000) sec /= 60;         /* past 99 minutes: h:mm */
    if (sec >= 6000) sec = 5999;
    seg7(dc, 198, 23, 14, 21, sec / 600);
    seg7(dc, 216, 23, 14, 21, sec / 60 % 10);
    SetRect(&b, 233, 29, 236, 32); fill(dc, b, C_CYAN);
    SetRect(&b, 233, 36, 236, 39); fill(dc, b, C_CYAN);
    seg7(dc, 240, 23, 14, 21, sec % 60 / 10);
    seg7(dc, 258, 23, 14, 21, sec % 10);
    /* progress */
    b = r_prog;
    fill(dc, b, C_SH);
    if (s->duration > 0) {
        b.right = b.left + (int)((r_prog.right - r_prog.left) * (s->time < s->duration ? s->time : s->duration) / s->duration);
        fill(dc, b, C_CYAN);
    }
    dm_status(dc, s);
    /* mode keys: repeat one, repeat all, shuffle */
    for (i = 0; i < 3; i++) {
        int on = i == 0 ? s->repeat == 1 : i == 1 ? s->repeat == 2 : s->shuffle != 0;
        if (skin_ok) blit(dc, sk_mode[i][on], 0, 0, 35, 10, r_mode[i].left, r_mode[i].top);
        else {
            fill(dc, r_mode[i], C_TEAL);
            SelectObject(dc, f_label);
            SetTextColor(dc, on ? C_CYAN : C_DK);
            DrawText(dc, modes[i], -1, &r_mode[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    /* volume: a stack of lines, lit from the bottom */
    for (i = 0; i < 17; i++)
        hline(dc, r_vol.left, r_vol.right, r_vol.bottom - 1 - i * 2, s->volume > i * 100 / 17 ? C_CYAN : C_SH);
}

/* the "MIDI" badge, with our name */
static void logo(HDC dc, int x, int y)
{
    RECT b;
    SIZE sz;
    SetRect(&b, x, y, x + 25, y + 25);
    fill(dc, b, C_DK);
    InflateRect(&b, -2, -2);
    fill(dc, b, C_YEL);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, f_logo);
    SetTextColor(dc, C_DK);
    DrawText(dc, "R", -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, f_logo2);
    TextOut(dc, x + 28, y + 3, "SQUEEZE", 7);
    GetTextExtentPoint32(dc, "SQUEEZE", 7, &sz);
    SetRect(&b, x + 25, y + 21, x + 28 + sz.cx, y + 23); fill(dc, b, C_DK);
    SetRect(&b, x + 26 + sz.cx, y + 19, x + 30 + sz.cx, y + 25); fill(dc, b, C_DK);
}

/* sunken tray round keys spanning (x0,y0)-(x1,y1); pad = silver between keys and border */
static void tray(HDC dc, int x0, int y0, int x1, int y1, int pad)
{
    int a = x0 - pad - 2, b = y0 - pad - 2, c = x1 + pad, d = y1 + pad;
    RECT r;
    SetRect(&r, a, b, c + 2, b + 2); fill(dc, r, C_SH);
    SetRect(&r, a, b, a + 2, d + 2); fill(dc, r, C_SH);
    SetRect(&r, a + 2, d, c + 2, d + 2); fill(dc, r, C_HI);
    SetRect(&r, c, b + 2, c + 2, d + 2); fill(dc, r, C_HI);
}

static void paint(HWND w)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps), m;
    HBITMAP bm;
    HGDIOBJ ob;
    RECT all, r;
    int h = compact ? H_COMPACT : H_CLIENT, i;
    status_t s;
    EnterCriticalSection(&lock);
    s = st;
    LeaveCriticalSection(&lock);
    SetRect(&all, 0, 0, W_CLIENT, h);
    /* draw off-screen, no flicker */
    m = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, W_CLIENT, H_CLIENT);
    ob = SelectObject(m, bm);
    fill(m, all, C_FACE);
    /* the original's trays: 2 px grey above/left, 2 px white below/right, 1 px of silver round the keys */
    for (i = 0; i < 3; i++) tray(m, 3, 9 + i * 20, 45, 22 + i * 20, 1);
    tray(m, 131, 63, 341, 76, 1);
    tray(m, 628 - nx, 11, 754 - nx, 24, 1);
    tray(m, 586 - nx, 31, 754 - nx, 44, 1);
    paint_display(m, &s);
    logo(m, 663 - nx, 50);
    /* window edge (over the trays that run into it): black line, white highlight, like the original */
    hline(m, 0, W_CLIENT, 0, C_DK); hline(m, 0, W_CLIENT, h - 1, C_DK);
    vline(m, 0, 0, h, C_DK); vline(m, W_CLIENT - 1, 0, h, C_DK);
    hline(m, 1, W_CLIENT - 1, 1, C_HI); vline(m, 1, 1, h - 1, C_HI);
    hline(m, 2, W_CLIENT - 2, h - 2, C_SH); vline(m, W_CLIENT - 2, 2, h - 1, C_SH);
    if (!compact) {
        hline(m, 2, W_CLIENT - 2, H_COMPACT - 1, C_SH);
        hline(m, 2, W_CLIENT - 2, H_COMPACT, C_HI);
        SetRect(&r, 6, 134, W_CLIENT - 6, 352); well(m, r);
        SetBkMode(m, TRANSPARENT);
        SelectObject(m, f_ui);
        SetTextColor(m, C_DK);
        SetRect(&r, 8, 111, 300, 129);
        DrawText(m, crumb, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
           m, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(m, ob);
    DeleteObject(bm);
    DeleteDC(m);
    EndPaint(w, &ps);
}

/* which row of MIDRES32 bitmap 301 a key uses (-1: text key) */
static int skin_row(int id)
{
    switch (id) {
    case ID_CLOSE: return 0;            /* green lamp */
    case ID_FOLD: return 1;             /* bar: fold the browser */
    case ID_MINI: return 2;             /* triangle down: minimize */
    case ID_STOP: return 3;
    case ID_PLAY: return 4;
    case ID_PAUSE: return 5;
    case ID_PREV: return 6;
    case ID_REW: return 7;
    case ID_FF: return 8;
    case ID_NEXT: return 9;
    }
    return -1;
}

/* key symbols drawn in code (no Creative DLL): 5 px high, engraved like the Win9x era -
   black symbol with a white shadow 1 px right/down.  x = key centre, y = top of the symbol. */
static void tri_r(HDC dc, int x, int y, COLORREF c)     /* > 3 wide, 5 high */
{
    int i;
    for (i = 0; i < 5; i++) hline(dc, x, x + 3 - (i < 2 ? 2 - i : i - 2), y + i, c);
}
static void tri_l(HDC dc, int x, int y, COLORREF c)     /* < 3 wide, 5 high, x = right edge + 1 */
{
    int i;
    for (i = 0; i < 5; i++) hline(dc, x - 3 + (i < 2 ? 2 - i : i - 2), x, y + i, c);
}
static void glyph1(HDC dc, int row, int x, int y, COLORREF c)
{
    RECT b;
    int i;
    switch (row) {
    case 0:                             /* power lamp (close): drawn in glyph() */
        break;
    case 1:                             /* bar: fold the browser */
        SetRect(&b, x - 5, y + 1, x + 6, y + 3); fill(dc, b, c);
        break;
    case 2:                             /* triangle down: minimize */
        for (i = 0; i < 4; i++) hline(dc, x - 3 + i, x + 4 - i, y + i, c);
        break;
    case 3:                             /* stop: a filled square */
        SetRect(&b, x - 2, y, x + 3, y + 5); fill(dc, b, c);
        break;
    case 4:                             /* play */
        tri_r(dc, x - 1, y, c);
        break;
    case 5:                             /* pause */
        SetRect(&b, x - 3, y, x - 1, y + 5); fill(dc, b, c);
        SetRect(&b, x + 1, y, x + 3, y + 5); fill(dc, b, c);
        break;
    case 6:                             /* previous: |< */
        vline(dc, x - 3, y, y + 5, c);
        tri_l(dc, x + 2, y, c);
        break;
    case 7:                             /* back: << */
        tri_l(dc, x, y, c);
        tri_l(dc, x + 3, y, c);
        break;
    case 8:                             /* forward: >> */
        tri_r(dc, x - 3, y, c);
        tri_r(dc, x, y, c);
        break;
    case 9:                             /* next: >| */
        tri_r(dc, x - 2, y, c);
        vline(dc, x + 2, y, y + 5, c);
        break;
    }
}
static void glyph(HDC dc, int row, RECT r, int down, int en)
{
    int x = (r.left + r.right) / 2 + down, y = r.top + 4 + down;
    if (row == 0) {                     /* the green power lamp, sunk into the key */
        RECT b;
        SetRect(&b, x - 6, y, x + 6, y + 5); fill(dc, b, C_DK);
        SetRect(&b, x - 5, y + 1, x + 5, y + 4); fill(dc, b, en ? RGB(0, 255, 0) : C_SH);
        hline(dc, x - 6, x + 7, y + 5, C_HI);
        vline(dc, x + 6, y, y + 6, C_HI);
        return;
    }
    if (en) glyph1(dc, row, x + 1, y + 1, C_HI);
    glyph1(dc, row, x, y, en ? C_DK : C_SH);
}

static void draw_button(DRAWITEMSTRUCT *d)
{
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    int id = d->CtlID, down = d->itemState & ODS_SELECTED, en = IsWindowEnabled(d->hwndItem), row = skin_row(id), col, lit = 0;
    status_t s;
    EnterCriticalSection(&lock); s = st; LeaveCriticalSection(&lock);
    if (id >= ID_NUM0 && id < ID_NUM0 + 10) lit = s.tracks && s.index == bank * 10 + id - ID_NUM0;
    if (id >= ID_TAB0 && id < ID_TAB0 + NTABS) lit = id - ID_TAB0 == tab;
    col = !en ? 3 : (down || lit) ? 2 : 0;   /* states: normal, focus, pressed, disabled */
    if (skin_ok && row >= 0) { blit(dc, sk_btn, col * 42, row * 13, 42, 13, r.left, r.top); return; }
    if (skin_ok && id >= ID_NUM0 && id < ID_NUM0 + 10) { blit(dc, sk_num, col * 21, (id - ID_NUM0) * 13, 21, 13, r.left, r.top); return; }
    ct_key(dc, r, down || lit, en);
    if (row >= 0) { glyph(dc, row, r, down || lit, en); return; }
    {   /* text keys (tabs, number keys ...) */
        char text[40];
        if (id >= ID_NUM0 && id < ID_NUM0 + 10) sprintf(text, "%d", (id - ID_NUM0 + 1) % 10);
        else GetWindowText(d->hwndItem, text, sizeof text);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, en ? C_DK : C_SH);
        SelectObject(dc, lit && id >= ID_TAB0 && id < ID_TAB0 + NTABS ? f_btnb : f_btn);
        if (down || lit) OffsetRect(&r, 1, 1);
        DrawText(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void draw_item(DRAWITEMSTRUCT *d)
{
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    entry *e;
    int sel = d->itemState & ODS_SELECTED;
    char num[16];
    if ((int)d->itemID < 0 || (int)d->itemID >= nents) { fill(dc, r, C_LCD); return; }
    e = &ents[d->itemID];
    fill(dc, r, sel ? C_TEAL : C_LCD);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, f_ui);
    r.left += 6;
    r.right -= 6;
    if (playable(e)) {                  /* a small play triangle, click it to play */
        int i, cy = (r.top + r.bottom) / 2;
        for (i = 0; i < 5; i++) vline(dc, r.left + 3 + i, cy - 4 + i, cy + 5 - i, sel ? C_HI : C_CYAN);
    }
    if (e->kind != 'H') r.left += PLAY_W;
    if (e->kind == 'H') {
        SetTextColor(dc, C_LED);
        DrawText(dc, e->text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (e->kind == 'T' || e->kind == 'Q') {
        sprintf(num, "%d.", e->num + 1);
        SetTextColor(dc, sel ? C_HI : C_SH);
        DrawText(dc, num, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        r.left += 30;
    }
    if (e->kind == 'L' && e->num) {
        sprintf(num, "%d", e->num);
        SetTextColor(dc, sel ? C_HI : C_SH);
        DrawText(dc, num, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        r.left += 40;
    }
    if (e->sub) {
        RECT rs = r;
        SetTextColor(dc, sel ? C_HI : C_SH);
        DrawText(dc, e->sub, -1, &rs, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        r.right -= e->kind == 'T' ? 50 : 200;
    }
    SetTextColor(dc, sel ? C_HI : strchr("PZXY", e->kind) ? C_YEL : strchr("GORV", e->kind) ? C_CYAN : C_FACE);
    DrawText(dc, e->text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (sel && (d->itemState & ODS_FOCUS)) DrawFocusRect(dc, &d->rcItem);
}

/* decoded cover -> ART x ART DDB, centred on black (covers can be non-square) */
static void take_cover(void)
{
    unsigned char *dib;
    int w, h;
    EnterCriticalSection(&lock);
    dib = cover_dib; w = cover_w; h = cover_h;
    cover_dib = NULL;
    cover_new = 0;
    LeaveCriticalSection(&lock);
    if (cover) { DeleteObject(cover); cover = NULL; }
    if (dib) {
        BITMAPINFO bi;
        HDC dc = GetDC(wmain_), m = CreateCompatibleDC(dc);
        HGDIOBJ o;
        RECT all;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 24;
        bi.bmiHeader.biCompression = BI_RGB;
        cover = CreateCompatibleBitmap(dc, ART, ART);
        o = SelectObject(m, cover);
        SetRect(&all, 0, 0, ART, ART);
        fill(m, all, RGB(0, 0, 0));
        SetDIBitsToDevice(m, (ART - w) / 2, (ART - h) / 2, w, h, 0, 0, 0, h, dib, &bi, DIB_RGB_COLORS);
        SelectObject(m, o);
        DeleteDC(m);
        ReleaseDC(wmain_, dc);
        free(dib);
    }
}

/* ---------------------------------------------------------- the window */

static HWND child(const char *cls, const char *text, DWORD style, int id, int x, int y, int w, int h)
{
    HWND c = CreateWindowEx(!strcmp(cls, "EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                            x, y, w, h, wmain_, (HMENU)id, inst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)f_ui, 0);
    return c;
}

static void fill_players(void)
{
    int i, sel = -1;
    char name[80];
    SendMessage(wplayer, CB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&lock);
    for (i = 0; i < nplayers; i++) {
        sprintf(name, players[i].connected ? "%s" : "%s (off)", players[i].name);
        SendMessage(wplayer, CB_ADDSTRING, 0, (LPARAM)name);
        if (!strcmp(players[i].id, cur_player)) sel = i;
    }
    if (sel < 0 && nplayers) {          /* first time: this PC's own player, else the first connected */
        for (i = 0; i < nplayers && sel < 0; i++) if (!lstrcmpi(players[i].id, own_mac)) sel = i;
        for (i = 0; i < nplayers && sel < 0; i++) if (players[i].connected) sel = i;
        if (sel < 0) sel = 0;
        lstrcpy(cur_player, players[sel].id);
        WritePrivateProfileString("retrosqueeze", "player", cur_player, ini);
    }
    LeaveCriticalSection(&lock);
    SendMessage(wplayer, CB_SETCURSEL, sel, 0);
}

static int same_players(void)
{
    int i, n = SendMessage(wplayer, CB_GETCOUNT, 0, 0);
    char a[80], b[80];
    EnterCriticalSection(&lock);
    if (n != nplayers) { LeaveCriticalSection(&lock); return 0; }
    for (i = 0; i < n; i++) {
        SendMessage(wplayer, CB_GETLBTEXT, i, (LPARAM)a);
        sprintf(b, players[i].connected ? "%s" : "%s (off)", players[i].name);
        if (strcmp(a, b)) { LeaveCriticalSection(&lock); return 0; }
    }
    LeaveCriticalSection(&lock);
    return 1;
}

/* clicks on the painted parts of the LCD: progress bar, volume lines, mode keys */
/* Fit the deck to the screen (640x480 too): the dot matrix gets narrower in whole characters,
   everything right of it moves left, the browser below shrinks with it.  Again on WM_DISPLAYCHANGE. */
static int tab_on(int t)
{
    return t == T_SPOTIFY ? has_spotty : t == T_PODCAST ? has_podcasts : 1;
}

static void layout(void)
{
    static const int bx[7] = { 628, 670, 712, 586, 628, 670, 712 }, by[7] = { 11, 11, 11, 31, 31, 31, 31 };
    RECT wa;
    int i, n, k, tw;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    nx = W_FULL - (wa.right - wa.left);
    nx = nx <= 0 ? 0 : (nx + 9) / 10 * 10;      /* 10 px = one character */
    if (nx > 150) nx = 150;                     /* at least 6 characters */
    wcl = W_FULL - nx;
    SetRect(&r_lcd, LCD_L, 7, LCD_R - nx, 57);
    SetRect(&r_vol, 543 - nx, 13, 561 - nx, 46);
    for (i = 0; i < 3; i++) SetRect(&r_mode[i], 505 - nx, 11 + i * 13, 540 - nx, 21 + i * 13);
    r_text.right = 498 - nx;
    if (dm_bmp) { DeleteObject(dm_bmp); dm_bmp = NULL; }
    if (!wmain_) return;
    for (i = 0; i < 7; i++) MoveWindow(wbtn[i], bx[i] - nx, by[i], 42, 13, FALSE);
    for (i = n = 0; i < NTABS; i++) n += tab_on(i);
    tw = nx ? (wcl - 8 - 6 - 130) / n : 78;     /* tabs, then the player list in what is left */
    if (tw > 78) tw = 78;
    for (i = k = 0; i < NTABS; i++) {           /* plugin tabs the server lacks are hidden */
        ShowWindow(wtab[i], tab_on(i) ? SW_SHOW : SW_HIDE);
        if (tab_on(i)) MoveWindow(wtab[i], 8 + k++ * tw, 86, tw - 2, 20, FALSE);
    }
    i = nx ? 8 + n * tw + 4 : 560;
    MoveWindow(wplayer, i, 86, wcl - 6 - i, 200, FALSE);
    SendMessage(wplayer, CB_SETDROPPEDWIDTH, 192, 0);
    MoveWindow(wquery, 370, 110, wcl - 99 - 370, 20, FALSE);
    MoveWindow(wgo, wcl - 93, 110, 87, 20, FALSE);
    MoveWindow(wlist, 8, 136, wcl - 16, 214, FALSE);
    GetWindowRect(wmain_, &wa);
    i = GetSystemMetrics(SM_CXSCREEN) - wcl;   /* keep it on the screen */
    SetWindowPos(wmain_, NULL, wa.left > i ? (i > 0 ? i : 0) : wa.left, wa.top, wcl,
                 compact ? H_COMPACT : H_CLIENT, SWP_NOZORDER);
    InvalidateRect(wmain_, NULL, TRUE);
}

static void deck_click(int x, int y)
{
    char cmd[64];
    POINT pt;
    status_t s;
    RECT r;
    EnterCriticalSection(&lock); s = st; LeaveCriticalSection(&lock);
    pt.x = x; pt.y = y;
    r = r_prog; InflateRect(&r, 0, 4);
    if (PtInRect(&r, pt) && s.duration > 0) {
        sprintf(cmd, "time %d", (int)(s.duration * (x - r_prog.left) / (r_prog.right - r_prog.left)));
        player_cmd(cmd);
        return;
    }
    r = r_vol; InflateRect(&r, 4, 2);
    if (PtInRect(&r, pt)) {
        int v = ((r_vol.bottom - y) * 100 + (r_vol.bottom - r_vol.top) / 2) / (r_vol.bottom - r_vol.top);
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        sprintf(cmd, "mixer volume %d", v);
        player_cmd(cmd);
        EnterCriticalSection(&lock); st.volume = v; LeaveCriticalSection(&lock);
        InvalidateRect(wmain_, &r_lcd, FALSE);
        return;
    }
    SetRect(&r, 186, 18, 276, 47);
    if (PtInRect(&r, pt)) {             /* time: elapsed <-> remaining */
        remain = !remain;
        WritePrivateProfileString("retrosqueeze", "remain", remain ? "1" : "0", ini);
        InvalidateRect(wmain_, &r_lcd, FALSE);
        return;
    }
    if (PtInRect(&r_mode[0], pt)) player_cmd(s.repeat == 1 ? "playlist repeat 0" : "playlist repeat 1");
    else if (PtInRect(&r_mode[1], pt)) player_cmd(s.repeat == 2 ? "playlist repeat 0" : "playlist repeat 2");
    else if (PtInRect(&r_mode[2], pt)) player_cmd(s.shuffle ? "playlist shuffle 0" : "playlist shuffle 1");
}

/* number keys: enabled for tracks that exist, bank follows the playing track */
static void update_numbers(status_t *s)
{
    int i;
    if (s->index != bank_index) { bank_index = s->index; bank = s->tracks ? s->index / 10 : 0; }
    if (bank * 10 >= s->tracks) bank = 0;
    for (i = 0; i < 10; i++) {
        EnableWindow(wnum[i], bank * 10 + i < s->tracks);
        InvalidateRect(wnum[i], NULL, FALSE);
    }
}

static void set_compact(int c)
{
    compact = c;
    SetWindowPos(wmain_, NULL, 0, 0, W_CLIENT, c ? H_COMPACT : H_CLIENT, SWP_NOMOVE | SWP_NOZORDER);
    WritePrivateProfileString("retrosqueeze", "compact", c ? "1" : "0", ini);
    InvalidateRect(wmain_, NULL, FALSE);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        static const char *tabs[NTABS] = { "Artists", "Albums", "Search", "Queue", "Radio", "Spotify", "Podcasts" };
        int i;
        wmain_ = w;
        /* left column: close (green lamp), fold the browser (bar), minimize (triangle) */
        child("BUTTON", "", BS_OWNERDRAW, ID_CLOSE, 3, 9, 42, 13);
        child("BUTTON", "", BS_OWNERDRAW, ID_FOLD, 3, 29, 42, 13);
        child("BUTTON", "", BS_OWNERDRAW, ID_MINI, 3, 49, 42, 13);
        /* transport, the original's two trays */
        wbtn[0] = child("BUTTON", "", BS_OWNERDRAW, ID_STOP, 628, 11, 42, 13);
        wbtn[1] = child("BUTTON", "", BS_OWNERDRAW, ID_PLAY, 670, 11, 42, 13);
        wbtn[2] = child("BUTTON", "", BS_OWNERDRAW, ID_PAUSE, 712, 11, 42, 13);
        wbtn[3] = child("BUTTON", "", BS_OWNERDRAW, ID_PREV, 586, 31, 42, 13);
        wbtn[4] = child("BUTTON", "", BS_OWNERDRAW, ID_REW, 628, 31, 42, 13);
        wbtn[5] = child("BUTTON", "", BS_OWNERDRAW, ID_FF, 670, 31, 42, 13);
        wbtn[6] = child("BUTTON", "", BS_OWNERDRAW, ID_NEXT, 712, 31, 42, 13);
        /* track number keys under the LCD */
        for (i = 0; i < 10; i++)
            wnum[i] = child("BUTTON", "", BS_OWNERDRAW, ID_NUM0 + i, 131 + i * 21, 63, 21, 13);
        /* the browser below (folds away) */
        for (i = 0; i < NTABS; i++)
            wtab[i] = child("BUTTON", tabs[i], BS_OWNERDRAW | WS_TABSTOP, ID_TAB0 + i, 8 + i * 78, 86, 76, 20);
        wplayer = child("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_PLAYER, 560, 86, 192, 200);
        wback = child("BUTTON", "< Back", BS_OWNERDRAW | WS_TABSTOP, ID_BACK, 304, 110, 60, 20);
        wquery = child("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_QUERY, 370, 110, 289, 20);
        wgo = child("BUTTON", "Find", BS_OWNERDRAW | WS_TABSTOP, ID_GO, 665, 110, 87, 20);
        wadd = child("BUTTON", "+ Queue", BS_OWNERDRAW | WS_TABSTOP, ID_ADD, 8, 356, 90, 20);
        wlist = CreateWindowEx(0, "LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                               LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NODATA | LBS_NOINTEGRALHEIGHT,
                               8, 136, 742, 214, w, (HMENU)ID_LIST, inst, NULL);
        SendMessage(wlist, LB_SETITEMHEIGHT, 0, 18);
        list_proc = (WNDPROC)SetWindowLong(wlist, GWL_WNDPROC, (LONG)list_sub);
        ShowWindow(wback, SW_HIDE);
        ShowWindow(wquery, SW_HIDE);
        ShowWindow(wgo, SW_HIDE);
        layout();
        SetTimer(w, 1, 100, NULL);
        return 0;
    }
    case WM_PAINT:
        paint(w);
        return 0;
    case WM_NCHITTEST: {                /* no title bar: the silver of the deck moves the window */
        LRESULT h = DefWindowProc(w, m, wp, lp);
        POINT p;
        p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
        ScreenToClient(w, &p);
        if (h == HTCLIENT && p.y < H_COMPACT && !PtInRect(&r_lcd, p)) return HTCAPTION;
        return h;
    }
    case WM_DISPLAYCHANGE:              /* new resolution: fit the deck again */
        layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER: {
        status_t s;
        EnterCriticalSection(&lock); s = st; LeaveCriticalSection(&lock);
        marquee++;
        {   /* repaint the title line only; the rest comes with WM_STATUS */
            RECT t;
            HDC dc;
            /* scrolling lines */
            if ((int)strlen(s.title) * 5 > DM_W || (int)strlen(s.artist) * 5 > DM_W || (int)strlen(s.album) * 5 + 35 > DM_W) {
                dc = GetDC(w);          /* straight to the screen: no full repaint, so it can run 10 times a second */
                dm_status(dc, &s);
                ReleaseDC(w, dc);
            }
            if (s.mode == 2 && marquee % 6 == 0) { SetRect(&t, 134, 11, 140, 17); InvalidateRect(w, &t, FALSE); }   /* blinking lamp */
        }
        return 0;
    }
    case WM_STATUS:
        if (cover_new) take_cover();
        InvalidateRect(w, &r_lcd, FALSE);
        InvalidateRect(w, &r_cover, FALSE);
        {
            status_t s;
            EnterCriticalSection(&lock); s = st; LeaveCriticalSection(&lock);
            update_numbers(&s);
        }
        return 0;
    case WM_PLAYERS:
        if (!same_players()) fill_players();
        return 0;
    case WM_PLUGINS:                    /* Spotty/Podcasts appeared or went away on the server */
        layout();
        if (!tab_on(tab)) open_tab(T_ARTISTS);
        return 0;
    case WM_REPLY: {
        char *r;
        int v;
        EnterCriticalSection(&lock);
        r = reply; reply = NULL; v = replyview;
        LeaveCriticalSection(&lock);
        if (r) {
            if (v == cur_view) fill_list(r, v);
            free(r);
        }
        { RECT c = { 8, 111, 300, 129 }; InvalidateRect(w, &c, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN:
        deck_click((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT *)lp)->itemHeight = 18;
        return TRUE;
    case WM_DRAWITEM:
        if (wp == ID_LIST) draw_item((DRAWITEMSTRUCT *)lp);
        else draw_button((DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:               /* black wells, cyan text, like the LCD */
        SetTextColor((HDC)wp, C_CYAN);
        SetBkColor((HDC)wp, C_LCD);
        return (LRESULT)b_list;
    case WM_CONTEXTMENU:
        if ((HWND)wp == wlist) {
            HMENU pm = CreatePopupMenu();
            int cmd;
            POINT p;
            p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
            {   /* select the row under the mouse first */
                POINT q = p;
                int i;
                ScreenToClient(wlist, &q);
                i = SendMessage(wlist, LB_ITEMFROMPOINT, 0, MAKELPARAM(q.x, q.y));
                if (HIWORD(i) == 0) SendMessage(wlist, LB_SETCURSEL, LOWORD(i), 0);
            }
            AppendMenu(pm, MF_STRING, 1, "Play now");
            AppendMenu(pm, MF_STRING, 2, "Add to queue");
            cmd = TrackPopupMenu(pm, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, w, NULL);
            DestroyMenu(pm);
            if (cmd) activate(cmd == 2 ? 1 : 2);
            return 0;
        }
        break;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        status_t s;
        EnterCriticalSection(&lock); s = st; LeaveCriticalSection(&lock);
        switch (id) {
        case ID_PREV: player_cmd("playlist index -1"); break;
        case ID_NEXT: player_cmd("playlist index +1"); break;
        case ID_STOP: player_cmd("stop"); break;
        case ID_PLAY:
            if (s.mode == 2) player_cmd("pause 0");
            else if (s.mode == 0) { player_cmd("power 1"); player_cmd("play"); }
            break;
        case ID_PAUSE: if (s.mode) player_cmd(s.mode == 1 ? "pause 1" : "pause 0"); break;
        case ID_REW: player_cmd("time -10"); break;
        case ID_FF: player_cmd("time +10"); break;
        case ID_CLOSE: PostMessage(w, WM_CLOSE, 0, 0); break;
        case ID_FOLD: set_compact(!compact); break;
        case ID_MINI: ShowWindow(w, SW_MINIMIZE); break;
        case ID_GO: do_find(); break;
        case ID_ADD: activate(1); break;
        case ID_BACK:
            if (nback) {
                nback--;
                lstrcpy(view_cmd, back_cmd[nback]);
                cur_view = back_view[nback];
                lstrcpy(crumb, nback ? "Back" : (tab == T_ARTISTS ? "All artists" : tab == T_ALBUMS ? "All albums" :
                                                 tab == T_RADIO ? "Radio" : tab == T_SPOTIFY ? "Spotify" :
                                                 tab == T_PODCAST ? "Podcasts" : "Search"));
                ShowWindow(wback, nback > 0 ? SW_SHOW : SW_HIDE);
                post_job(view_cmd, cur_view);
                SetFocus(wlist);
            }
            break;
        case ID_LIST:
            if (code == LBN_DBLCLK) activate(0);
            break;
        case ID_PLAYER:
            if (code == CBN_SELCHANGE) {
                int i = SendMessage(wplayer, CB_GETCURSEL, 0, 0);
                EnterCriticalSection(&lock);
                if (i >= 0 && i < nplayers) lstrcpy(cur_player, players[i].id);
                memset(&st, 0, sizeof st);
                LeaveCriticalSection(&lock);
                WritePrivateProfileString("retrosqueeze", "player", cur_player, ini);
                if (tab == T_QUEUE) open_tab(T_QUEUE);
                InvalidateRect(w, NULL, FALSE);
            }
            break;
        default:
            if (id >= ID_NUM0 && id < ID_NUM0 + 10) {   /* jump to that track of the queue */
                char cmd[40];
                sprintf(cmd, "playlist index %d", bank * 10 + id - ID_NUM0);
                player_cmd("power 1");
                player_cmd(cmd);
            }
            if (id >= ID_TAB0 && id < ID_TAB0 + NTABS) { open_tab(id - ID_TAB0); if (id - ID_TAB0 != T_SEARCH) SetFocus(wlist); }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(w, m, wp, lp);
}

/* ------------------------------------------------------- finding the server */

/* LMS answers a UDP broadcast on 3483 (what Squeezeboxes do): "e" + wanted tags,
   reply "E" + TLVs (4-char tag, 1 length byte, value).  Same subnet only. */
static int discover(void)
{
    static const char ask[] = "eIPAD\0NAME\0JSON\0CLIP\0";
    struct sockaddr_in a, from;
    unsigned char buf[600];
    SOCKET u;
    int tries, on = 1, got = 0;
    u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u == INVALID_SOCKET) return 0;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(3483);
    a.sin_addr.s_addr = INADDR_BROADCAST;
    for (tries = 0; tries < 3 && !got; tries++) {
        fd_set fs;
        struct timeval tv;
        sendto(u, ask, sizeof ask - 1, 0, (struct sockaddr *)&a, sizeof a);
        FD_ZERO(&fs);
        FD_SET(u, &fs);
        tv.tv_sec = 1; tv.tv_usec = 0;
        while (!got && select(0, &fs, NULL, NULL, &tv) > 0) {
            int fl = sizeof from, n = recvfrom(u, (char *)buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl), i;
            if (n < 1 || buf[0] != 'E') continue;
            lstrcpyn(lms_host, inet_ntoa(from.sin_addr), sizeof lms_host);
            lms_port = 9090;
            for (i = 1; i + 5 <= n && i + 5 + buf[i + 4] <= n; i += 5 + buf[i + 4]) {
                char v[64];
                int l = buf[i + 4] < 63 ? buf[i + 4] : 63;
                memcpy(v, buf + i + 5, l);
                v[l] = 0;
                if (!memcmp(buf + i, "JSON", 4)) web_port = atoi(v);
                else if (!memcmp(buf + i, "CLIP", 4) && atoi(v)) lms_port = atoi(v);
                else if (!memcmp(buf + i, "IPAD", 4) && v[0]) lstrcpyn(lms_host, v, sizeof lms_host);
            }
            got = 1;
        }
    }
    closesocket(u);
    return got;
}

/* talk to host:port with a timeout; returns the reply (up to n-1 bytes) or 0 bytes */
static int probe_talk(const char *host, int port, const char *req, char *buf, int n)
{
    struct sockaddr_in a;
    SOCKET t;
    int got = 0, k;
    if (!resolve(host, &a, port)) return 0;
    t = socket(AF_INET, SOCK_STREAM, 0);
    if (t == INVALID_SOCKET) return 0;
    if (!connect(t, (struct sockaddr *)&a, sizeof a) && send(t, req, strlen(req), 0) > 0) {
        for (;;) {
            fd_set fs;
            struct timeval tv;
            FD_ZERO(&fs);
            FD_SET(t, &fs);
            tv.tv_sec = 2; tv.tv_usec = 0;
            if (select(0, &fs, NULL, NULL, &tv) <= 0) break;
            k = recv(t, buf + got, n - 1 - got, 0);
            if (k <= 0) break;
            got += k;
            buf[got] = 0;
            if (got >= n - 1 || (req[0] != 'P' && strchr(buf, '\n'))) break;   /* CLI: one line is enough */
        }
    }
    closesocket(t);
    buf[got] = 0;
    return got;
}

/* is port the LMS CLI?  then also learn the web port from it */
static int probe_cli(const char *host, int port)
{
    char b[300];
    if (!probe_talk(host, port, "version ?\n", b, sizeof b) || strncmp(b, "version ", 8)) return 0;
    if (probe_talk(host, port, "pref httpport ?\n", b, sizeof b) && !strncmp(b, "pref httpport ", 14) && atoi(b + 14))
        web_port = atoi(b + 14);
    return 1;
}

/* is port the LMS web port?  returns its CLI port (JSON-RPC: pref plugin.cli:cliport) */
static int probe_web(const char *host, int port)
{
    static const char body[] = "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"\",[\"pref\",\"plugin.cli:cliport\",\"?\"]]}";
    char req[400], b[2000], *p;
    sprintf(req, "POST /jsonrpc.js HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
            host, (int)strlen(body), body);
    if (!probe_talk(host, port, req, b, sizeof b) || (p = strstr(b, "\"_p2\":\"")) == NULL) return 0;
    return atoi(p + 7);
}

/* host[:port] as typed: the port may be the CLI port or the web port (what the browser shows) */
static int probe_server(const char *host, int port)
{
    int cli;
    if (port) {
        if (probe_cli(host, port)) { lms_port = port; return 1; }
        if ((cli = probe_web(host, port)) != 0 && probe_cli(host, cli)) { lms_port = cli; web_port = port; return 1; }
        return 0;
    }
    if (probe_cli(host, 9090)) { lms_port = 9090; return 1; }
    if ((cli = probe_web(host, 9000)) != 0 && probe_cli(host, cli)) { lms_port = cli; web_port = 9000; return 1; }
    return 0;
}

#define IDC_SERVER 101

static BOOL CALLBACK server_dlg(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    char v[80];
    (void)lp;
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemText(d, IDC_SERVER, lms_host);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            char *p;
            int port = 0;
            HCURSOR old;
            GetDlgItemText(d, IDC_SERVER, v, sizeof v);
            for (p = v; *p == ' ' || *p == '\t'; p++) ;
            if (!strncmp(p, "http://", 7)) p += 7;          /* pasted from the browser */
            memmove(v, p, strlen(p) + 1);
            if ((p = strchr(v, '/')) != NULL) *p = 0;
            if ((p = strchr(v, ' ')) != NULL) *p = 0;
            if (!v[0]) return TRUE;
            if ((p = strchr(v, ':')) != NULL) { *p = 0; port = atoi(p + 1); }
            old = SetCursor(LoadCursor(NULL, IDC_WAIT));
            if (!probe_server(v, port)) {
                SetCursor(old);
                MessageBox(d, "No Lyrion Music Server found at that address.\n\n"
                              "Enter the address you open LMS with in the browser (e.g. 192.168.1.10:9000), "
                              "or the CLI port (default 9090).", "RetroSqueeze", MB_ICONEXCLAMATION);
                return TRUE;
            }
            SetCursor(old);
            lstrcpyn(lms_host, v, sizeof lms_host);
            EndDialog(d, 1);
        } else if (LOWORD(wp) == IDCANCEL)
            EndDialog(d, 0);
        return TRUE;
    }
    return FALSE;
}

/* no server in the INI: look for one, else ask; remembered in the INI */
static int find_server(void)
{
    char v[80];
    if (discover()) probe_cli(lms_host, lms_port);   /* web port from the server's own pref, if it answers */
    else if (DialogBox(inst, MAKEINTRESOURCE(100), NULL, server_dlg) != 1) return 0;
    sprintf(v, "%s:%d", lms_host, lms_port);
    WritePrivateProfileString("retrosqueeze", "lms", v, ini);
    sprintf(v, "%d", web_port);
    WritePrivateProfileString("retrosqueeze", "web", v, ini);
    return 1;
}

static HFONT font(const char *face, int h, int bold)
{
    return CreateFont(-h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, face);
}

int PASCAL WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASS wc;
    WSADATA wsa;
    MSG msg;
    RECT r;
    char *p, v[80];
    unsigned tid;
    HWND w;
    (void)prev; (void)cmdline;
    inst = hi;
    GetModuleFileName(NULL, ini, sizeof ini);
    if ((p = strrchr(ini, '\\')) != NULL) strcpy(p + 1, "RETROSQ.INI");
    GetPrivateProfileString("retrosqueeze", "lms", "", v, sizeof v, ini);
    if ((p = strchr(v, ':')) != NULL) { *p = 0; lms_port = atoi(p + 1); }
    lstrcpyn(lms_host, v, sizeof lms_host);
    web_port = GetPrivateProfileInt("retrosqueeze", "web", 9000, ini);   /* Lyrion default; the server tells the real one */
    GetPrivateProfileString("retrosqueeze", "player", "", cur_player, sizeof cur_player, ini);
    GetPrivateProfileString("retrosqueeze", "playername", "", own_name, sizeof own_name, ini);
    if (!own_name[0]) {                 /* first start: the player is called like this PC */
        DWORD n = sizeof own_name;
        if (!GetComputerName(own_name, &n) || !own_name[0]) lstrcpy(own_name, "RetroSqueeze");
        WritePrivateProfileString("retrosqueeze", "playername", own_name, ini);
    }
    GetPrivateProfileString("retrosqueeze", "playermac", "", own_mac, sizeof own_mac, ini);
    if (strlen(own_mac) != 17) {        /* first start: a locally administered MAC of our own */
        DWORD t = GetTickCount() ^ GetCurrentProcessId() << 8;
        sprintf(own_mac, "02:52:53:%02x:%02x:%02x", (int)(t >> 16) & 255, (int)(t >> 8) & 255, (int)t & 255);
        WritePrivateProfileString("retrosqueeze", "playermac", own_mac, ini);
    }

    if (WSAStartup(MAKEWORD(1, 1), &wsa)) {
        MessageBox(NULL, "Winsock is not available.", "RetroSqueeze", MB_ICONSTOP);
        return 1;
    }
    if (!lms_host[0] && !find_server()) { WSACleanup(); return 0; }
    InitializeCriticalSection(&lock);
    if (GetPrivateProfileInt("retrosqueeze", "localplayer", 1, ini)) {   /* this PC as a Squeezebox */
        unsigned v[6];
        unsigned char m[6];
        int i;
        sscanf(own_mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        for (i = 0; i < 6; i++) m[i] = (unsigned char)v[i];
        slimplay_start(lms_host, own_name, m);
    }
    jobevent = CreateEvent(NULL, FALSE, FALSE, NULL);
    f_ui = font("Arial", 13, 0);
    f_btn = font("Arial", 11, 0);
    f_btnb = font("Arial", 11, 1);
    f_label = font("Small Fonts", 9, 0);
    f_logo = font("Arial", 18, 1);
    f_logo2 = font("Arial", 12, 1);
    f_lcd_big = font("Arial", 12, 1);
    f_lcd = font("Arial", 11, 0);
    b_list = CreateSolidBrush(C_LCD);
    compact = GetPrivateProfileInt("retrosqueeze", "compact", 0, ini);
    remain = GetPrivateProfileInt("retrosqueeze", "remain", 0, ini);
    if (compact) r.bottom = H_COMPACT;
    skin_init();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "RetroSqueeze";
    RegisterClass(&wc);
    layout();
    SetRect(&r, 0, 0, W_CLIENT, H_CLIENT);
    /* no title bar, like the Creative players: a popup that is dragged by its silver */
    {   /* centred in the work area (above the taskbar) */
        RECT wa;
        int ww = r.right - r.left, wh = r.bottom - r.top;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
        w = CreateWindow("RetroSqueeze", "RetroSqueeze", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                         wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2 > 0 ?
                         wa.top + (wa.bottom - wa.top - wh) / 2 : wa.top, ww, wh, NULL, NULL, hi, NULL);
    }
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, status_thread, NULL, 0, &tid));
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, &tid));
    ShowWindow(w, show);
    UpdateWindow(w);
    open_tab(T_ARTISTS);
    SetFocus(wlist);

    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            if (msg.hwnd == wquery) { do_find(); SetFocus(wlist); continue; }
            if (msg.hwnd == wlist) { activate(0); continue; }
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_BACK && msg.hwnd == wlist) {
            SendMessage(w, WM_COMMAND, ID_BACK, 0);
            continue;
        }
        if (!IsDialogMessage(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    ShowWindow(w, SW_HIDE);
    slimplay_stop();                    /* sound card closed before the exit */
    ExitProcess(0);                     /* no WSACleanup: the other threads may still sit in a socket call */
    return 0;
}
