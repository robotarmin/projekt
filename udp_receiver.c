/*
 * RTP fogadas es RTCP (AES67 6.3).
 *
 *   - payload format az SDP rtpmap alapjan (8.4); idegen PT eldobva
 *   - SSRC rogzites; ha a kuldo uj SSRC-vel indul ujra, automatikus atallas
 *   - CSRC / fejlec-kiterjesztes / padding tolerálva (rtp.c, 6.3)
 *   - 1440 B-nal nagyobb payload: elfogadjuk, de jelezzuk (a kuldo nem felel meg a 6.3-nak)
 *   - RTCP receiver report + SDES CNAME (RFC 3550 6.4.2, 6.5), kilepeskor BYE;
 *     DSCP AF41, mint az RTP media (6.2: "Receivers should mark outgoing RTCP ...")
 *   - erkezesi idobelyeg a PTP media oraval, kozvetlenul a recvfrom utan
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define RX_BUF_SIZE       65536     /* legnagyobb UDP datagram */
#define RX_POLL_MS        50
#define SSRC_RESYNC_PKTS  100
#define RTCP_MIN_INTERVAL 5.0       /* s, RFC 3550 6.2 minimum */
#define DSCP_AF41_TOS     (34 << 2)

struct udp_receiver {
    aes67_session *session;
    jitter_buffer *jb;
    uint8_t        expected_pt;
    uint32_t       sample_rate;

    pthread_t      thread;
    volatile int   running;

    pthread_mutex_t lock;
    udp_rx_stats   stats;
    uint32_t       locked_ssrc;

    int            rtcp_fd;
    uint32_t       my_ssrc;
    char           cname[64];
    struct sockaddr_in rtcp_dest;
    int            rtcp_dest_valid;
    uint32_t       prev_expected, prev_received;
    unsigned int   rnd;
};

/* ---------------------------------------------------------- */

#define PUT32(b, o, v) do { uint32_t _v = (v); (b)[(o)++] = (uint8_t)(_v >> 24); \
    (b)[(o)++] = (uint8_t)(_v >> 16); (b)[(o)++] = (uint8_t)(_v >> 8); (b)[(o)++] = (uint8_t)_v; } while (0)

static size_t put_sdes(udp_receiver *r, uint8_t *b, size_t o)
{
    size_t cl = strlen(r->cname);
    size_t chunk = (4 + 2 + cl + 1 + 3) & ~(size_t)3;      /* SSRC + item + vege, 4-re igazitva */
    size_t words = chunk / 4;
    b[o++] = 0x81; b[o++] = 202;
    b[o++] = (uint8_t)(words >> 8); b[o++] = (uint8_t)words;
    PUT32(b, o, r->my_ssrc);
    b[o++] = 1; b[o++] = (uint8_t)cl;
    memcpy(&b[o], r->cname, cl); o += cl;
    size_t pad = chunk - (4 + 2 + cl);
    memset(&b[o], 0, pad); o += pad;
    return o;
}

static size_t build_rr(udp_receiver *r, uint8_t *b, uint32_t src_ssrc)
{
    uint32_t ext_high, expected, received, jit;
    if (jb_get_rtcp(r->jb, &ext_high, &expected, &received, &jit) != 0) return 0;

    uint32_t exp_int = expected - r->prev_expected;
    uint32_t rcv_int = received - r->prev_received;
    r->prev_expected = expected;
    r->prev_received = received;
    int64_t lost_int = (int64_t)exp_int - (int64_t)rcv_int;
    uint32_t fraction = (exp_int && lost_int > 0) ? (uint32_t)((lost_int << 8) / exp_int) : 0;
    if (fraction > 255) fraction = 255;
    int64_t cum = (int64_t)expected - (int64_t)received;
    if (cum < 0) cum = 0;
    if (cum > 0x7FFFFF) cum = 0x7FFFFF;

    size_t o = 0;
    b[o++] = 0x81; b[o++] = 201; b[o++] = 0; b[o++] = 7;   /* RR, RC=1, hossz=7 */
    PUT32(b, o, r->my_ssrc);
    PUT32(b, o, src_ssrc);
    PUT32(b, o, (fraction << 24) | (uint32_t)cum);
    PUT32(b, o, ext_high);
    PUT32(b, o, jit);
    PUT32(b, o, 0);                                       /* LSR (SR-t nem kovetunk) */
    PUT32(b, o, 0);                                       /* DLSR */
    return put_sdes(r, b, o);
}

static void rtcp_send(udp_receiver *r, const uint8_t *b, size_t n)
{
    if (r->rtcp_fd < 0 || !r->rtcp_dest_valid || !n) return;
    if (sendto(r->rtcp_fd, b, n, 0, (struct sockaddr *)&r->rtcp_dest,
               sizeof(r->rtcp_dest)) == (ssize_t)n) {
        pthread_mutex_lock(&r->lock);
        r->stats.rtcp_sent++;
        pthread_mutex_unlock(&r->lock);
    }
}

static void send_rr(udp_receiver *r)
{
    uint32_t ssrc = udp_receiver_get_ssrc(r);
    if (!ssrc) return;
    uint8_t buf[256];
    rtcp_send(r, buf, build_rr(r, buf, ssrc));
}

static void send_bye(udp_receiver *r)
{
    uint32_t ssrc = udp_receiver_get_ssrc(r);
    if (!ssrc) return;
    uint8_t buf[256];
    size_t o = build_rr(r, buf, ssrc);
    if (!o) return;
    buf[o++] = 0x81; buf[o++] = 203; buf[o++] = 0; buf[o++] = 1;   /* BYE, SC=1 */
    PUT32(buf, o, r->my_ssrc);
    rtcp_send(r, buf, o);
}

static double next_interval(udp_receiver *r)
{
    /* RFC 3550 6.3.1: [0.5, 1.5] szorzo a determinisztikus intervallumon */
    double f = 0.5 + (double)(rand_r(&r->rnd) % 1000) / 1000.0;
    return RTCP_MIN_INTERVAL * f / 1.21828;               /* e - 3/2 kompenzacio */
}

static double now_mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---------------------------------------------------------- */

static void *rx_thread_main(void *arg)
{
    udp_receiver *r = arg;
    int fd = session_get_fd(r->session);
    if (fd < 0) return NULL;
    const session_params *sp = session_get_params(r->session);

    aes67_set_rt_priority(60, "fogado szal");

    static uint8_t buf[RX_BUF_SIZE];
    int pt_warned = 0, size_warned = 0, push_err = 0, pending_count = 0;
    uint32_t pending_ssrc = 0;
    double next_rtcp = now_mono() + next_interval(r);

    while (r->running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, RX_POLL_MS);
        if (pr < 0) { if (errno == EINTR) continue; break; }

        double t = now_mono();
        if (t >= next_rtcp) {
            send_rr(r);
            next_rtcp = t + next_interval(r);
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) continue;

        struct sockaddr_in from; socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        const uint64_t now_ptp = ptp_media_clock_now(r->sample_rate);
        const uint64_t now_raw = raw_media_clock_now(r->sample_rate);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fprintf(stderr, "fogado: recvfrom hiba: %s\n", strerror(errno));
            break;
        }
        if (n == 0) continue;

        pthread_mutex_lock(&r->lock);
        r->stats.packets_received++;
        pthread_mutex_unlock(&r->lock);

        rtp_header h;
        if (rtp_parse(buf, (size_t)n, &h) != 0) {
            pthread_mutex_lock(&r->lock);
            r->stats.packets_parse_error++;
            pthread_mutex_unlock(&r->lock);
            continue;
        }

        if (h.payload_type != r->expected_pt) {
            pthread_mutex_lock(&r->lock);
            r->stats.packets_wrong_pt++;
            pthread_mutex_unlock(&r->lock);
            if (!pt_warned) {
                pt_warned = 1;
                fprintf(stderr, "FIGYELEM: a stream payload type-ja %u, az SDP rtpmap szerint %u. "
                                "Az SDP nem a kuldo aktualis leirasa (SAP-pal: 'sap' argumentum).\n",
                        h.payload_type, r->expected_pt);
            }
            continue;
        }

        if (h.payload_length > AES67_MAX_PAYLOAD) {
            pthread_mutex_lock(&r->lock);
            r->stats.packets_oversize++;
            pthread_mutex_unlock(&r->lock);
            if (!size_warned) {
                size_warned = 1;
                fprintf(stderr, "Megjegyzes: %zu B RTP payload (> 1440 B, AES67 6.3). "
                                "Fogadjuk, de a kuldo nem szabvanyos.\n", h.payload_length);
            }
        }

        /* SSRC rogzites + atallas uj SSRC-re (kuldo ujraindult) */
        int do_reset = 0;
        pthread_mutex_lock(&r->lock);
        if (r->locked_ssrc == 0) {
            r->locked_ssrc = h.ssrc;
            pending_count = 0;
        } else if (h.ssrc != r->locked_ssrc) {
            r->stats.packets_wrong_ssrc++;
            if (h.ssrc == pending_ssrc) pending_count++;
            else { pending_ssrc = h.ssrc; pending_count = 1; }
            if (pending_count >= SSRC_RESYNC_PKTS) {
                r->locked_ssrc = h.ssrc;
                pending_count = 0;
                do_reset = 1;
            } else {
                pthread_mutex_unlock(&r->lock);
                continue;
            }
        } else {
            pending_count = 0;
        }
        pthread_mutex_unlock(&r->lock);

        if (do_reset) {
            fprintf(stderr, "Uj SSRC (0x%08x): a jitter puffer ujraindul\n", h.ssrc);
            jb_reset(r->jb);
            r->prev_expected = r->prev_received = 0;
        }

        /* RTCP cel: multicastnal csoport:port+1, unicastnal a kuldo:port+1 (5004 -> 5005) */
        if (!r->rtcp_dest_valid) {
            memset(&r->rtcp_dest, 0, sizeof(r->rtcp_dest));
            r->rtcp_dest.sin_family = AF_INET;
            r->rtcp_dest.sin_port   = htons((uint16_t)(sp->port + 1));
            if (sp->is_multicast) inet_pton(AF_INET, sp->address, &r->rtcp_dest.sin_addr);
            else                  r->rtcp_dest.sin_addr = from.sin_addr;
            r->rtcp_dest_valid = 1;
        }

        if (jb_push(r->jb, &h, now_ptp, now_raw) == 0) {
            pthread_mutex_lock(&r->lock);
            r->stats.packets_pushed++;
            pthread_mutex_unlock(&r->lock);
        } else if (push_err++ < 5) {
            fprintf(stderr, "HIBA: a jitter puffer elutasitotta a csomagot (seq=%u, %zu B)\n",
                    h.sequence, h.payload_length);
        }
    }
    return NULL;
}

udp_receiver *udp_receiver_create(aes67_session *s, jitter_buffer *jb)
{
    if (!s || !jb) return NULL;
    const session_params *sp = session_get_params(s);
    if (!sp) return NULL;

    udp_receiver *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->session     = s;
    r->jb          = jb;
    r->expected_pt = sp->payload_type;
    r->sample_rate = sp->sample_rate;
    pthread_mutex_init(&r->lock, NULL);

    r->rtcp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (r->rtcp_fd >= 0) {
        int tos = DSCP_AF41_TOS;
        setsockopt(r->rtcp_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        unsigned char ttl = 16, loop = 0;
        setsockopt(r->rtcp_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        setsockopt(r->rtcp_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        int ifx = session_get_ifindex(s);
        if (ifx > 0) {
            struct ip_mreqn m;
            memset(&m, 0, sizeof(m));
            m.imr_ifindex = ifx;
            setsockopt(r->rtcp_fd, IPPROTO_IP, IP_MULTICAST_IF, &m, sizeof(m));
        }
    }

    char host[40] = "host";
    gethostname(host, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    snprintf(r->cname, sizeof(r->cname), "aes67rx@%s", host);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    r->rnd = (unsigned)(ts.tv_nsec ^ (getpid() << 12));
    r->my_ssrc = (uint32_t)rand_r(&r->rnd) ^ ((uint32_t)rand_r(&r->rnd) << 16);
    if (!r->my_ssrc) r->my_ssrc = 1;
    return r;
}

int udp_receiver_start(udp_receiver *r)
{
    if (!r || r->running) return -1;
    r->running = 1;
    if (aes67_thread_create(&r->thread, rx_thread_main, r) != 0) { r->running = 0; return -1; }
    return 0;
}

void udp_receiver_stop(udp_receiver *r)
{
    if (!r || !r->running) return;
    r->running = 0;
    pthread_join(r->thread, NULL);
    send_bye(r);
}

void udp_receiver_destroy(udp_receiver *r)
{
    if (!r) return;
    if (r->running) udp_receiver_stop(r);
    if (r->rtcp_fd >= 0) close(r->rtcp_fd);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

void udp_receiver_get_stats(udp_receiver *r, udp_rx_stats *out)
{
    if (!r || !out) return;
    pthread_mutex_lock(&r->lock);
    *out = r->stats;
    pthread_mutex_unlock(&r->lock);
}

uint32_t udp_receiver_get_ssrc(udp_receiver *r)
{
    if (!r) return 0;
    pthread_mutex_lock(&r->lock);
    uint32_t v = r->locked_ssrc;
    pthread_mutex_unlock(&r->lock);
    return v;
}
