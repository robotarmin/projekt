/*
 * SDP feldolgozas (RFC 8866, RFC 7273) es a fogado socket kezelese.
 *
 * AES67-2023 vonatkozo pontjai:
 *   8.1  ptime / maxptime: tizedes ponttal, tetszoleges pontossaggal ertelmezve
 *   8.2  ts-refclk: IEEE1588-2008 (GMID + domain, vagy traceable), IEEE802.1AS
 *   8.3  mediaclk:direct=<offset>
 *   8.4  a payload formatumot az rtpmap hatarozza meg, nem a PT szama
 *   8.5.0 a korabbi hibas leirasok (sendonly multicastnal, domain-nmbr=) tolerálva
 *   6.1  IPv4, unicast es multicast vetel; 10.2 IGMP (a kernel v3-mal indul)
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49
#endif

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

/* a=ts-refclk:ptp=IEEE1588-2008:39-A7-94-FF-FE-07-CB-D0:0
 * a=ts-refclk:ptp=IEEE1588-2008:traceable
 * a=ts-refclk:ptp=IEEE802.1AS-2011:39-A7-94-FF-FE-07-CB-D0
 * Tobb ts-refclk sor eseten az elso GMID-es PTP forras szamit (8.2). */
static void parse_refclk(const char *p, session_params *out)
{
    if (strncasecmp(p, "ptp=", 4) != 0) {
        if (out->clock_type == SDP_CLK_NONE) out->clock_type = SDP_CLK_OTHER;
        return;
    }
    p += 4;
    int type = (strncasecmp(p, "IEEE802.1AS", 11) == 0) ? SDP_CLK_8021AS : SDP_CLK_PTP;
    if (out->clock_type != SDP_CLK_PTP && out->clock_type != SDP_CLK_8021AS)
        out->clock_type = type;

    const char *c = strchr(p, ':');
    if (!c) return;
    c++;
    if (strncasecmp(c, "traceable", 9) == 0) { out->traceable = 1; return; }

    if (out->gmid[0] == 0) {
        size_t n = 0;
        while (c[n] && c[n] != ':' && !isspace((unsigned char)c[n]) &&
               n < sizeof(out->gmid) - 1) {
            out->gmid[n] = (char)toupper((unsigned char)c[n]);
            n++;
        }
        out->gmid[n] = 0;
        const char *d = strchr(c, ':');
        if (d) {
            d++;
            if (strncasecmp(d, "domain-nmbr=", 12) == 0) d += 12;   /* 8.5.0 errata */
            if (isdigit((unsigned char)*d)) out->ptp_domain = atoi(d);
        }
        if (strncasecmp(c, "traceable", 9) != 0 && strstr(c, ":traceable"))
            out->traceable = 1;
    }
    if (type == SDP_CLK_8021AS) out->ptp_domain = 0;    /* 802.1AS: mindig 0 (8.2) */
}

int sdp_parse_text(const char *text, session_params *out)
{
    if (!text || !out) return SDP_ERR_OPEN;

    memset(out, 0, sizeof(*out));
    out->ptp_domain = -1;
    out->direction  = SDP_DIR_SENDRECV;

    int have_m = 0, foreign = 0, have_c = 0, have_rtpmap = 0, c_ip6 = 0;
    unsigned pt_from_m = 0;
    char enc[16] = {0};
    int session_level_dir = -1;

    const char *p = text;
    char line[2048];
    while (*p) {
        size_t n = strcspn(p, "\r\n");
        size_t m = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, p, m);
        line[m] = 0;
        p += n;
        while (*p == '\r' || *p == '\n') p++;

        char *s = trim(line);
        if (*s == 0) continue;

        if (s[0] == 'm' && s[1] == '=') {
            if (have_m) break;                     /* csak az elso audio stream */
            unsigned port = 0; char proto[32] = {0}, fmts[128] = {0};
            if (sscanf(s + 2, "audio %u %31s %127[^\n]", &port, proto, fmts) != 3 ||
                strncmp(proto, "RTP/AVP", 7) != 0) {
                foreign = 1;                       /* masik media: kihagyjuk */
                continue;
            }
            foreign = 0;
            out->port = (uint16_t)port;
            pt_from_m = (unsigned)strtoul(fmts, NULL, 10);
            have_m = 1;
            continue;
        }
        if (foreign) continue;

        if (s[0] == 's' && s[1] == '=' && !have_m) {
            snprintf(out->session_name, sizeof(out->session_name), "%s", s + 2);
        }
        else if (s[0] == 'o' && s[1] == '=') {
            char u[64], id[64], ver[64], net[8], at[8], addr[64];
            if (sscanf(s + 2, "%63s %63s %63s %7s %7s %63s", u, id, ver, net, at, addr) == 6)
                snprintf(out->origin_addr, sizeof(out->origin_addr), "%s", addr);
        }
        else if (s[0] == 'c' && s[1] == '=') {
            char net[8], atype[8], addr[64];
            if (sscanf(s + 2, "%7s %7s %63s", net, atype, addr) == 3) {
                if (strcasecmp(atype, "IP4") != 0) { c_ip6 = 1; continue; }
                char *slash = strchr(addr, '/');
                if (slash) *slash = 0;
                snprintf(out->address, sizeof(out->address), "%s", addr);
                have_c = 1;
            }
        }
        else if (strncmp(s, "a=rtpmap:", 9) == 0) {
            unsigned pt = 0, rate = 0, ch = 1; char e[16] = {0};
            int k = sscanf(s + 9, "%u %15[^/]/%u/%u", &pt, e, &rate, &ch);
            if (k >= 3 && have_m && pt == pt_from_m) {
                snprintf(enc, sizeof(enc), "%s", e);
                out->sample_rate = rate;
                out->channels    = (uint8_t)(ch > 255 ? 255 : ch);
                have_rtpmap = 1;
            }
        }
        else if (strncmp(s, "a=ptime:", 8) == 0) {
            double v = strtod(s + 8, NULL);          /* "1", "0.250", "4.", "0.12" */
            if (v > 0.0 && v <= 1000.0) out->ptime_ms = v;
        }
        else if (strncmp(s, "a=maxptime:", 11) == 0) {
            double v = strtod(s + 11, NULL);
            if (v > 0.0 && v <= 1000.0) out->maxptime_ms = v;
        }
        else if (strncmp(s, "a=mediaclk:", 11) == 0) {
            const char *mc = s + 11;
            if (strncmp(mc, "direct", 6) == 0) {
                out->have_mediaclk = 1;
                const char *eq = strchr(mc, '=');
                out->mediaclk_offset = eq ? (uint32_t)strtoull(eq + 1, NULL, 10) : 0;
            }
        }
        else if (strncmp(s, "a=ts-refclk:", 12) == 0) {
            parse_refclk(s + 12, out);
        }
        else if (!strcmp(s, "a=recvonly") || !strcmp(s, "a=sendrecv") ||
                 !strcmp(s, "a=sendonly") || !strcmp(s, "a=inactive")) {
            int d = !strcmp(s, "a=recvonly") ? SDP_DIR_RECVONLY :
                    !strcmp(s, "a=sendrecv") ? SDP_DIR_SENDRECV :
                    !strcmp(s, "a=sendonly") ? SDP_DIR_SENDONLY : SDP_DIR_INACTIVE;
            if (have_m) out->direction = d; else session_level_dir = d;
        }
        /* minden mas attributum: figyelmen kivul hagyva (RFC 8866) */
    }
    if (out->direction == SDP_DIR_SENDRECV && session_level_dir >= 0)
        out->direction = session_level_dir;

    if (!have_m || !have_rtpmap) return SDP_ERR_MISSING;
    if (!have_c) return c_ip6 ? SDP_ERR_SYNTAX : SDP_ERR_MISSING;   /* IPv6 nem tamogatott (6.1.1) */
    if (out->direction == SDP_DIR_INACTIVE) return SDP_ERR_DIRECTION;

    out->payload_type = (uint8_t)pt_from_m;

    if (!strcasecmp(enc, "L16"))      out->bytes_per_sample = 2;
    else if (!strcasecmp(enc, "L24")) out->bytes_per_sample = 3;
    else return SDP_ERR_CODEC;

    if (out->sample_rate != 44100 && out->sample_rate != 48000 && out->sample_rate != 96000)
        return SDP_ERR_RATE;
    if (out->channels < 1 || out->channels > 64) return SDP_ERR_CHANNELS;

    /* 8.1: mintaszam = ptime * mintaveteli frekvencia, egeszre kerekitve */
    if (out->ptime_ms > 0.0)
        out->packet_samples = (uint32_t)((double)out->sample_rate * out->ptime_ms / 1000.0 + 0.5);

    struct in_addr a;
    if (inet_pton(AF_INET, out->address, &a) != 1) return SDP_ERR_SYNTAX;
    out->is_multicast = IN_MULTICAST(ntohl(a.s_addr)) ? 1 : 0;

    if (out->clock_type == SDP_CLK_PTP && out->ptp_domain < 0 && out->gmid[0])
        out->ptp_domain = 0;
    return SDP_OK;
}

int sdp_parse_file(const char *path, session_params *out)
{
    if (!path || !out) return SDP_ERR_OPEN;
    FILE *f = fopen(path, "r");
    if (!f) return SDP_ERR_OPEN;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return sdp_parse_text(buf, out);
}

const char *sdp_strerror(int rc)
{
    switch (rc) {
    case SDP_OK:            return "ok";
    case SDP_ERR_OPEN:      return "a fajl nem nyithato meg";
    case SDP_ERR_SYNTAX:    return "szintaktikai hiba (vagy nem IPv4 cim)";
    case SDP_ERR_MISSING:   return "hianyzo m=audio / c= / a=rtpmap sor";
    case SDP_ERR_CODEC:     return "nem L16/L24 kodolas (7.1)";
    case SDP_ERR_RATE:      return "nem 44.1/48/96 kHz (7.1)";
    case SDP_ERR_CHANNELS:  return "ervenytelen csatornaszam";
    case SDP_ERR_DIRECTION: return "a stream inaktiv (a=inactive)";
    default:                return "ismeretlen hiba";
    }
}

int sdp_ptime_is_standard(const session_params *p)
{
    if (!p || p->ptime_ms <= 0.0) return -1;
    /* 2. tablazat: 6, 12, 16, 48, 192 minta (48/44.1 kHz) ill. 12, 24, 32, 96 (96 kHz) */
    static const unsigned s48[] = { 6, 12, 16, 48, 192 };
    static const unsigned s96[] = { 12, 24, 32, 96 };
    const unsigned *t = (p->sample_rate == 96000) ? s96 : s48;
    size_t n = (p->sample_rate == 96000) ? 4 : 5;
    for (size_t i = 0; i < n; i++)
        if (p->packet_samples == t[i]) return 1;
    return 0;
}

/* ------------------------------------------------------------ */

struct aes67_session {
    session_params params;
    int            fd;
    int            ifindex;
    int            joined;
    struct in_addr group;
};

static int igmp(aes67_session *s, int opt)
{
    struct ip_mreqn m;
    memset(&m, 0, sizeof(m));
    m.imr_multiaddr = s->group;
    m.imr_ifindex   = s->ifindex;
    return setsockopt(s->fd, IPPROTO_IP, opt, &m, sizeof(m));
}

aes67_session *session_open(const session_params *p, const char *iface)
{
    if (!p) return NULL;
    aes67_session *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->fd = -1;
    s->params = *p;

    if (inet_pton(AF_INET, s->params.address, &s->group) != 1) { free(s); return NULL; }

    if (iface && *iface) {
        s->ifindex = (int)if_nametoindex(iface);
        if (s->ifindex == 0)
            fprintf(stderr, "session: ismeretlen interface (%s), az alapertelmezettet hasznalom\n",
                    iface);
    }

    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0) { perror("socket"); free(s); return NULL; }

    int one = 1, zero = 0;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int rcvbuf = 2 << 20;
    setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    /* csak a sajat csoportunk forgalma erkezzen ide (mas socketek tagsaga ne) */
    setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof(zero));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(s->params.port);
    if (s->params.is_multicast) sa.sin_addr = s->group;
    else                        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "session: bind(%u) hiba: %s\n", s->params.port, strerror(errno));
        goto fail;
    }

    if (s->params.is_multicast) {
        if (igmp(s, IP_ADD_MEMBERSHIP) < 0) {
            fprintf(stderr, "session: multicast csatlakozas hiba: %s\n", strerror(errno));
            goto fail;
        }
        s->joined = 1;
    }
    return s;

fail:
    close(s->fd);
    free(s);
    return NULL;
}

int session_rejoin(aes67_session *s)
{
    if (!s || !s->joined) return -1;
    igmp(s, IP_DROP_MEMBERSHIP);
    return igmp(s, IP_ADD_MEMBERSHIP);
}

void session_close(aes67_session *s)
{
    if (!s) return;
    if (s->fd >= 0) {
        if (s->joined) igmp(s, IP_DROP_MEMBERSHIP);
        close(s->fd);
    }
    free(s);
}

int session_get_fd(const aes67_session *s)      { return s ? s->fd : -1; }
int session_get_ifindex(const aes67_session *s) { return s ? s->ifindex : 0; }
const session_params *session_get_params(const aes67_session *s) { return s ? &s->params : NULL; }
