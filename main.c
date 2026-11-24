/*
 * A kód mesterséges intelligencia segítségével készült. Ezt az egyetemi szabályzatnak megfelelően itt jelölöm. [AI1]
 * aes67rx - AES67-2023 vevo.
 *
 *   aes67rx [opciok] <session.sdp | sap | sap:nev>
 *
 *   1. SDP fajlbol vagy SAP bejelentesbol (9., E.2), ellenorzes (7., 8.)
 *   2. PTP timeReceiver az SDP domainjevel (4., 8.2, A melleklet), lockra varakozas
 *   3. kapcsolodasi dontes a domain es a grandmaster alapjan (8.2)
 *   4. vetel, jitter puffer, PTP-hez kotott lejatszas allando link offsettel (5., 6., 7.4)
 *   5. futas kozben: szinkronallapot es grandmaster figyelese, a kuldo SAP-ban frissitett
 *      leirasanak kovetese (8.2), IGMP megujitas csendnel (10.2, 4. megj.)
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static struct {
    const char *iface, *dev;
    double link_ms;
    int domain, external, ptp, wait_s, verbose;
    long lock_ns;
} opt = { "eth0", NULL, 0.0, -1, 0, 1, 15, 0, 2000000 };

static void usage(const char *prog)
{
    fprintf(stderr,
"Hasznalat: %s [opciok] <session.sdp | sap | sap:nev>\n"
"  sap / sap:nev  az elso / az adott nevu (s=, reszlet is eleg) SAP-pal bejelentett stream\n"
"  -i IFACE   halozati interfesz (alap: eth0)\n"
"  -d ESZKOZ  ALSA eszkoz (alap: $AES67_ALSA_DEVICE vagy plughw:Headphones,0)\n"
"  -l MS      rogzitett link offset (alap: automatikus)\n"
"  -D N       PTP domain (alap: az SDP szerint, kulonben 0)\n"
"  -x         mar futo ptp4l hasznalata (pmc)\n"
"  -n         PTP nelkul, erkezesi ido alapjan (nem AES67-megfelelo)\n"
"  -T NS      PTP lock kuszob (alap: 2000000 ns)\n"
"  -w S       varakozas PTP lockra indulaskor (alap: 15 s)\n"
"  -L         SAP bejelentesek listazasa (10 s)\n"
"  -v         a ptp4l teljes naploja\n"
"Kornyezet: AES67_ALSA_DEVICE, AES67_ALSA_PERIOD (keret), AES67_ALSA_PERIODS (2-16, alap 3),\n"
"           AES67_TAI_OFFSET (alap 37), AES67_PTP4L_ARGS\n", prog);
}

static void print_sdp(const sdp_t *p)
{
    printf("Stream \"%s\": %s:%u (%s%s%s), PT %u, %s/%u Hz/%u csatorna, ptime %g ms\n",
           p->name, p->addr, p->port, p->multicast ? "multicast" : "unicast",
           p->source[0] ? ", forras " : "", p->source, p->pt, p->bps == 3 ? "L24" : "L16",
           p->rate, p->channels, p->ptime);
    for (int i = 0; i < p->nclk; i++)
        printf("  ts-refclk PTP GMID %s, domain %d\n", p->clk[i].gmid, p->clk[i].domain);
    if (p->traceable) printf("  ts-refclk traceable\n");
    if (p->have_mediaclk) printf("  mediaclk direct=%u\n", p->mediaclk);

    /* nem szabvanyos kuldo: csak figyelmeztetes, a vevo kezeli */
    const unsigned spp = (unsigned)(p->rate * p->ptime / 1000.0 + 0.5);
    const unsigned std = p->rate == 96000 ? (spp == 12 || spp == 24 || spp == 32 || spp == 96)
                                          : (spp == 6 || spp == 12 || spp == 16 || spp == 48 || spp == 192);
    if (!p->ptime) fprintf(stderr, "Megjegyzes: nincs a=ptime (8.1); a csomagmeretet a streambol vesszuk.\n");
    else if (!std) fprintf(stderr, "Megjegyzes: a ptime (%u minta) nem szerepel a 2. tablazatban (7.2).\n", spp);
    if (p->refclk != 1) fprintf(stderr, "FIGYELEM: nincs PTP a=ts-refclk (8.2).\n");
    if (!p->have_mediaclk) fprintf(stderr, "FIGYELEM: nincs a=mediaclk:direct= (8.3), 0 feltetelezve.\n");
}

/* 8.2: ne kapcsolodjunk, ha a domain elter es nem mindket referencia traceable (a sajatunkrol
 * ezt nem tudjuk); kapcsolodhatunk eltero grandmasterrel, de a szinkron hibas lehet. */
static int clock_ok(const sdp_t *p, int domain, const char *gm)
{
    int dom = !p->nclk, same_gm = !p->nclk || !gm[0];
    for (int i = 0; i < p->nclk; i++)
        if (p->clk[i].domain == domain) {
            dom = 1;
            same_gm |= !strcasecmp(p->clk[i].gmid, gm);
        }
    if (!dom) {
        fprintf(stderr, "HIBA: a stream PTP domainje (%d) nem a vevoe (%d): nem kapcsolodunk (8.2).\n",
                p->clk[0].domain, domain);
        return 0;
    }
    if (!same_gm)
        fprintf(stderr, "Megjegyzes: a stream grandmastere (%s) nem a vevoe (%s): kapcsolodunk, de "
                        "a szinkron hibas lehet (8.2).\n", p->clk[0].gmid, gm);
    return 1;
}

static int sap_wait(int fd, const char *name, sdp_t *out)
{
    char text[4096];
    int del;
    for (time_t end = time(NULL) + 60; !g_stop && time(NULL) < end; )
        if (sap_recv(fd, 500, text, sizeof(text), &del, NULL, 0) && !del && !sdp_parse(text, out) &&
            (!name || strcasestr(out->name, name)))
            return 0;
    return -1;
}

static void sap_list(int fd)
{
    char text[4096], from[64], key[160], seen[32][160];
    int del, n = 0;
    for (time_t end = time(NULL) + 10; !g_stop && time(NULL) < end; ) {
        if (!sap_recv(fd, 500, text, sizeof(text), &del, from, sizeof(from)) || del) continue;
        sdp_origin(text, key, sizeof(key));
        int dup = 0;
        for (int i = 0; i < n; i++) dup |= !strcmp(seen[i], key);
        if (dup) continue;
        if (n < 32) snprintf(seen[n++], sizeof(seen[0]), "%s", key);
        sdp_t p;
        const char *e = sdp_parse(text, &p);
        if (e) printf("  (%s: %s)\n", from, e);
        else printf("  \"%s\"  %s:%u  %s/%u/%u  ptime %g ms  (kuldo %s)\n", p.name, p.addr, p.port,
                    p.bps == 3 ? "L24" : "L16", p.rate, p.channels, p.ptime, from);
    }
}

/* 1 s SAP-figyeles: 1, ha a kuldo uj leirast hirdetett a streamhez (*sp frissitve). */
static int sap_update(int fd, sdp_t *sp)
{
    char text[4096], key[160];
    int del;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long left = 1000; left > 0 && !g_stop; ) {
        if (sap_recv(fd, (int)left, text, sizeof(text), &del, NULL, 0)) {
            sdp_origin(text, key, sizeof(key));
            if (!strcmp(key, sp->origin)) {
                sdp_t n;
                if (del) fprintf(stderr, "SAP: a kuldo visszavonta a streamet.\n");
                else if (!sdp_parse(text, &n) && memcmp(&n, sp, sizeof(n))) { *sp = n; return 1; }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        left = 1000 - (t.tv_sec - t0.tv_sec) * 1000 - (t.tv_nsec - t0.tv_nsec) / 1000000;
    }
    return 0;
}

/* Egy stream vetele. 0 = leallitas, 1 = uj leiras (*sp), -1 = hiba. */
static int run(sdp_t *sp, int sapfd, int domain)
{
    jb_t *jb = jb_create(sp->rate, sp->channels, sp->bps, sp->mediaclk, opt.ptp && ptp_locked());
    rx_t *rx = jb ? rx_start(sp, opt.iface, jb) : NULL;
    audio_t *au = rx ? audio_start(jb, sp->rate, sp->channels, opt.dev, opt.link_ms) : NULL;
    if (!au) {
        rx_stop(rx);
        jb_destroy(jb);
        return -1;
    }
    printf("A vevo fut (Ctrl+C: leallitas).\n");

    int ret = 0, idle = 0, locked = -1, mode = -1;
    uint64_t last_rx = 0, foreign = 0;
    char gm[24] = "", g[24];
    while (!g_stop) {
        if (sapfd < 0) sleep(1);
        else if (sap_update(sapfd, sp)) { ret = 1; break; }

        if (opt.ptp) {
            int l = ptp_locked();
            if (l != locked && locked >= 0)
                fprintf(stderr, l ? "PTP lock.\n" : "FIGYELEM: a PTP lock elveszett, a lejatszas a "
                                                    "helyi oraval folytatodik.\n");
            jb_allow_ptp(jb, locked = l);
            if (!ptp_gmid(g, sizeof(g)) && strcmp(g, gm)) {
                if (gm[0]) fprintf(stderr, "Megjegyzes: grandmaster valtas: %s -> %s\n", gm, g);
                strcpy(gm, g);
                clock_ok(sp, domain, gm);
            }
        }

        rx_stats_t rs;
        jb_stats_t js;
        audio_stats_t as;
        rx_get_stats(rx, &rs);
        jb_stats(jb, &js);
        audio_get_stats(au, &as);
        if (js.ready && js.mode != mode) {
            mode = js.mode;
            printf("Lejatszas: %s, %u minta/csomag (%.3f ms)\n",
                   mode == JB_PTP ? "PTP media ora (AES67)" : "erkezesi ido (a kuldo nem PTP-szinkron)",
                   js.pkt_frames, js.pkt_frames * 1000.0 / sp->rate);
        }
        char ptp[40] = "ki";
        if (opt.ptp) snprintf(ptp, sizeof(ptp), "%s%+ld ns", ptp_locked() ? "" : "NINCS LOCK ", ptp_offset());
        printf("RX %llu lost %llu late %llu dup %llu | jit %.2f tr %.2f buf %.1f ms | %s L %.1f ms "
               "err %+.1f us corr %+.1f ppm xrun %llu resync %llu | PTP %s\n",
               (unsigned long long)js.received, (unsigned long long)js.lost,
               (unsigned long long)js.late, (unsigned long long)js.dup, js.jitter_ms, js.transit_ms,
               js.fill_ms, !as.synced ? "----" : mode == JB_PTP ? "PTP" : "ARR", as.link_ms,
               as.err_us, as.ppm, (unsigned long long)as.xruns, (unsigned long long)as.resyncs, ptp);
        if (rs.foreign != foreign)
            printf("   eldobva: %llu (idegen PT/SSRC vagy hibas RTP)\n", (unsigned long long)(foreign = rs.foreign));

        if (rs.packets != last_rx) { idle = 0; last_rx = rs.packets; continue; }
        if (++idle == 3 || idle % 10 == 0) rx_rejoin(rx);
        if (idle == 3) fprintf(stderr, "Nincs bejovo csomag: IGMP tagsag megujitva.\n");
        if (idle == 10) fprintf(stderr, "FIGYELEM: 10 s ota nincs csomag (fut a kuldo? jo az -i?)\n");
    }
    audio_stop(au);
    rx_stop(rx);
    jb_destroy(jb);
    return ret;
}

int main(int argc, char **argv)
{
    int c, list = 0;
    while ((c = getopt(argc, argv, "i:d:l:D:xnT:w:Lvh")) != -1)
        switch (c) {
        case 'i': opt.iface = optarg; break;
        case 'd': opt.dev = optarg; break;
        case 'l': opt.link_ms = atof(optarg); break;
        case 'D': opt.domain = atoi(optarg); break;
        case 'x': opt.external = 1; break;
        case 'n': opt.ptp = 0; break;
        case 'T': opt.lock_ns = atol(optarg); break;
        case 'w': opt.wait_s = atoi(optarg); break;
        case 'L': list = 1; break;
        case 'v': opt.verbose = 1; break;
        default:  usage(argv[0]); return c != 'h';
        }
    if (!list && optind >= argc) { usage(argv[0]); return 1; }

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *src = list ? "sap" : argv[optind];
    int sapfd = -1;
    sdp_t sp;
    if (!strncmp(src, "sap", 3) && (!src[3] || src[3] == ':')) {
        if ((sapfd = sap_open(opt.iface)) < 0) { perror("SAP (9875)"); return 1; }
        if (list) {
            printf("SAP bejelentesek (10 s):\n");
            sap_list(sapfd);
            return 0;
        }
        printf("SAP: varakozas a stream bejelentesere (max 60 s)...\n");
        if (sap_wait(sapfd, src[3] ? src + 4 : NULL, &sp)) {
            fprintf(stderr, "Nem erkezett megfelelo SAP bejelentes (lista: %s -L).\n", argv[0]);
            return 1;
        }
    } else {
        static char text[16384];
        FILE *f = fopen(src, "r");
        size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
        if (f) fclose(f);
        text[n] = 0;
        const char *e = f ? sdp_parse(text, &sp) : "a fajl nem nyithato meg";
        if (e) { fprintf(stderr, "SDP hiba (%s): %s\n", src, e); return 1; }
    }
    struct rlimit ml;                    /* lapozas ellen; felhasznalokent a limit szukos */
    if (geteuid() && (getrlimit(RLIMIT_MEMLOCK, &ml) || ml.rlim_cur != RLIM_INFINITY))
        fprintf(stderr, "Megjegyzes: nem root: mlockall kihagyva.\n");
    else if (mlockall(MCL_CURRENT | MCL_FUTURE))
        fprintf(stderr, "Megjegyzes: mlockall nem sikerult (lapozas kimaradast okozhat).\n");
    if (!opt.ptp) fprintf(stderr, "FIGYELEM: PTP nelkul (-n) a lejatszas nem AES67-megfelelo.\n");

    int rc = 0, domain = -1;
    while (!g_stop) {
        print_sdp(&sp);
        int d = sp.nclk && opt.domain < 0 ? sp.clk[0].domain : opt.domain < 0 ? 0 : opt.domain;
        if (opt.ptp && d != domain) {
            if (domain >= 0) ptp_stop();
            if (ptp_start(opt.iface, d, opt.external, opt.lock_ns, opt.verbose)) { rc = -1; break; }
            domain = d;
            printf("Varakozas PTP lockra (max %d s)...\n", opt.wait_s);
            for (int i = 0; i < opt.wait_s * 10 && !g_stop && !ptp_locked(); i++) usleep(100000);
            char gm[24] = "";
            ptp_gmid(gm, sizeof(gm));
            if (ptp_locked()) printf("PTP lock: offset %+ld ns, grandmaster %s\n", ptp_offset(), gm);
            else fprintf(stderr, "Megjegyzes: meg nincs PTP lock; addig erkezesi ido alapjan "
                                 "jatszunk, utana automatikusan PTP modra valtunk.\n");
        }
        char gm[24] = "";
        ptp_gmid(gm, sizeof(gm));
        if (g_stop || (opt.ptp && !clock_ok(&sp, domain, gm))) { rc = g_stop ? 0 : -1; break; }
        if ((rc = run(&sp, sapfd, domain)) <= 0) break;
        printf("A kuldo uj leirast hirdetett: ujraindulas.\n");
    }
    if (domain >= 0) ptp_stop();
    return rc < 0;
}
