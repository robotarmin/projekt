/*
 * SAP (Session Announcement Protocol, RFC 2974) - AES67 9. fejezet, E.2 melleklet.
 *
 * A kuldok (PipeWire, Dante AES67 modban, Ravenna stb.) SAP-pal hirdetik az SDP-t:
 *   adminisztrativ hatokor: 239.255.255.255:9875
 *   globalis hatokor:       224.2.127.254:9875
 * Igy a vevo mindig a kuldo AKTUALIS leirasat kapja (PT, ptime, ts-refclk, mediaclk),
 * kezzel irt SDP nelkul.
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SAP_PORT 9875
#define SAP_MAX_SEEN 64

static const char *sap_groups[] = { "239.255.255.255", "224.2.127.254" };

static int join(int fd, const char *grp, int ifindex)
{
    struct ip_mreqn m;
    memset(&m, 0, sizeof(m));
    inet_pton(AF_INET, grp, &m.imr_multiaddr);
    m.imr_ifindex = ifindex;
    return setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m));
}

/* SAP csomag -> SDP szoveg. 0 = ok, -1 = nem hasznalhato. *deletion = 1 torlesnel. */
static int sap_decode(const uint8_t *b, size_t n, char *sdp, size_t sdp_size,
                      int *deletion, uint16_t *hash)
{
    if (n < 4) return -1;
    int version = b[0] >> 5;
    int a_ipv6  = (b[0] >> 4) & 1;
    int t_del   = (b[0] >> 2) & 1;
    int enc     = (b[0] >> 1) & 1;
    int comp    = b[0] & 1;
    if (version != 1 || enc || comp) return -1;
    size_t auth = (size_t)b[1] * 4;
    *hash = (uint16_t)((b[2] << 8) | b[3]);
    size_t off = 4 + (a_ipv6 ? 16 : 4) + auth;
    if (off >= n) return -1;

    const char *pl = (const char *)b + off;
    size_t pl_len = n - off;
    /* opcionalis payload type ("application/sdp\0"); ha "v=0"-val kezdodik, nincs */
    if (!(pl_len >= 3 && pl[0] == 'v' && pl[1] == '=')) {
        size_t tl = strnlen(pl, pl_len);
        if (tl >= pl_len) return -1;
        if (strncasecmp(pl, "application/sdp", 15) != 0) return -1;
        pl += tl + 1;
        pl_len -= tl + 1;
    }
    if (pl_len == 0) return -1;
    size_t c = pl_len < sdp_size - 1 ? pl_len : sdp_size - 1;
    memcpy(sdp, pl, c);
    sdp[c] = 0;
    *deletion = t_del;
    return 0;
}

int sap_find(const char *iface, const char *name_filter, int timeout_s,
             int list_only, char *out, size_t out_size)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("sap socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SAP_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "sap: bind(%d) hiba: %s\n", SAP_PORT, strerror(errno));
        close(fd);
        return -1;
    }
    int ifx = (iface && *iface) ? (int)if_nametoindex(iface) : 0;
    int joined = 0;
    for (size_t i = 0; i < sizeof(sap_groups) / sizeof(sap_groups[0]); i++)
        if (join(fd, sap_groups[i], ifx) == 0) joined++;
    if (!joined) {
        fprintf(stderr, "sap: multicast csatlakozas sikertelen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct { uint16_t hash; uint32_t src; } seen[SAP_MAX_SEEN];
    int nseen = 0, found = -1;

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint8_t buf[4096];
    char sdp[4096];

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long el_ms = (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
        long left = (long)timeout_s * 1000L - el_ms;
        if (left <= 0) break;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)(left > 500 ? 500 : left));
        if (pr < 0) { if (errno == EINTR) break; continue; }
        if (pr == 0) continue;

        struct sockaddr_in from; socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;

        int del = 0; uint16_t hash = 0;
        if (sap_decode(buf, (size_t)n, sdp, sizeof(sdp), &del, &hash) != 0 || del) continue;

        session_params p;
        int rc = sdp_parse_text(sdp, &p);

        if (list_only) {
            int dup = 0;
            for (int i = 0; i < nseen; i++)
                if (seen[i].hash == hash && seen[i].src == from.sin_addr.s_addr) dup = 1;
            if (dup) continue;
            if (nseen < SAP_MAX_SEEN) {
                seen[nseen].hash = hash; seen[nseen].src = from.sin_addr.s_addr; nseen++;
            }
            char src[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src));
            if (rc == SDP_OK)
                printf("  \"%s\"  %s:%u  %s/%u/%u  ptime=%g ms  refclk=%s  (kuldo: %s)\n",
                       p.session_name, p.address, p.port,
                       p.bytes_per_sample == 3 ? "L24" : "L16", p.sample_rate, p.channels,
                       p.ptime_ms,
                       p.clock_type == SDP_CLK_PTP ? "PTP" :
                       p.clock_type == SDP_CLK_8021AS ? "802.1AS" :
                       p.clock_type == SDP_CLK_NONE ? "nincs" : "egyeb", src);
            else
                printf("  (nem tamogatott stream, %s: %s)\n", src, sdp_strerror(rc));
            fflush(stdout);
            continue;
        }

        if (rc != SDP_OK) continue;
        if (name_filter && *name_filter && strcmp(p.session_name, name_filter) != 0 &&
            !strcasestr(p.session_name, name_filter))
            continue;

        size_t l = strlen(sdp);
        if (l >= out_size) l = out_size - 1;
        memcpy(out, sdp, l);
        out[l] = 0;
        found = 0;
        break;
    }
    close(fd);
    return list_only ? 0 : found;
}
