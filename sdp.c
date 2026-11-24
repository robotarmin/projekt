/*
 * SDP (RFC 8866, RFC 7273) es SAP (RFC 2974).
 *
 *   8.1   ptime: tizedes ponttal, tetszoleges pontossaggal ("1", "0.250", "1.000000", "4.")
 *   8.2   ts-refclk: IEEE1588-2008 (GMID:domain vagy traceable), IEEE802.1AS; tobb forras
 *   8.3   mediaclk:direct=<offset>
 *   8.4   a payload formatumot az rtpmap adja, nem a PT erteke
 *   8.5.0 korabbi hibak tolerálva (sendonly multicastnal, domain-nmbr=)
 *   RFC 4570 a=source-filter: a forrasspecifikus (SSM, IGMPv3) csatlakozashoz
 *   E.2   SAP v2: 239.255.255.255 (adminisztrativ) es 224.2.127.254 (globalis), 9875
 * Ismeretlen attributumokat figyelmen kivul hagyunk (RFC 8866).
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>

void sdp_origin(const char *text, char *out, size_t n)
{
    char user[64], id[64], net[16], type[16], addr[64];
    out[0] = 0;
    const char *l = text;
    while (l && strncmp(l, "o=", 2)) {
        l = strchr(l, '\n');
        if (l) l++;
    }
    if (l && sscanf(l + 2, "%63s %63s %*s %15s %15s %63s", user, id, net, type, addr) == 5)
        snprintf(out, n, "%s %s %s %s %s", user, id, net, type, addr);
}

/* ptp=IEEE1588-2008:39-A7-94-FF-FE-07-CB-D0:0 | ptp=IEEE1588-2008:traceable |
 * ptp=IEEE802.1AS-2011:39-A7-94-FF-FE-07-CB-D0 | local | ntp=... */
static void parse_refclk(sdp_t *o, const char *v)
{
    if (strncmp(v, "ptp=", 4)) {
        if (!o->refclk) o->refclk = 2;
        return;
    }
    o->refclk = 1;
    const int as = !strncmp(v + 4, "IEEE802.1AS", 11);
    const char *c = strchr(v + 4, ':');
    if (!c++) return;
    if (!strncasecmp(c, "traceable", 9)) { o->traceable = 1; return; }
    if (o->nclk >= MAX_REFCLK) return;

    char *gm = o->clk[o->nclk].gmid;
    if (sscanf(c, "%23[0-9A-Fa-f-]", gm) != 1) return;
    for (char *q = gm; *q; q++) *q = (char)toupper((unsigned char)*q);
    const char *d = strchr(c, ':');
    int dom = 0;                                    /* 802.1AS: mindig 0 (8.2) */
    if (d && !as) {
        d++;
        if (!strncmp(d, "domain-nmbr=", 12)) d += 12;
        if (!strncmp(d, "traceable", 9)) o->traceable = 1;
        else dom = atoi(d);
    }
    o->clk[o->nclk++].domain = dom;
}

const char *sdp_parse(const char *text, sdp_t *o)
{
    memset(o, 0, sizeof(*o));
    sdp_origin(text, o->origin, sizeof(o->origin));

    int section = 0;                  /* 0 session, 1 az elso RTP/AVP audio, 2 mas media */
    int have_c = 0, ip6 = 0, inactive = 0;
    unsigned rate = 0, ch = 0;
    char enc[16] = "", line[1024];

    for (const char *p = text; *p; ) {
        size_t len = strcspn(p, "\r\n");
        snprintf(line, sizeof(line), "%.*s", (int)len, p);
        p += len;
        p += strspn(p, "\r\n");
        for (char *e = line + strlen(line); e > line && isspace((unsigned char)e[-1]); ) *--e = 0;

        if (!strncmp(line, "m=", 2)) {
            if (section == 1) break;
            unsigned port = 0, f = 0;
            char proto[32];
            if (sscanf(line, "m=audio %u %31s %u", &port, proto, &f) == 3 &&
                !strncmp(proto, "RTP/AVP", 7)) {
                section = 1;
                o->port = (uint16_t)port;
                o->pt = (uint8_t)f;
            } else
                section = 2;
            continue;
        }
        if (section == 2) continue;

        if (!strncmp(line, "s=", 2) && section == 0)
            snprintf(o->name, sizeof(o->name), "%.127s", line + 2);
        else if (!strncmp(line, "c=", 2)) {
            char net[8], type[8], addr[64];
            if (sscanf(line + 2, "%7s %7s %63[^/ ]", net, type, addr) == 3) {
                if (strcmp(type, "IP4")) ip6 = 1;
                else { snprintf(o->addr, sizeof(o->addr), "%s", addr); have_c = 1; }
            }
        }
        else if (!strncmp(line, "a=rtpmap:", 9) && section == 1) {
            unsigned p2, r = 0, c = 1;
            char e[16];
            if (sscanf(line + 9, "%u %15[^/]/%u/%u", &p2, e, &r, &c) >= 3 && p2 == o->pt) {
                snprintf(enc, sizeof(enc), "%s", e);
                rate = r;
                ch = c;
            }
        }
        else if (!strncmp(line, "a=ptime:", 8))
            o->ptime = strtod(line + 8, NULL);
        else if (!strncmp(line, "a=mediaclk:direct=", 18)) {
            o->have_mediaclk = 1;
            o->mediaclk = (uint32_t)strtoul(line + 18, NULL, 10);
        }
        else if (!strncmp(line, "a=ts-refclk:", 12))
            parse_refclk(o, line + 12);
        else if (!strncmp(line, "a=source-filter:", 16))
            sscanf(line + 16, " incl IN IP4 %*s %63s", o->source);
        else if (!strcmp(line, "a=inactive"))
            inactive = 1;
    }

    if (!o->port)  return "nincs RTP/AVP audio media (m=audio)";
    if (!enc[0])   return "nincs a=rtpmap a media payload type-jahoz";
    if (inactive)  return "a stream inaktiv (a=inactive)";
    if (!have_c)   return ip6 ? "IPv6 nem tamogatott (6.1.1)" : "nincs c= cim";
    if (!strcasecmp(enc, "L16"))      o->bps = 2;
    else if (!strcasecmp(enc, "L24")) o->bps = 3;
    else return "nem L16/L24 kodolas (7.1)";
    if (rate != 44100 && rate != 48000 && rate != 96000) return "nem 44,1/48/96 kHz (7.1)";
    if (ch < 1 || ch > 64) return "ervenytelen csatornaszam";
    o->rate = rate;
    o->channels = (uint8_t)ch;
    if (o->ptime < 0.0 || o->ptime > 1000.0) o->ptime = 0.0;

    struct in_addr a;
    if (inet_pton(AF_INET, o->addr, &a) != 1) return "hibas c= cim";
    o->multicast = IN_MULTICAST(ntohl(a.s_addr));
    if (!o->multicast || inet_pton(AF_INET, o->source, &a) != 1) o->source[0] = 0;
    return NULL;
}

/* ------------------------------------------------------------------ */

int sap_open(const char *iface)
{
    static const char *groups[] = { "239.255.255.255", "224.2.127.254" };
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), one = 1, joined = 0;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(9875) };
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }

    struct ip_mreqn m = { .imr_ifindex = (int)if_nametoindex(iface) };
    for (size_t i = 0; i < 2; i++) {
        inet_pton(AF_INET, groups[i], &m.imr_multiaddr);
        joined += setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) == 0;
    }
    if (!joined) { close(fd); return -1; }
    return fd;
}

int sap_recv(int fd, int timeout_ms, char *sdp, size_t n, int *deleted,
             char *from, size_t from_n)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0) return 0;

    uint8_t b[4096];
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    ssize_t len = recvfrom(fd, b, sizeof(b) - 1, 0, (struct sockaddr *)&sa, &sl);
    if (len < 8) return 0;
    b[len] = 0;

    /* V=1, nincs titkositas es tomorites; fejlec: 4 B + forrascim + hitelesites */
    if ((b[0] >> 5) != 1 || (b[0] & 3)) return 0;
    size_t off = 4 + ((b[0] & 0x10) ? 16 : 4) + (size_t)b[1] * 4;
    if (off >= (size_t)len) return 0;
    const char *pl = (const char *)b + off;
    if (strncmp(pl, "v=0", 3) && strncmp(pl, "o=", 2)) {      /* payload type mezo */
        if (strcasecmp(pl, "application/sdp")) return 0;
        pl += strlen(pl) + 1;
        if (pl >= (const char *)b + len) return 0;
    }
    snprintf(sdp, n, "%s", pl);
    *deleted = (b[0] >> 2) & 1;
    if (from) inet_ntop(AF_INET, &sa.sin_addr, from, (socklen_t)from_n);
    return 1;
}
