/*
 * RTP vetel es RTCP (AES67 6., 10.2).
 *
 *   - IPv4 unicast es multicast (6.1); multicastnal IGMP csatlakozas az interfeszen
 *     (10.2: a kernel IGMPv3-mal indul, v2 halozaton visszaesik). Ha az SDP-ben
 *     a=source-filter van, forrasspecifikus (SSM) csatlakozas; ha ugy nem jon csomag,
 *     a tagsag megujitasakor forrasszures nelkul probaljuk.
 *   - RTP (RFC 3550): CSRC, fejlec-kiterjesztes, padding tolerálva (6.3); a PT-t az
 *     SDP rtpmap-je adja (8.4), mas PT eldobva; SSRC rogzitve, uj kuldo (100 csomag
 *     uj SSRC-vel) eseten a jitter puffer ujraindul.
 *   - 1440 B-nal nagyobb payload: fogadjuk, de jelezzuk (6.3).
 *   - RTCP receiver report + SDES CNAME (RFC 3550 6.4.2), kilepeskor BYE, az RTP port+1-re;
 *     DSCP AF41, ahogy az RTP media (6.2).
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/random.h>
#include <sys/socket.h>

#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49
#endif
#define MAX_PAYLOAD 1440        /* 6.3 */
#define DSCP_AF41   (34 << 2)

struct rx {
    sdp_t     sp;
    jb_t     *jb;
    int       fd, rtcp_fd, ifindex, ssm, joined;
    struct in_addr group, source;
    pthread_t th;
    volatile int run;
    pthread_mutex_t lock;
    rx_stats_t st;
    uint32_t  ssrc, my_ssrc, prev_exp, prev_rcv;
    unsigned  seed;
    struct sockaddr_in rtcp_to;
    char      cname[80];
};

static int join(rx_t *r, int on)
{
    struct sockaddr_in g = { .sin_family = AF_INET, .sin_addr = r->group };
    if (r->ssm) {
        struct group_source_req q = { .gsr_interface = (uint32_t)r->ifindex };
        memcpy(&q.gsr_group, &g, sizeof(g));
        g.sin_addr = r->source;
        memcpy(&q.gsr_source, &g, sizeof(g));
        return setsockopt(r->fd, IPPROTO_IP, on ? MCAST_JOIN_SOURCE_GROUP : MCAST_LEAVE_SOURCE_GROUP,
                          &q, sizeof(q));
    }
    struct group_req q = { .gr_interface = (uint32_t)r->ifindex };
    memcpy(&q.gr_group, &g, sizeof(g));
    return setsockopt(r->fd, IPPROTO_IP, on ? MCAST_JOIN_GROUP : MCAST_LEAVE_GROUP, &q, sizeof(q));
}

void rx_rejoin(rx_t *r)
{
    if (!r->joined) return;
    join(r, 0);
    if (r->ssm) {
        r->ssm = 0;
        fprintf(stderr, "rx: forrasspecifikus (SSM) tagsag helyett forrasszures nelkul probalom\n");
    }
    join(r, 1);
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}

/* RR + SDES (+ BYE) osszetett RTCP csomag. */
static void rtcp_send(rx_t *r, int bye)
{
    jb_stats_t s;
    jb_stats(r->jb, &s);
    pthread_mutex_lock(&r->lock);
    uint32_t src = r->ssrc;
    pthread_mutex_unlock(&r->lock);
    if (!src || !s.uniq || !r->rtcp_to.sin_port || r->rtcp_fd < 0) return;

    if (s.expected < r->prev_exp || s.uniq < r->prev_rcv) r->prev_exp = r->prev_rcv = 0;
    uint32_t de = s.expected - r->prev_exp, dr = s.uniq - r->prev_rcv;
    r->prev_exp = s.expected;
    r->prev_rcv = s.uniq;
    uint32_t frac = de > dr ? (uint32_t)(((uint64_t)(de - dr) << 8) / de) : 0;
    uint32_t cum = s.expected > s.uniq ? s.expected - s.uniq : 0;
    if (cum > 0x7FFFFF) cum = 0x7FFFFF;

    uint8_t b[160];
    size_t cl = strlen(r->cname), words = (4 + 2 + cl + 1 + 3) / 4, o = 32;
    memcpy(b, "\x81\xC9\x00\x07", 4);                       /* RR, 1 report block */
    put32(b + 4, r->my_ssrc);
    put32(b + 8, src);
    put32(b + 12, (frac > 255 ? 255 : frac) << 24 | cum);
    put32(b + 16, s.ext_seq);
    put32(b + 20, s.jitter_ts);
    put32(b + 24, 0);                                       /* LSR, DLSR: SR-t nem kovetunk */
    put32(b + 28, 0);
    memset(b + o, 0, 4 + words * 4);
    b[o] = 0x81; b[o + 1] = 202; b[o + 3] = (uint8_t)words; /* SDES, CNAME */
    put32(b + o + 4, r->my_ssrc);
    b[o + 8] = 1;
    b[o + 9] = (uint8_t)cl;
    memcpy(b + o + 10, r->cname, cl);
    o += 4 + words * 4;
    if (bye) {
        memcpy(b + o, "\x81\xCB\x00\x01", 4);
        put32(b + o + 4, r->my_ssrc);
        o += 8;
    }
    sendto(r->rtcp_fd, b, o, 0, (struct sockaddr *)&r->rtcp_to, sizeof(r->rtcp_to));
}

/* RFC 3550 6.3.1: 5 s minimum, [0.5, 1.5] veletlen szorzo, e - 3/2 kompenzacio */
static double rtcp_interval(rx_t *r)
{
    return 5.0 * (0.5 + (rand_r(&r->seed) % 1000) / 1000.0) / 1.21828;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static void count(rx_t *r, uint64_t *field)
{
    pthread_mutex_lock(&r->lock);
    (*field)++;
    pthread_mutex_unlock(&r->lock);
}

static void *rx_thread(void *arg)
{
    rx_t *r = arg;
    static uint8_t b[65536];
    double next_rtcp = now_s() + rtcp_interval(r);
    int warned_pt = 0, warned_size = 0, pending = 0;
    uint32_t pending_ssrc = 0;

    while (r->run) {
        struct pollfd p = { .fd = r->fd, .events = POLLIN };
        int pr = poll(&p, 1, 50);
        if (now_s() >= next_rtcp) {
            rtcp_send(r, 0);
            next_rtcp = now_s() + rtcp_interval(r);
        }
        if (pr <= 0) continue;

        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(r->fd, b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
        const uint64_t t_ptp = media_now(r->sp.rate), t_raw = raw_now(r->sp.rate);
        if (n <= 0) continue;
        count(r, &r->st.packets);

        /* RTP fejlec: V=2, CSRC lista, kiterjesztes, padding */
        size_t off = 12 + 4 * (size_t)(b[0] & 0x0F), end = (size_t)n;
        if (n >= 12 && (b[0] & 0x10) && off + 4 <= end) off += 4 + 4 * (size_t)(b[off + 2] << 8 | b[off + 3]);
        if (n >= 12 && (b[0] & 0x20)) end -= b[n - 1];
        if (n < 12 || (b[0] >> 6) != 2 || off >= end || end > (size_t)n) {
            count(r, &r->st.foreign);
            continue;
        }
        const uint8_t  pt   = b[1] & 0x7F;
        const uint16_t seq  = (uint16_t)(b[2] << 8 | b[3]);
        const uint32_t ts   = (uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7];
        const uint32_t ssrc = (uint32_t)b[8] << 24 | b[9] << 16 | b[10] << 8 | b[11];

        if (pt != r->sp.pt) {
            count(r, &r->st.foreign);
            if (!warned_pt++)
                fprintf(stderr, "FIGYELEM: a stream payload type-ja %u, a leirase %u: az SDP nem a "
                                "kuldo aktualis leirasa (SAP-pal: 'sap' argumentum).\n", pt, r->sp.pt);
            continue;
        }
        if (end - off > MAX_PAYLOAD) {
            count(r, &r->st.oversize);
            if (!warned_size++)
                fprintf(stderr, "Megjegyzes: %zu B RTP payload (> 1440 B, 6.3): fogadjuk, de a "
                                "kuldo nem szabvanyos.\n", end - off);
        }

        pthread_mutex_lock(&r->lock);
        int ok = 1, reset = 0;
        if (!r->ssrc) r->ssrc = ssrc;
        else if (ssrc != r->ssrc) {
            pending = ssrc == pending_ssrc ? pending + 1 : 1;
            pending_ssrc = ssrc;
            r->st.foreign++;
            ok = reset = pending >= 100;
            if (reset) r->ssrc = ssrc;
        } else
            pending = 0;
        pthread_mutex_unlock(&r->lock);
        if (reset) {
            fprintf(stderr, "rx: uj SSRC (0x%08x), a jitter puffer ujraindul\n", ssrc);
            jb_reset(r->jb);
        }
        if (!ok) continue;

        /* RTCP cel: forrasszures nelkuli multicastnal a csoport, kulonben a kuldo; port+1 */
        if (!r->rtcp_to.sin_port) {
            r->rtcp_to.sin_family = AF_INET;
            r->rtcp_to.sin_addr = r->sp.multicast && !r->ssm ? r->group : from.sin_addr;
            r->rtcp_to.sin_port = htons((uint16_t)(r->sp.port + 1));
        }
        jb_push(r->jb, ts, seq, b + off, end - off, t_ptp, t_raw);
    }
    return NULL;
}

rx_t *rx_start(const sdp_t *sp, const char *iface, jb_t *jb)
{
    rx_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->sp = *sp;
    r->jb = jb;
    r->rtcp_fd = -1;
    r->ifindex = (int)if_nametoindex(iface);
    inet_pton(AF_INET, sp->addr, &r->group);
    r->ssm = sp->source[0] && inet_pton(AF_INET, sp->source, &r->source) == 1;
    pthread_mutex_init(&r->lock, NULL);

    r->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int one = 1, zero = 0, rcvbuf = 2 << 20;
    setsockopt(r->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(r->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(r->fd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof(zero));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(sp->port) };
    if (sp->multicast) sa.sin_addr = r->group;
    if (bind(r->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("rx: bind");
        goto fail;
    }
    if (sp->multicast) {
        if (join(r, 1) < 0) { perror("rx: multicast csatlakozas"); goto fail; }
        r->joined = 1;
    }

    r->rtcp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int tos = DSCP_AF41, ttl = 32;
    struct ip_mreqn mi = { .imr_ifindex = r->ifindex };
    setsockopt(r->rtcp_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    setsockopt(r->rtcp_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(r->rtcp_fd, IPPROTO_IP, IP_MULTICAST_IF, &mi, sizeof(mi));

    char host[64] = "";
    gethostname(host, sizeof(host) - 1);
    snprintf(r->cname, sizeof(r->cname), "aes67rx@%.60s", host);
    if (getrandom(&r->seed, sizeof(r->seed), 0) != sizeof(r->seed)) r->seed = (unsigned)getpid();
    r->my_ssrc = (uint32_t)rand_r(&r->seed) << 16 ^ (uint32_t)rand_r(&r->seed);

    r->run = 1;
    if (thread_start(&r->th, rx_thread, r, 60) == 0) return r;
    perror("rx: szal");
fail:
    if (r->joined) join(r, 0);
    close(r->fd);
    if (r->rtcp_fd >= 0) close(r->rtcp_fd);
    pthread_mutex_destroy(&r->lock);
    free(r);
    return NULL;
}

void rx_stop(rx_t *r)
{
    if (!r) return;
    r->run = 0;
    pthread_join(r->th, NULL);
    rtcp_send(r, 1);
    if (r->joined) join(r, 0);
    close(r->fd);
    close(r->rtcp_fd);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

void rx_get_stats(rx_t *r, rx_stats_t *out)
{
    pthread_mutex_lock(&r->lock);
    *out = r->st;
    pthread_mutex_unlock(&r->lock);
}
