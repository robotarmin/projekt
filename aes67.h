/*
 * aes67rx - AES67-2023 vevo (receiver) szoftveres PTP idobelyegzessel.
 *
 * Modulok:
 *   rtp.c          RTP fejlec feldolgozas (RFC 3550 / 3551)
 *   session.c      SDP feldolgozas (RFC 8866, RFC 7273), multicast/unicast socket, IGMP
 *   sap.c          SAP (RFC 2974) alapu SDP felderites (AES67 9., E.2 melleklet)
 *   ptp_slave.c    PTP timeReceiver: sajat ptp4l vagy kulso ptp4l (pmc), media ora (5.)
 *   jitter.c       media ora szerint cimzett jitter puffer
 *   udp_receiver.c RTP fogadas, RTCP receiver report / SDES / BYE
 *   audio_output.c ALSA kimenet, PTP media orahoz kotott lejatszas (allando link offset,
 *                  7.4), adaptiv resampling a hangkartya oraeltereseinek kiegyenlitesere
 *   main.c         vezerles, statisztika, szinkron allapot figyelese (8.2)
 */
#ifndef AES67_H
#define AES67_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

/* ============================================================
 * RTP (RFC 3550 / 3551)
 * ============================================================ */

typedef struct {
    uint8_t  version, padding, extension, csrc_count;
    uint8_t  marker, payload_type;
    uint16_t sequence;
    uint32_t timestamp, ssrc;
    size_t   payload_offset, payload_length;
    const uint8_t *payload;
} rtp_header;

int rtp_parse(const uint8_t *packet, size_t length, rtp_header *out);

static inline int rtp_seq_diff(uint16_t a, uint16_t b)
{
    uint16_t d = (uint16_t)(a - b);
    return (d < 0x8000u) ? (int)d : (int)d - 0x10000;
}

static inline int32_t rtp_ts_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);            /* ketto-komplemens: atfordulas-biztos */
}

/* AES67 6.3: legnagyobb megengedett RTP payload */
#define AES67_MAX_PAYLOAD 1440

/* ============================================================
 * SDP / session (AES67-2023 8. fejezet)
 * ============================================================ */

#define SDP_CLK_NONE   0   /* nincs ts-refclk */
#define SDP_CLK_PTP    1   /* ptp=IEEE1588-2008 (2019 is igy jelolve, 8.2) */
#define SDP_CLK_8021AS 2   /* ptp=IEEE802.1AS-2011 */
#define SDP_CLK_OTHER  3   /* local, ntp, ... */

#define SDP_DIR_RECVONLY  0
#define SDP_DIR_SENDRECV  1
#define SDP_DIR_SENDONLY  2
#define SDP_DIR_INACTIVE  3

typedef struct {
    char     session_name[128];  /* s= */
    char     origin_addr[64];    /* o= cime */
    char     address[64];        /* c=: multicast csoport vagy unicast cim */
    int      is_multicast;
    uint16_t port;
    uint8_t  payload_type;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bytes_per_sample;   /* L16 = 2, L24 = 3 */
    double   ptime_ms;           /* 0 = nincs megadva */
    double   maxptime_ms;        /* 0 = nincs megadva */
    uint32_t packet_samples;     /* ptime alapjan (csak tajekoztato, 7.2.0) */

    int      have_mediaclk;      /* a=mediaclk:direct=<offset> */
    uint32_t mediaclk_offset;

    int      clock_type;         /* SDP_CLK_* */
    char     gmid[32];           /* "39-A7-94-FF-FE-07-CB-D0" vagy "" */
    int      ptp_domain;         /* -1 = nincs megadva */
    int      traceable;

    int      direction;          /* SDP_DIR_* */
} session_params;

#define SDP_OK              0
#define SDP_ERR_OPEN       -1
#define SDP_ERR_SYNTAX     -2
#define SDP_ERR_MISSING    -3
#define SDP_ERR_CODEC      -4
#define SDP_ERR_RATE       -5
#define SDP_ERR_CHANNELS   -6
#define SDP_ERR_DIRECTION  -7

int         sdp_parse_text(const char *text, session_params *out);
int         sdp_parse_file(const char *path, session_params *out);
const char *sdp_strerror(int rc);
/* Szabvanyos-e a ptime (7.2, 2. tablazat): 1 = igen, 0 = nem, -1 = nincs ptime */
int         sdp_ptime_is_standard(const session_params *p);

typedef struct aes67_session aes67_session;

aes67_session *session_open(const session_params *p, const char *iface);
void           session_close(aes67_session *s);
int            session_rejoin(aes67_session *s);    /* IGMP report ujrakuldese (10.2, 4. megj.) */
int            session_get_fd(const aes67_session *s);
int            session_get_ifindex(const aes67_session *s);
const session_params *session_get_params(const aes67_session *s);

/* ============================================================
 * SAP felderites (RFC 2974, AES67 E.2)
 * ============================================================ */

/* list_only = 1: a bejelentett streamek kiirasa timeout_s ideig.
 * Kulonben az elso, name_filter-re illeszkedo (NULL/"" = barmelyik) L16/L24
 * audio SDP-t adja vissza out-ban. 0 = siker, -1 = hiba / nincs talalat. */
int sap_find(const char *iface, const char *name_filter, int timeout_s,
             int list_only, char *out, size_t out_size);

/* ============================================================
 * PTP timeReceiver (IEEE 1588-2008, AES67 4., A melleklet)
 * ============================================================ */

#define PTP_MODE_INTERNAL 0     /* sajat ptp4l -S folyamat */
#define PTP_MODE_EXTERNAL 1     /* mar futo ptp4l, pmc-vel figyelve */

int      ptp_slave_start(const char *iface, int domain, int mode);
void     ptp_slave_stop(void);
void     ptp_slave_set_lock_threshold(long ns);
void     ptp_slave_set_verbose(int on);           /* a ptp4l teljes naplojanak kiirasa */
bool     ptp_slave_is_locked(void);
long     ptp_slave_get_offset_ns(void);
int      ptp_slave_get_gmid(char *out, size_t n);     /* 0 = ok, -1 = ismeretlen */
int      ptp_slave_get_tai(struct timespec *ts);      /* PTP idoskala (TAI) */
/* Media ora (5. fejezet): mintaszam a TAI epoch (1970-01-01 00:00:00 TAI) ota. */
uint64_t ptp_media_clock_now(uint32_t sample_rate);
/* Helyi, nem hangolt (CLOCK_MONOTONIC_RAW) ora mintaban: erkezesi modhoz, amikor a
 * rendszerorat a ptp4l meg erosen hangolja. */
uint64_t raw_media_clock_now(uint32_t sample_rate);

/* ============================================================
 * Jitter puffer
 * ============================================================ */

/* Legnagyobb elfogadott RTP payload: a legnagyobb UDP datagram. Az AES67 (6.3) 1440 B-ot
 * ir elo, a nagyobb csomagot elfogadjuk, de jelezzuk. A csomag hossza ezen felul
 * legfeljebb a jitter puffer negyede lehet (~170 ms). */
#define JB_MAX_PACKET_BYTES 65536

#define JB_MODE_ARRIVAL 0   /* nincs kozos ora: erkezesi idobol becsult eltolas */
#define JB_MODE_PTP     1   /* az RTP timestamp a PTP media orat koveti (5., 8.3) */

typedef struct jitter_buffer jitter_buffer;

typedef struct {
    uint64_t packets_received;
    uint64_t packets_lost;
    uint64_t packets_duplicate;
    uint64_t packets_late;
    uint64_t underrun_frames;
    uint64_t resets;
    double   jitter_ms;          /* RFC 3550 interarrival jitter */
    double   fill_ms;            /* beerkezett, meg le nem jatszott audio */
    uint32_t packet_frames;      /* utolso csomag mintaszama (tenyleges packet time) */
    int      mode;               /* JB_MODE_* */
    int      ready;
    int      have_transit_max;   /* PTP modban: van mert atviteli ido */
    double   transit_max_ms;     /* PTP modban: a csomag utolso mintajanak legnagyobb
                                    kesese a timestampjehez kepest (kuldo + halozat) */
} jb_stats;

jitter_buffer *jb_create(uint32_t sample_rate, uint8_t channels,
                         uint8_t bytes_per_sample,
                         int ptp_mode, uint32_t mediaclk_offset);
void  jb_destroy(jitter_buffer *jb);
/* now_ptp: PTP media ora, now_raw: helyi nyers ora (mindketto az erkezes pillanataban) */
int   jb_push(jitter_buffer *jb, const rtp_header *h, uint64_t now_ptp, uint64_t now_raw);
/* Szabad-e PTP modra valtani (a vevo PTP-je lockolt). Erkezesi modban a puffer
 * passzivan figyeli, hogy a kuldo timestampje a PTP orahoz igazodik-e, es ha igen,
 * atall PTP modra. */
void  jb_set_ptp_allowed(jitter_buffer *jb, int allowed);
void  jb_reset(jitter_buffer *jb);
/* lokalis media ido = stream pozicio + offset */
int   jb_get_clock(jitter_buffer *jb, int64_t *offset, int *ready, uint32_t *generation,
                   int *mode);
/* nframes frame (int32, 24 bites skala, interleaved) a start poziciotol.
 * Visszaad: a meg meg nem erkezett frame-ek szamat. */
int   jb_read(jitter_buffer *jb, int64_t start, size_t nframes, int32_t *out);
void  jb_advance(jitter_buffer *jb, int64_t new_floor);
/* a legmagasabb beerkezett stream pozicio + 1 (INT64_MIN, ha nincs adat) */
int64_t jb_get_end(jitter_buffer *jb);
void  jb_get_stats(jitter_buffer *jb, jb_stats *out);
int   jb_get_rtcp(jitter_buffer *jb, uint32_t *ext_highest_seq, uint32_t *expected,
                  uint32_t *received, uint32_t *jitter_samples);

/* ============================================================
 * UDP / RTP fogado, RTCP (6.3)
 * ============================================================ */

typedef struct udp_receiver udp_receiver;

typedef struct {
    uint64_t packets_received;
    uint64_t packets_parse_error;
    uint64_t packets_wrong_pt;
    uint64_t packets_wrong_ssrc;
    uint64_t packets_pushed;
    uint64_t packets_oversize;   /* > 1440 B payload (kuldo nem felel meg a 6.3-nak) */
    uint64_t rtcp_sent;
} udp_rx_stats;

udp_receiver *udp_receiver_create(aes67_session *s, jitter_buffer *jb);
int            udp_receiver_start(udp_receiver *r);
void           udp_receiver_stop(udp_receiver *r);
void           udp_receiver_destroy(udp_receiver *r);
void           udp_receiver_get_stats(udp_receiver *r, udp_rx_stats *out);
uint32_t       udp_receiver_get_ssrc(udp_receiver *r);

/* ============================================================
 * Audio kimenet (ALSA)
 * ============================================================ */

typedef struct audio_output audio_output;

typedef struct {
    uint64_t frames_written;
    uint64_t xruns;
    uint64_t resyncs;
    uint64_t underrun_frames;
    int      alsa_delay_frames;
    int      synced;
    double   correction_ppm;     /* aktualis sebessegkorrekcio */
    double   sync_error_us;      /* szurt idozitesi hiba a cel lejatszasi idohoz kepest */
    double   link_offset_ms;     /* 7.4 */
    unsigned out_rate, out_channels;
    char     out_format[24];
} audio_stats;

/* device: ALSA eszkoz (NULL = AES67_ALSA_DEVICE vagy alapertelmezett)
 * link_offset_ms: <= 0 eseten automatikus */
audio_output *audio_output_create(aes67_session *s, jitter_buffer *jb,
                                  const char *device, double link_offset_ms);
int            audio_output_start(audio_output *a);
void           audio_output_stop(audio_output *a);
void           audio_output_destroy(audio_output *a);
void           audio_output_get_stats(audio_output *a, audio_stats *out);

/* ============================================================
 * Segedfuggveny: valos ideju prioritas (sikertelenseg nem hiba)
 * ============================================================ */
void aes67_set_rt_priority(int prio, const char *who);
/* Munkaszal inditasa SIGINT/SIGTERM letiltasaval: a jeleket mindig a fo szal kapja,
 * igy egy blokkolo hivas (pl. snd_pcm_writei) sem szakad meg leallitaskor. */
int  aes67_thread_create(pthread_t *t, void *(*fn)(void *), void *arg);
/* Gyerekfolyamatban, exec elott: jelmaszk visszaallitasa es a 2-nel nagyobb fd-k
 * lezarasa (a gyerek ne orokolje a socketeket, az ALSA eszkozt). */
void aes67_child_prepare(void);

#endif /* AES67_H */
