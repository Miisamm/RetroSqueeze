/* slimplay.c - SlimProto player core for RetroSqueeze: makes the PC a Squeezebox.

   Copyright (C) 2026 Sandro Miisamm.  GPL-3.0-or-later, see LICENSE.

   LMS sends uncompressed PCM (the player only advertises "pcm"), so there is no
   decoding at all: HTTP stream -> ring buffer -> sound card.  The protocol side
   follows squeezelite's slimproto.c (HELO deviceid 12, STAT events STMc/STMs/
   STMd/STMu/STMt/STMf/STMp/STMr, RESP, DSCO, SETD name).

   Builds for Win32 (Watcom, waveOut) and, for testing without Windows, for Linux
   (gcc, a silent output that "plays" in real time).  Big-endian on the wire. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "slimplay.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#define MINIMP3_P5                      /* the Pentium tuning of the synth (from PITMM) */
#include "minimp3.h"

#ifdef _WIN32
#include <windows.h>
#include <winsock.h>
#include <mmsystem.h>
typedef CRITICAL_SECTION mutex_t;
#define MUTEX_INIT(m) InitializeCriticalSection(&m)
#define LOCK(m) EnterCriticalSection(&m)
#define UNLOCK(m) LeaveCriticalSection(&m)
#define msleep(n) Sleep(n)
#define sock_close closesocket
static unsigned long now_ms(void) { return GetTickCount(); }
#include <process.h>
typedef unsigned (__stdcall *thread_fn)(void *);
#define THREAD(name) static unsigned __stdcall name(void *arg)
static void spawn(thread_fn f) { unsigned id; HANDLE h = (HANDLE)_beginthreadex(NULL, 0, f, NULL, 0, &id); if (h) CloseHandle(h); }
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
typedef pthread_mutex_t mutex_t;
#define MUTEX_INIT(m) pthread_mutex_init(&m, NULL)
#define LOCK(m) pthread_mutex_lock(&m)
#define UNLOCK(m) pthread_mutex_unlock(&m)
#define msleep(n) usleep((n) * 1000)
#define sock_close close
static unsigned long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000UL + t.tv_nsec / 1000000; }
typedef void *(*thread_fn)(void *);
#define THREAD(name) static void *name(void *arg)
static void spawn(thread_fn f) { pthread_t t; pthread_create(&t, NULL, f, NULL); pthread_detach(t); }
#endif

#define RING (2 * 1024 * 1024)          /* ~11.9 s of 44.1 kHz 16-bit stereo */

static mutex_t mx;
static char server[64], name[64], logline[160];
static unsigned char mac[6];
static unsigned long server_ip;         /* network order */
static SOCKET ctl = INVALID_SOCKET;     /* the SlimProto connection */
static int volume_l = 0x10000, volume_r = 0x10000;   /* 16.16 */

/* ring buffer + stream + output state, all under mx */
static unsigned char *ring;
static unsigned long w_total, r_total;  /* bytes written into / taken out of the ring (ever) */
static int stream_state;                /* 0 idle, 1 connecting, 2 streaming, 3 done (EOF) */
static unsigned long stream_bytes;
static char stream_req[1024];
static int stream_req_len, stream_gen; /* bump stream_gen to abort a running stream */
static unsigned long stream_ip;
static unsigned short stream_port;
static int fmt_rate = 44100, fmt_ch = 2, fmt_bits = 16, fmt_le = 1;
static volatile int quitting, out_done; /* slimplay_stop(): close the sound card before the process exits */
static int autostart, out_state;        /* out_state: 0 stopped, 1 waiting for data, 2 running, 3 paused */
static unsigned long track_start_at;    /* r_total offset where the current/next track begins */
static int track_pending;               /* next track's first byte not yet played */
static unsigned long played_base;       /* r_total-domain bytes played at the current track start */
static unsigned long played;            /* bytes actually played by the device (r_total domain) */
static unsigned long threshold;
static int ev_STMs, ev_STMd, ev_STMu, ev_STMl, ev_RESP, ev_DSCO, ev_META, sent_STMu;
static int stream_fmt, wait_cont, cont_seen;   /* 'p' pcm / 'm' mp3; radio waits for "cont" */
static unsigned long cont_metaint;
static unsigned char meta_buf[4096];
static int meta_len;
static char resp_hdr[1024];
static int resp_len;
static unsigned long last_stmt;
static int connected_ok;

static void say(const char *s) { LOCK(mx); strncpy(logline, s, sizeof logline - 1); UNLOCK(mx); }

/* --------------------------------------------------------------- wire */

static void put32(unsigned char *p, unsigned long v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static unsigned long get32(const unsigned char *p) { return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }

static mutex_t sendmx;
static int send_all(SOCKET s, const void *buf, int n)
{
    const char *p = buf;
    while (n > 0) {
        int k = send(s, p, n, 0);
        if (k <= 0) return 0;
        p += k; n -= k;
    }
    return 1;
}

static void send_pkt(const char *op, const void *body, int n, const void *extra, int n2)
{
    unsigned char h[8];
    memcpy(h, op, 4);
    put32(h + 4, n + n2);
    LOCK(sendmx);
    if (ctl != INVALID_SOCKET) {
        send_all(ctl, h, 8);
        if (n) send_all(ctl, body, n);
        if (n2) send_all(ctl, extra, n2);
    }
    UNLOCK(sendmx);
}

static void send_helo(int reconnect)
{
    static const char caps[] = "Model=squeezelite,ModelName=RetroSqueeze,AccuratePlayPoints=1,HasDigitalOut=0,"
                               "Firmware=v0.1,MaxSampleRate=44100,pcm,mp3";
    unsigned char b[36];
    memset(b, 0, sizeof b);
    b[0] = 12;                          /* deviceid: squeezeplay */
    b[1] = 0;                           /* revision */
    memcpy(b + 2, mac, 6);
    /* b+8: uuid[16] = 0 */
    put16(b + 24, reconnect ? 0x4000 : 0);
    put32(b + 26, 0);
    LOCK(mx); put32(b + 30, stream_bytes); UNLOCK(mx);   /* bytes_received_L */
    /* lang "EN" */
    {
        unsigned char full[38];
        memcpy(full, b, 34);
        full[34] = 'E'; full[35] = 'N';
        send_pkt("HELO", full, 36, caps, sizeof caps - 1);
    }
}

static void send_stat(const char *ev, unsigned long server_ts_raw)
{
    unsigned char b[53];
    unsigned long ms = 0, now = now_ms(), full, outfull;
    LOCK(mx);
    if (played > played_base)
        ms = (unsigned long)((double)(played - played_base) * 1000.0 / (fmt_rate * fmt_ch * 2));
    full = w_total - r_total;
    outfull = r_total - played;
    memset(b, 0, sizeof b);
    memcpy(b, ev, 4);
    put32(b + 7, RING);
    put32(b + 11, full);
    put32(b + 15, 0);
    put32(b + 19, stream_bytes);
    put16(b + 23, 0xffff);
    put32(b + 25, now);
    put32(b + 29, 65536);
    put32(b + 33, outfull);
    put32(b + 37, ms / 1000);
    put32(b + 43, ms);
    memcpy(b + 47, &server_ts_raw, 4); /* echoed as received */
#if defined(SLIMPLAY_TEST)
    if (getenv("SLIMPLAY_DEBUG")) fprintf(stderr, "%lu STAT %.4s ms=%lu played=%lu base=%lu r=%lu w=%lu out=%d pend=%d\n",
                                          now % 100000, ev, ms, played, played_base, r_total, w_total, out_state, track_pending);
#endif
    UNLOCK(mx);
    send_pkt("STAT", b, sizeof b, NULL, 0);
}

static void send_name(void)
{
    unsigned char id = 0;
    send_pkt("SETD", &id, 1, name, strlen(name) + 1);
}

/* ------------------------------------------------------------ audio out */

static int out_open(void);
static void out_close(void);
static void out_pause(int on);
static unsigned long out_position(void);   /* bytes played since out_open */
static void out_reset(void);
static int out_free_slot(void);
static void out_submit(const unsigned char *pcm, int n);

/* copy from the ring with the LMS volume applied (16-bit samples) */
static int take(unsigned char *dst, int max)
{
    int n, i, k;
    LOCK(mx);
    n = (int)(w_total - r_total);
    if (n > max) n = max;
    /* don't run into the start of the next track if its format differs (not handled) */
    n &= ~3;
    for (k = 0; k < n; ) {
        unsigned long off = (r_total + k) % RING;
        int chunk = n - k;
        if (off + chunk > RING) chunk = RING - off;
        memcpy(dst + k, ring + off, chunk);
        k += chunk;
    }
    r_total += n;
    UNLOCK(mx);
    if (volume_l != 0x10000 || volume_r != 0x10000) {
        short *s = (short *)dst;
        for (i = 0; i < n / 2; i += 2) {
            s[i] = (short)(((long)s[i] * volume_l) >> 16);
            s[i + 1] = (short)(((long)s[i + 1] * volume_r) >> 16);
        }
    }
    return n;
}

#define CHUNK 8820                      /* 50 ms of 44.1 kHz stereo 16-bit */

THREAD(output_thread)
{
    static unsigned char buf[CHUNK];
    unsigned long base = 0;             /* r_total value when the device was (re)opened */
    int open = 0, paused = 0;
    (void)arg;
    for (;;) {
        int state;
        if (quitting) {                 /* exiting: release the device while everything is still loaded */
            if (open) out_close();
            out_done = 1;
            return 0;
        }
        LOCK(mx);
        state = out_state;
        UNLOCK(mx);
        if (state == 0) {               /* stopped: drop the device */
            if (open) { out_close(); open = 0; }
            msleep(20);
            continue;
        }
        if (!open) {
            if (!out_open()) { say("can't open the sound card"); msleep(500); continue; }
            open = 1;
            paused = 0;
            LOCK(mx); base = r_total; played = r_total; UNLOCK(mx);
        }
        if (state == 1) {               /* buffering until the threshold */
            int go;
            LOCK(mx);
            go = w_total - r_total >= threshold || stream_state == 3;
            if (go && autostart) out_state = state = 2;
            else if (go && !ev_STMl) { ev_STMl = 1; }
            UNLOCK(mx);
            if (state != 2) { msleep(20); continue; }
        }
        if (state == 3) { if (!paused) { out_pause(1); paused = 1; } msleep(20); }
        else {
            if (paused) { out_pause(0); paused = 0; }
            while (out_free_slot()) {
                int n = take(buf, CHUNK);
                if (!n) break;
                out_submit(buf, n);
            }
            msleep(10);
        }
        /* bookkeeping: what has really been played */
        LOCK(mx);
        played = base + out_position();
        if (played > r_total) played = r_total;
        if (track_pending && out_state == 2 && played >= track_start_at) {
            track_pending = 0;
            played_base = track_start_at;
            ev_STMs = 1;
        }
        if (out_state == 2 && stream_state == 3 && w_total == r_total && played >= r_total && !sent_STMu) {
            ev_STMu = sent_STMu = 1;    /* everything played, nothing more coming */
            out_state = 0;
        }
        UNLOCK(mx);
    }
}

#ifdef _WIN32
/* ---- waveOut: 12 x 50 ms buffers ---- */
#define NBUF 12
static HWAVEOUT wo;
static WAVEHDR hdr[NBUF];
static unsigned char wbuf[NBUF][CHUNK];
static int wnext;
static int out_open(void)
{
    WAVEFORMATEX f;
    int i;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = (WORD)fmt_ch;
    f.nSamplesPerSec = fmt_rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = (WORD)(fmt_ch * 2);
    f.nAvgBytesPerSec = fmt_rate * f.nBlockAlign;
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return 0;
    for (i = 0; i < NBUF; i++) {
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (char *)wbuf[i];
        hdr[i].dwBufferLength = CHUNK;
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        hdr[i].dwFlags |= WHDR_DONE;
    }
    wnext = 0;
    return 1;
}
static void out_close(void)
{
    int i;
    waveOutReset(wo);
    for (i = 0; i < NBUF; i++) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
    waveOutClose(wo);
}
static void out_reset(void) { waveOutReset(wo); }
static void out_pause(int on) { if (on) waveOutPause(wo); else waveOutRestart(wo); }
static unsigned long out_position(void)
{
    MMTIME t;
    t.wType = TIME_BYTES;
    waveOutGetPosition(wo, &t, sizeof t);
    return t.wType == TIME_BYTES ? t.u.cb : t.wType == TIME_SAMPLES ? t.u.sample * fmt_ch * 2 : 0;
}
static int out_free_slot(void) { return (hdr[wnext].dwFlags & WHDR_DONE) != 0; }
static void out_submit(const unsigned char *pcm, int n)
{
    WAVEHDR *h = &hdr[wnext];
    memcpy(h->lpData, pcm, n);
    h->dwBufferLength = n;
    h->dwFlags &= ~WHDR_DONE;
    waveOutWrite(wo, h, sizeof *h);
    wnext = (wnext + 1) % NBUF;
}
#else
/* ---- test output: plays silently in real time, 600 ms "device" buffer ---- */
static unsigned long t_open, t_paused_at, t_paused_total, t_submitted;
static int t_paused;
static FILE *t_dump;
static int out_open(void) { t_open = now_ms(); t_paused = 0; t_paused_total = 0; t_submitted = 0; if (getenv("SLIMPLAY_DUMP")) t_dump = fopen(getenv("SLIMPLAY_DUMP"), "ab"); return 1; }
static void out_close(void) { if (t_dump) fclose(t_dump); t_dump = NULL; }
static void out_reset(void) { t_open = now_ms(); t_submitted = 0; t_paused_total = 0; }
static void out_pause(int on)
{
    if (on && !t_paused) { t_paused = 1; t_paused_at = now_ms(); }
    if (!on && t_paused) { t_paused = 0; t_paused_total += now_ms() - t_paused_at; }
}
static unsigned long out_position(void)
{
    unsigned long t = (t_paused ? t_paused_at : now_ms()) - t_open - t_paused_total;
    unsigned long p = (unsigned long)((double)t * fmt_rate * fmt_ch * 2 / 1000.0);
    return p < t_submitted ? p : t_submitted;
}
static int out_free_slot(void) { return t_submitted - out_position() < 600UL * fmt_rate * fmt_ch * 2 / 1000; }
static void out_submit(const unsigned char *pcm, int n) { if (t_dump) fwrite(pcm, 1, n, t_dump); t_submitted += n; }
#endif

/* ------------------------------------------------------------ streaming */

/* LMS may send 8/24/32-bit or big-endian PCM (or a WAV file): make it 16-bit LE in
   place.  Partial samples are carried over to the next chunk. */
static unsigned char carry[8];
static int carry_n;
static int to16(unsigned char *b, int n, int *first)
{
    static unsigned char tmp[16384 + 8];
    int bytes = fmt_bits / 8, i, o = 0, total, skip = 0;
    if (*first) {
        *first = 0;
        if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {   /* WAV: use its format, skip to data */
            int p = 12;
            while (p + 8 <= n) {
                unsigned long sz = b[p + 4] | (b[p + 5] << 8) | ((unsigned long)b[p + 6] << 16) | ((unsigned long)b[p + 7] << 24);
                if (!memcmp(b + p, "fmt ", 4) && p + 24 <= n) {
                    LOCK(mx);
                    fmt_ch = b[p + 10];
                    fmt_rate = b[p + 12] | (b[p + 13] << 8) | ((long)b[p + 14] << 16);
                    fmt_bits = b[p + 22];
                    fmt_le = 1;
                    UNLOCK(mx);
                    bytes = fmt_bits / 8;
                }
                if (!memcmp(b + p, "data", 4)) { skip = p + 8; break; }
                p += 8 + (int)sz;
            }
        }
    }
    if (bytes == 2 && fmt_le && !carry_n) return n - skip ? (memmove(b, b + skip, n - skip), n - skip) : 0;
    memcpy(tmp, carry, carry_n);
    memcpy(tmp + carry_n, b + skip, n - skip);
    total = carry_n + n - skip;
    for (i = 0; i + bytes <= total; i += bytes) {
        int hi, lo;                     /* keep the top 16 bits */
        if (bytes == 1) { hi = (unsigned char)(tmp[i] - 128); lo = 0; }
        else if (fmt_le) { hi = tmp[i + bytes - 1]; lo = tmp[i + bytes - 2]; }
        else { hi = tmp[i]; lo = tmp[i + 1]; }
        b[o++] = (unsigned char)lo;
        b[o++] = (unsigned char)hi;
    }
    carry_n = total - i;
    memcpy(carry, tmp + i, carry_n);
    return o;
}

/* append 16-bit PCM to the ring, waiting for room; 0 if the stream was aborted */
static int ring_write(const unsigned char *pcm, int n, int gen)
{
    while (n > 0) {
        int room, chunk;
        unsigned long off;
        LOCK(mx);
        if (gen != stream_gen) { UNLOCK(mx); return 0; }
        room = RING - (int)(w_total - r_total);
        if (room < 4) { UNLOCK(mx); msleep(20); continue; }
        chunk = n < room ? n : room;
        off = w_total % RING;
        if (off + chunk > RING) chunk = RING - off;
        memcpy(ring + off, pcm, chunk);
        w_total += chunk;
        UNLOCK(mx);
        pcm += chunk;
        n -= chunk;
    }
    return 1;
}

/* MP3 (format 'm'): radio and the library's MP3s - LMS has no mp3->pcm rule, so we
   decode here with minimp3 (the same Pentium-tuned copy as in PITMM). */
static mp3dec_t mp3d;
static unsigned char mbuf[16384];
static int mlen;
static short mpcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

static int mp3_feed(const unsigned char *b, int n, int gen, int eof)
{
    while (n > 0 || eof) {
        int k = n < (int)sizeof mbuf - mlen ? n : (int)sizeof mbuf - mlen, used = 0;
        memcpy(mbuf + mlen, b, k);
        mlen += k; b += k; n -= k;
        /* decode whole frames; keep a little in hand unless the stream has ended */
        while (mlen > (eof && n == 0 ? 0 : 2048)) {
            mp3dec_frame_info_t fi;
            int smp = mp3dec_decode_frame(&mp3d, mbuf + used, mlen - used, mpcm, &fi);
            if (!fi.frame_bytes) { if (eof && n == 0) mlen = used; break; }   /* need more data */
            used += fi.frame_bytes;
            if (smp) {
                LOCK(mx);
                if (fi.hz && (fi.hz != fmt_rate || fi.channels != fmt_ch) && out_state <= 1) { fmt_rate = fi.hz; fmt_ch = fi.channels; }
                UNLOCK(mx);
                if (!ring_write((unsigned char *)mpcm, smp * fi.channels * 2, gen)) return 0;
            }
            if (mlen - used < 2048 && !(eof && n == 0)) break;
        }
        memmove(mbuf, mbuf + used, mlen - used);
        mlen -= used;
        if (eof && n == 0) break;
    }
    return 1;
}

THREAD(stream_thread)
{
    (void)arg;
    for (;;) {
        int gen, n, hdr_done = 0, hlen = 0, first = 1, mp3, meta_left, meta_skip = 0, eof = 0;
        unsigned long metaint;
        struct sockaddr_in a;
        SOCKET s;
        char req[1024], h[1024];
        static unsigned char buf[16384], meta[4096];
        int metan = 0;
        LOCK(mx);
        if (stream_state != 1) { UNLOCK(mx); msleep(20); continue; }
        gen = stream_gen;
        memcpy(req, stream_req, stream_req_len);
        n = stream_req_len;
        mp3 = stream_fmt == 'm';
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = stream_ip;
        a.sin_port = stream_port;
        UNLOCK(mx);
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&a, sizeof a) || !send_all(s, req, n)) {
            if (s != INVALID_SOCKET) sock_close(s);
            LOCK(mx); if (gen == stream_gen) { stream_state = 0; ev_DSCO = 2; } UNLOCK(mx);   /* 2 = connect failed */
            continue;
        }
        LOCK(mx); if (gen == stream_gen) stream_state = 2; UNLOCK(mx);
        carry_n = 0;
        mlen = 0;
        mp3dec_init(&mp3d);
        metaint = 0;
        meta_left = 0;
        for (;;) {
            int k, aborted, want;
            LOCK(mx);
            aborted = gen != stream_gen;
            UNLOCK(mx);
            if (aborted) break;
            if (hdr_done && wait_cont) {    /* radio: LMS sends "cont" (metadata interval) first */
                LOCK(mx);
                if (cont_seen) { wait_cont = 0; metaint = cont_metaint; meta_left = (int)metaint; }
                UNLOCK(mx);
                if (wait_cont) { msleep(20); continue; }
            }
            want = !hdr_done ? 1 : sizeof buf;
            if (hdr_done && metaint && meta_skip == 0 && meta_left < want) want = meta_left ? meta_left : 1;
            if (meta_skip && meta_skip < want) want = meta_skip;
            {   /* don't block forever: a stop has to be able to end this */
                fd_set rd;
                struct timeval tv;
                FD_ZERO(&rd);
                FD_SET(s, &rd);
                tv.tv_sec = 0; tv.tv_usec = 200000;
                if (select((int)s + 1, &rd, NULL, NULL, &tv) <= 0) continue;
            }
            k = recv(s, (char *)buf, want, 0);
            if (k <= 0) { eof = 1; break; }
            if (!hdr_done) {
                if (hlen < (int)sizeof h - 1) h[hlen++] = buf[0];
                if (hlen >= 4 && !memcmp(h + hlen - 4, "\r\n\r\n", 4)) {
                    hdr_done = 1;
                    LOCK(mx);
                    memcpy(resp_hdr, h, hlen); resp_len = hlen; ev_RESP = 1;
                    UNLOCK(mx);
                }
                continue;
            }
            if (meta_skip) {                /* inside an ICY metadata block: collect it for LMS */
                if (metan + k <= (int)sizeof meta) { memcpy(meta + metan, buf, k); metan += k; }
                meta_skip -= k;
                if (!meta_skip) {
                    LOCK(mx);
                    if (metan < (int)sizeof meta_buf) { memcpy(meta_buf, meta, metan); meta_len = metan; ev_META = 1; }
                    UNLOCK(mx);
                    meta_left = (int)metaint;
                }
                continue;
            }
            if (metaint && meta_left == 0) {   /* the length byte of the next metadata block */
                meta_skip = buf[0] * 16;
                metan = 0;
                if (!meta_skip) meta_left = (int)metaint;
                continue;
            }
            if (metaint) meta_left -= k;
            LOCK(mx); stream_bytes += k; UNLOCK(mx);
            if (mp3) { if (!mp3_feed(buf, k, gen, 0)) break; }
            else {
                k = to16(buf, k, &first);
                if (k && !ring_write(buf, k, gen)) break;
            }
        }
        if (eof) {                          /* end of the stream: finish decoding, tell LMS */
            if (mp3) mp3_feed(buf, 0, gen, 1);
            LOCK(mx);
            if (gen == stream_gen) { stream_state = 3; ev_STMd = 1; ev_DSCO = 1; }
            UNLOCK(mx);
        }
        sock_close(s);
    }
}

/* -------------------------------------------------------- server frames */

static void flush_all(void)
{
    LOCK(mx);
    stream_gen++;
    stream_state = 0;
    out_state = 0;
    r_total = w_total;                  /* drop buffered audio */
    played = played_base = r_total;
    track_pending = 0;
    ev_STMs = ev_STMd = ev_STMu = ev_STMl = 0;
    UNLOCK(mx);
}

static void on_strm(const unsigned char *p, int len)
{
    char cmd = p[4];
#if defined(SLIMPLAY_TEST)
    if (getenv("SLIMPLAY_DEBUG") && cmd != 't') fprintf(stderr, "%lu strm %c autostart=%c fmt=%c\n", now_ms() % 100000, cmd, p[5], p[6]);
#endif
    unsigned long rg = get32(p + 18), rg_raw;
    memcpy(&rg_raw, p + 18, 4);
    switch (cmd) {
    case 't':
        send_stat("STMt", rg_raw);
        break;
    case 'q':
        flush_all();
        send_stat("STMf", 0);
        break;
    case 'f':
        flush_all();
        send_stat("STMf", 0);
        break;
    case 'p':
        LOCK(mx); if (out_state == 2) out_state = 3; UNLOCK(mx);
        if (!rg) send_stat("STMp", 0);
        else {                          /* pause for rg ms (sync); resume afterwards */
            msleep(rg);
            LOCK(mx); if (out_state == 3) out_state = 2; UNLOCK(mx);
        }
        break;
    case 'u':
        LOCK(mx); if (out_state == 3 || out_state == 1) out_state = 2; UNLOCK(mx);
        send_stat("STMr", 0);
        break;
    case 'a':                           /* skip ahead rg ms (sync) - drop that much audio */
        LOCK(mx);
        {
            unsigned long n = (unsigned long)((double)rg * fmt_rate * fmt_ch * 2 / 1000) & ~3UL;
            if (n > w_total - r_total) n = w_total - r_total;
            r_total += n;
        }
        UNLOCK(mx);
        break;
    case 's': {
        static const int rates[] = { 11025, 22050, 32000, 44100, 48000, 8000, 12000, 16000, 24000, 96000 };
        int hl = len - 28;
        send_stat("STMf", 0);
        if (p[6] != 'p' && p[6] != 'm') { send_stat("STMn", 0); break; }   /* PCM, or MP3 decoded here */
        LOCK(mx);
        stream_fmt = p[6];
        wait_cont = p[5] >= '2';        /* autostart 2/3: wait for "cont" before reading the body */
        cont_seen = 0;
        if (p[7] >= '0' && p[7] <= '3') fmt_bits = (p[7] - '0' + 1) * 8;
        if (p[8] >= '0' && p[8] <= '9') fmt_rate = rates[p[8] - '0'];
        if (p[9] == '1' || p[9] == '2') fmt_ch = p[9] - '0';
        fmt_le = p[10] == '1';
        autostart = (p[5] - '0') & 1;   /* 0/2 = wait for "u", 1/3 = start by itself */
        if (stream_fmt == 'm') { fmt_bits = 16; fmt_le = 1; }
        threshold = (unsigned long)p[11] * 1024;
        if (threshold < 65536) threshold = 65536;
        stream_gen++;
        memcpy(&stream_ip, p + 24, 4);
        if (!stream_ip) stream_ip = server_ip;
        memcpy(&stream_port, p + 22, 2);
        if (hl > (int)sizeof stream_req) hl = sizeof stream_req;
        memcpy(stream_req, p + 28, hl);
        stream_req_len = hl;
        stream_state = 1;
        stream_bytes = 0;
        /* gapless: the new track starts where the buffered audio ends */
        track_start_at = w_total;
        track_pending = 1;
        sent_STMu = 0;
        ev_STMl = 0;
        if (out_state == 0) out_state = 1;
        UNLOCK(mx);
        send_stat("STMc", 0);
        break;
    }
    }
}

/* LMS sends squeezelite-type players its SqueezePlay curve (74 dB range, 50% = -25 dB), made for
   a hi-fi amp behind the player; on a PC sound card half volume is nearly silent.  Turn the gain
   back into LMS's 0..100 and use an audio-taper curve instead: (v/100)^2, 50% = -12 dB. */
static int regain(unsigned long g)
{
    double db, v;
    if (g == 0) return 0;
    db = 20.0 * log10(g / 65536.0);
    v = db > -37.0 ? 100.0 + db * 75.0 / 37.0 : (db + 74.0) * 25.0 / 37.0;
    if (v <= 0) return 0;
    if (v >= 100) return 0x10000;
    return (int)(65536.0 * v * v / 10000.0);
}

static void on_frame(const unsigned char *p, int len)
{
    if (len < 4) return;
    if (!memcmp(p, "strm", 4) && len >= 28) on_strm(p, len);
    else if (!memcmp(p, "cont", 4) && len >= 8) {
        LOCK(mx); cont_metaint = get32(p + 4); cont_seen = 1; UNLOCK(mx);
    } else if (!memcmp(p, "audg", 4) && len >= 22) {
        int adjust = p[12];
        volume_l = adjust ? regain(get32(p + 14)) : 0x10000;
        volume_r = adjust ? regain(get32(p + 18)) : 0x10000;
    } else if (!memcmp(p, "setd", 4) && len >= 5 && p[4] == 0) {
        if (len > 5) { LOCK(mx); strncpy(name, (const char *)p + 5, sizeof name - 1); UNLOCK(mx); }
        send_name();
    }
    /* aude, vers, serv, cont, codc, grf*: not needed for a PCM player */
}

THREAD(control_thread)
{
    int reconnect = 0;
    (void)arg;
    for (;;) {
        struct sockaddr_in a;
        struct hostent *he;
        SOCKET s;
        unsigned char hb[2];
        static unsigned char frame[4096];
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(3483);
        a.sin_addr.s_addr = inet_addr(server);
        if (a.sin_addr.s_addr == INADDR_NONE && (he = gethostbyname(server)) != NULL) memcpy(&a.sin_addr, he->h_addr, 4);
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&a, sizeof a)) {
            if (s != INVALID_SOCKET) sock_close(s);
            say("can't reach the music server, retrying");
            msleep(5000);
            continue;
        }
        server_ip = a.sin_addr.s_addr;
        LOCK(sendmx); ctl = s; UNLOCK(sendmx);
        send_helo(reconnect);
        reconnect = 1;
        LOCK(mx); connected_ok = 1; UNLOCK(mx);
        say("connected");
        for (;;) {
            fd_set rd;
            struct timeval tv;
            int r, need, got;
            /* events from the stream/output threads */
            {
                int s1, d, u, l, rs, ds, me;
                static char hdr[4096];
                int hl = 0;
                LOCK(mx);
                s1 = ev_STMs; d = ev_STMd; u = ev_STMu; l = ev_STMl == 1; rs = ev_RESP; ds = ev_DSCO; me = ev_META;
                ev_STMs = ev_STMd = ev_STMu = ev_RESP = ev_DSCO = ev_META = 0;
                if (me && !rs) { memcpy(hdr, meta_buf, meta_len); hl = meta_len; }
                if (l) ev_STMl = 2;
                if (rs) { memcpy(hdr, resp_hdr, resp_len); hl = resp_len; }
                UNLOCK(mx);
                if (rs) send_pkt("RESP", hdr, hl, NULL, 0);
                if (me && !rs) send_pkt("META", hdr, hl, NULL, 0);
                if (ds) { unsigned char reason = (unsigned char)(ds == 1 ? 0 : 1); send_pkt("DSCO", &reason, 1, NULL, 0); }
                if (s1) send_stat("STMs", 0);
                if (d) send_stat("STMd", 0);
                if (l) send_stat("STMl", 0);
                if (u) send_stat("STMu", 0);
                {
                    int running;
                    LOCK(mx); running = out_state == 2; UNLOCK(mx);
                    if (running && now_ms() - last_stmt > 1000) { last_stmt = now_ms(); send_stat("STMt", 0); }
                }
            }
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            tv.tv_sec = 0; tv.tv_usec = 100000;
            r = select((int)s + 1, &rd, NULL, NULL, &tv);
            if (r < 0) break;
            if (r == 0) continue;
            if (recv(s, (char *)hb, 1, 0) != 1 || recv(s, (char *)hb + 1, 1, 0) != 1) break;
            need = (hb[0] << 8) | hb[1];
            if (need > (int)sizeof frame) need = sizeof frame;   /* never happens for the frames we use */
            for (got = 0; got < need; ) {
                int k = recv(s, (char *)frame + got, need - got, 0);
                if (k <= 0) break;
                got += k;
            }
            if (got < need) break;
            on_frame(frame, need);
        }
        LOCK(sendmx); ctl = INVALID_SOCKET; UNLOCK(sendmx);
        sock_close(s);
        LOCK(mx); connected_ok = 0; UNLOCK(mx);
        say("lost the music server, reconnecting");
        msleep(2000);
    }
}

/* ---------------------------------------------------------------- API */

/* before exit: stop the sound output and drop the server connection (Win98 crashed exiting with a wave device open) */
void slimplay_stop(void)
{
    int i;
    quitting = 1;
    LOCK(mx); stream_gen++; UNLOCK(mx);
    for (i = 0; i < 100 && ring && !out_done; i++) msleep(10);
    LOCK(sendmx);
    if (ctl != INVALID_SOCKET) { shutdown(ctl, 2); sock_close(ctl); ctl = INVALID_SOCKET; }
    UNLOCK(sendmx);
}

int slimplay_start(const char *srv, const char *player_name, const unsigned char player_mac[6])
{
    static int started;
    if (started) return 1;
    ring = malloc(RING);
    if (!ring) return 0;
    started = 1;
    MUTEX_INIT(mx);
    MUTEX_INIT(sendmx);
    strncpy(server, srv, sizeof server - 1);
    strncpy(name, player_name, sizeof name - 1);
    memcpy(mac, player_mac, 6);
    spawn(control_thread);
    spawn(stream_thread);
    spawn(output_thread);
    return 1;
}

void slimplay_info(int *connected, int *state, char *msg, int msglen)
{
    LOCK(mx);
    if (connected) *connected = connected_ok;
    if (state) *state = out_state;
    if (msg) { strncpy(msg, logline, msglen - 1); msg[msglen - 1] = 0; }
    UNLOCK(mx);
}

int slimplay_format(int *rate)
{
    int f;
    LOCK(mx);
    f = out_state ? stream_fmt : 0;
    if (rate) *rate = fmt_rate;
    UNLOCK(mx);
    return f;
}

#ifdef SLIMPLAY_TEST
/* Linux test harness: ./slimplay <server> <name> <mac> */
int main(int argc, char **argv)
{
    unsigned char m[6];
    unsigned v[6];
    unsigned long last_r = 0;
    if (argc < 4 || sscanf(argv[3], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        printf("usage: slimplay server name mac\n");
        return 1;
    }
    { int i; for (i = 0; i < 6; i++) m[i] = (unsigned char)v[i]; }
    slimplay_start(argv[1], argv[2], m);
    for (;;) {
        int c, st;
        char msg[160];
        sleep(2);
        slimplay_info(&c, &st, msg, sizeof msg);
        LOCK(mx);
        printf("%lu conn=%d out=%d stream=%d rate=%d bytes=%lu ring=%lu played=%lus vol=%d %s\n", now_ms() / 1000, c, st, stream_state,
               fmt_rate, stream_bytes, w_total - r_total, played > played_base ? (played - played_base) / (fmt_rate * fmt_ch * 2) : 0,
               volume_l * 100 / 65536, msg);
        last_r = r_total;
        UNLOCK(mx);
        fflush(stdout);
    }
    (void)last_r;
    return 0;
}
#endif
