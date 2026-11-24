/*
 * aes67rx - AES67-2023 vevo.
 *
 *   sdp.c     SDP (RFC 8866, RFC 7273), SAP felderites (RFC 2974)     AES67 8., 9., E.2
 *   ptp.c     PTP timeReceiver (ptp4l, szoftveres/hardveres), media ora  4., 5., A
 *   rx.c      unicast/multicast RTP vetel, IGMP/SSM, RTCP               6., 10.2
 *   jitter.c  media ora szerint cimzett jitter puffer                   5., 7.2, 7.5
 *   audio.c   ALSA lejatszas allando link offsettel                     7.4
 *   main.c    vezerles, kapcsolodasi dontes (8.2), allapotsor
 */
#ifndef AES67_H
#define AES67_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* ---- SDP / SAP ---- */

#define MAX_REFCLK 4

typedef struct {
    char     name[128];       /* s= */
    char     origin[160];     /* o= a verzioszam nelkul: a SAP-frissitesek kulcsa */
    char     addr[64];        /* c= : multicast csoport vagy unicast cim */
    char     source[64];      /* a=source-filter: incl ... forrascim (SSM), vagy "" */
    int      multicast;
    uint16_t port;
    uint8_t  pt;              /* payload type, az rtpmap-bol ertelmezve (8.4) */
    uint32_t rate;
    uint8_t  channels;
    uint8_t  bps;             /* L16: 2, L24: 3 bajt/minta */
    double   ptime;           /* ms, 0 = nincs megadva */
    int      have_mediaclk;
    uint32_t mediaclk;        /* a=mediaclk:direct=<offset> (8.3) */
    int      refclk;          /* 0 nincs ts-refclk, 1 PTP, 2 mas ora (8.2) */
    int      traceable;
    int      nclk;            /* PTP forrasok GMID-del (8.2: tobb is lehet) */
    struct { char gmid[24]; int domain; } clk[MAX_REFCLK];
} sdp_t;

const char *sdp_parse(const char *text, sdp_t *out);   /* NULL = ok, kulonben a hiba oka */
void        sdp_origin(const char *text, char *out, size_t n);
int         sap_open(const char *iface);               /* -1 = hiba */
/* Egy SAP bejelentes timeout_ms-on belul: 1 = van (sdp kitoltve), 0 = nincs. */
int         sap_recv(int fd, int timeout_ms, char *sdp, size_t n, int *deleted,
                     char *from, size_t from_n);

/* ---- PTP es media ora ---- */

int      ptp_start(const char *iface, int domain, int external, long lock_ns, int verbose);
void     ptp_stop(void);
int      ptp_locked(void);
long     ptp_offset(void);                    /* ns */
int      ptp_gmid(char *out, size_t n);       /* 0 = ismert */
/* Media ora (5.): mintaszam az 1970-01-01 00:00:00 TAI epoch ota. */
uint64_t media_now(uint32_t rate);
/* Helyi, nem hangolt ora mintaban (erkezesi modhoz). */
uint64_t raw_now(uint32_t rate);
/* Szal inditasa SIGINT/SIGTERM nelkul (azokat a fo szal kapja), prio > 0: SCHED_FIFO. */
int      thread_start(pthread_t *t, void *(*fn)(void *), void *arg, int prio);

/* ---- Jitter puffer ---- */

#define JB_ARR 0   /* a kuldo nem a PTP orat koveti: erkezesi idobol becsult eltolas */
#define JB_PTP 1   /* AES67: az S stream-pozicio az S + link offset PTP-idopontban szol */

typedef struct jb jb_t;

typedef struct {
    int      ready, mode;
    uint32_t gen;             /* uj korszak (reset, modvaltas): a kimenet ujraszinkronizal */
    int64_t  off;             /* erkezesi modban: helyi ora - stream pozicio (PTP-ben 0) */
    double   transit_ms;      /* a legkesobbi csomag kesese az off-hoz kepest (2-4 s) */
    double   jitter_ms, fill_ms;
    uint32_t pkt_frames;      /* utolso csomag mintaszama (tenyleges packet time) */
    uint64_t received, lost, dup, late;
    uint32_t ext_seq, expected, uniq, jitter_ts;   /* RTCP, az aktualis korszakra */
} jb_stats_t;

jb_t *jb_create(uint32_t rate, uint8_t channels, uint8_t bps, uint32_t mediaclk, int ptp);
void  jb_destroy(jb_t *jb);
void  jb_push(jb_t *jb, uint32_t ts, uint16_t seq, const uint8_t *data, size_t len,
              uint64_t now_ptp, uint64_t now_raw);
void  jb_reset(jb_t *jb);
void  jb_allow_ptp(jb_t *jb, int on);  /* a vevo PTP-je lockolt: PTP modra valthat */
void  jb_stats(jb_t *jb, jb_stats_t *out);
int   jb_read(jb_t *jb, int64_t pos, size_t n, int32_t *out);  /* -> hianyzo mintak */
void  jb_release(jb_t *jb, int64_t pos);                       /* pos elott mar nem kell */

/* ---- RTP vetel, RTCP ---- */

typedef struct rx rx_t;
typedef struct { uint64_t packets, foreign, oversize; } rx_stats_t;

rx_t *rx_start(const sdp_t *sp, const char *iface, jb_t *jb);
void  rx_stop(rx_t *r);
void  rx_get_stats(rx_t *r, rx_stats_t *out);
void  rx_rejoin(rx_t *r);              /* IGMP tagsag megujitasa (10.2, 4. megj.) */

/* ---- ALSA kimenet ---- */

typedef struct audio audio_t;
typedef struct {
    int      synced;
    double   link_ms, err_us, ppm;
    uint64_t xruns, resyncs, missing;
} audio_stats_t;

audio_t *audio_start(jb_t *jb, uint32_t rate, uint8_t channels, const char *dev,
                     double link_ms);
void     audio_stop(audio_t *a);
void     audio_get_stats(audio_t *a, audio_stats_t *out);

#endif
