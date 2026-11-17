/*
 * aes67rx - AES67-2023 vevo, szoftveres PTP idobelyegzessel.
 *
 * Inditas:
 *   aes67rx [opciok] <session.sdp | sap | sap:nev>
 *
 * Lepesek:
 *   1. SDP: fajlbol vagy SAP bejelentesbol (9., E.2), ellenorzes (7., 8.)
 *   2. PTP timeReceiver inditasa az SDP-ben megadott domainnel (4., 8.2, A melleklet)
 *   3. 8.2 szerinti kapcsolodasi dontes: domain / GMID / traceable osszevetes
 *   4. multicast/unicast vetel (6.1, 10.2), jitter puffer, RTCP (6.3)
 *   5. PTP-hez kotott lejatszas allando link offsettel (5., 7.4)
 *   6. folyamatos szinkronallapot-figyeles (8.2), IGMP ujrakuldes (10.2, 4. megj.)
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <sys/mman.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void usage(const char *prog)
{
    fprintf(stderr,
"Hasznalat: %s [opciok] <session.sdp | sap | sap:nev>\n"
"\n"
"  session.sdp     AES67 SDP leiro fajl\n"
"  sap             az elso SAP-pal bejelentett L16/L24 stream\n"
"  sap:nev         az adott nevu (s=) SAP stream (reszlet is eleg)\n"
"\n"
"Opciok:\n"
"  -i IFACE   halozati interfesz (PTP es multicast), alap: eth0\n"
"  -d ESZKOZ  ALSA eszkoz (alap: $AES67_ALSA_DEVICE vagy plughw:Headphones,0)\n"
"  -l MS      allando link offset ms-ban (alap: automatikus)\n"
"  -D N       PTP domain feluliras (alap: az SDP ts-refclk szerint, kulonben 0)\n"
"  -x         kulso, mar futo ptp4l hasznalata (pmc-vel figyelve)\n"
"  -n         PTP nelkul (csak erkezesi-ido alapu, NEM AES67-megfelelo mod)\n"
"  -T NS      PTP lock kuszob ns-ban (alap: 2000000; szoftveres idobelyegzeshez)\n"
"  -w S       PTP lockra varakozas inditaskor (alap: 15; kesobbi locknal a vevo\n"
"             automatikusan PTP modra valt)\n"
"  -L         SAP bejelentesek listazasa (10 s) es kilepes\n"
"  -v         a ptp4l teljes naplojanak kiirasa (PTP hibakereseshez)\n"
"  -h         sugo\n"
"\n"
"Kornyezeti valtozok: AES67_ALSA_DEVICE, AES67_ALSA_PERIOD (keret), AES67_ALSA_PERIODS (2-16),\n"
"  AES67_TAI_OFFSET (s, alap 37), AES67_PTP4L_ARGS (tovabbi ptp4l kapcsolok)\n",
        prog);
}

static const char *clk_name(int t)
{
    switch (t) {
    case SDP_CLK_PTP:    return "IEEE1588-2008";
    case SDP_CLK_8021AS: return "IEEE802.1AS";
    case SDP_CLK_OTHER:  return "nem PTP";
    default:             return "nincs megadva";
    }
}

static void print_sdp(const session_params *p)
{
    printf("Stream: \"%s\"\n", p->session_name[0] ? p->session_name : "(nevtelen)");
    printf("  cim      %s:%u (%s)  PT=%u\n", p->address, p->port,
           p->is_multicast ? "multicast" : "unicast", p->payload_type);
    printf("  formatum %s, %u Hz, %u csatorna\n",
           p->bytes_per_sample == 3 ? "L24" : "L16", p->sample_rate, p->channels);
    if (p->ptime_ms > 0) {
        printf("  ptime    %g ms (%u minta/csomag)", p->ptime_ms, p->packet_samples);
        if (p->maxptime_ms > 0) printf("  maxptime %g ms", p->maxptime_ms);
        printf("\n");
    } else
        printf("  ptime    nincs megadva\n");
    printf("  ora      %s%s%s", clk_name(p->clock_type),
           p->gmid[0] ? "  GMID=" : "", p->gmid);
    if (p->ptp_domain >= 0) printf("  domain=%d", p->ptp_domain);
    if (p->traceable) printf("  traceable");
    printf("\n");
    if (p->have_mediaclk) printf("  mediaclk direct=%u\n", p->mediaclk_offset);
}

/* AES67 megfeleloseg ellenorzese a leiras alapjan: csak figyelmeztetesek */
static void check_sdp(const session_params *p)
{
    int st = sdp_ptime_is_standard(p);
    if (st == 0)
        fprintf(stderr, "Megjegyzes: a ptime (%g ms = %u minta) nem szerepel az AES67 2. tablazataban "
                        "(0.125/0.25/0.333/1/4 ms). A vevo kezeli, de a kuldo nem szabvanyos (7.2).\n",
                p->ptime_ms, p->packet_samples);
    if (st < 0)
        fprintf(stderr, "Megjegyzes: hianyzik az a=ptime (8.1 kotelezo). A csomagmeretet a streambol vesszuk.\n");
    if (p->clock_type == SDP_CLK_NONE)
        fprintf(stderr, "FIGYELEM: hianyzik az a=ts-refclk (8.2 kotelezo). A kuldo oraja ismeretlen.\n");
    if (!p->have_mediaclk)
        fprintf(stderr, "FIGYELEM: hianyzik az a=mediaclk:direct= (8.3 kotelezo). Offset = 0 feltetelezve.\n");
    if (p->channels > 8)
        fprintf(stderr, "Megjegyzes: %u csatorna (> 8, AES67 7.3 szerint opcionalis).\n", p->channels);
    if (p->direction == SDP_DIR_SENDONLY && p->is_multicast)
        fprintf(stderr, "Megjegyzes: a=sendonly multicast leirasban (8.5.0 errata), elfogadva.\n");
}

int main(int argc, char **argv)
{
    const char *iface = "eth0", *device = NULL;
    double link_ms = 0.0;
    int domain_override = -1, ptp_mode = PTP_MODE_INTERNAL, use_ptp = 1;
    int wait_s = 15, list_sap = 0, verbose = 0;
    long lock_ns = 2000000L;

    int opt;
    while ((opt = getopt(argc, argv, "i:d:l:D:xnT:w:Lvh")) != -1) {
        switch (opt) {
        case 'i': iface = optarg; break;
        case 'd': device = optarg; break;
        case 'l': link_ms = atof(optarg); break;
        case 'D': domain_override = atoi(optarg); break;
        case 'x': ptp_mode = PTP_MODE_EXTERNAL; break;
        case 'n': use_ptp = 0; break;
        case 'T': lock_ns = atol(optarg); break;
        case 'w': wait_s = atoi(optarg); break;
        case 'L': list_sap = 1; break;
        case 'v': verbose = 1; break;
        default:  usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    install_signal_handlers();
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (list_sap) {
        printf("SAP bejelentesek (10 s):\n");
        return sap_find(iface, NULL, 10, 1, NULL, 0) == 0 ? 0 : 1;
    }
    if (optind >= argc) { usage(argv[0]); return 1; }
    const char *src = argv[optind];

    /* ---- 1. SDP ---- */
    session_params sp;
    int rc;
    if (!strcmp(src, "sap") || !strncmp(src, "sap:", 4)) {
        const char *name = src[3] == ':' ? src + 4 : NULL;
        printf("SAP: varakozas a stream bejelentesere%s%s%s (max 60 s)...\n",
               name ? " (\"" : "", name ? name : "", name ? "\")" : "");
        fflush(stdout);
        static char sdp_text[8192];
        if (sap_find(iface, name, 60, 0, sdp_text, sizeof(sdp_text)) != 0) {
            fprintf(stderr, "Nem erkezett megfelelo SAP bejelentes. Listazas: %s -L\n", argv[0]);
            return 1;
        }
        rc = sdp_parse_text(sdp_text, &sp);
    } else {
        rc = sdp_parse_file(src, &sp);
    }
    if (rc != SDP_OK) {
        fprintf(stderr, "SDP hiba: %s (%s)\n", sdp_strerror(rc), src);
        return 1;
    }
    print_sdp(&sp);
    check_sdp(&sp);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "Megjegyzes: mlockall nem sikerult (lapozas okozhat kimaradast).\n");

    /* ---- 2. PTP ---- */
    int ptp_started = 0, locked = 0;
    int domain = domain_override >= 0 ? domain_override : (sp.ptp_domain >= 0 ? sp.ptp_domain : 0);
    char gm[32] = "";

    if (use_ptp) {
        ptp_slave_set_lock_threshold(lock_ns);
        ptp_slave_set_verbose(verbose);
        printf("PTP: %s, interfesz %s, domain %d\n",
               ptp_mode == PTP_MODE_INTERNAL ? "ptp4l -S (sajat)" : "kulso ptp4l (pmc)",
               iface, domain);
        if (ptp_slave_start(iface, domain, ptp_mode) != 0) {
            fprintf(stderr, "A PTP nem indithato. Root jog es linuxptp szukseges; "
                            "ha mar fut ptp4l szolgaltatas, hasznald a -x kapcsolot.\n");
            return 1;
        }
        ptp_started = 1;

        printf("Varakozas PTP lockra (max %d s)...\n", wait_s);
        fflush(stdout);
        for (int i = 0; i < wait_s * 2 && !g_stop; i++) {
            if (ptp_slave_is_locked()) { locked = 1; break; }
            usleep(500000);
        }
        if (g_stop) { ptp_slave_stop(); return 0; }

        if (locked) {
            ptp_slave_get_gmid(gm, sizeof(gm));
            printf("PTP lock: offset %+ld ns, grandmaster %s\n",
                   ptp_slave_get_offset_ns(), gm[0] ? gm : "(ismeretlen)");
        } else {
            fprintf(stderr, "Megjegyzes: a PTP %d s alatt meg nem lockolt. Addig erkezesi-ido "
                            "alapu lejatszas; a lock utan a vevo automatikusan PTP modra valt.\n",
                    wait_s);
        }
    } else {
        fprintf(stderr, "FIGYELEM: PTP kikapcsolva (-n): a lejatszas nem AES67-megfelelo.\n");
    }

    /* ---- 3. 8.2: kapcsolodasi dontes ---- */
    int try_ptp = use_ptp && locked;
    if (try_ptp) {
        if (sp.clock_type == SDP_CLK_PTP || sp.clock_type == SDP_CLK_8021AS) {
            if (sp.ptp_domain >= 0 && sp.ptp_domain != domain && !sp.traceable)
                fprintf(stderr, "FIGYELEM: eltero PTP domain (stream: %d, vevo: %d), nem traceable "
                                "referencia. A szinkron varhatoan sikertelen (8.2).\n",
                        sp.ptp_domain, domain);
            if (sp.gmid[0] && gm[0] && strcasecmp(sp.gmid, gm) != 0)
                fprintf(stderr, "Megjegyzes: eltero grandmaster (stream: %s, vevo: %s). "
                                "Kapcsolodunk, de a szinkron hibas lehet (8.2).\n", sp.gmid, gm);
        } else {
            fprintf(stderr, "Megjegyzes: a stream nem jelol PTP orat. Megprobaljuk PTP modban; "
                            "ha a timestampek nem illeszkednek, erkezesi modra valtunk.\n");
        }
    }

    /* ---- 4. Vetel ---- */
    aes67_session *s = session_open(&sp, iface);
    if (!s) {
        fprintf(stderr, "A fogado socket nem nyithato meg.\n");
        if (ptp_started) ptp_slave_stop();
        return 1;
    }

    jitter_buffer *jb = jb_create(sp.sample_rate, sp.channels, sp.bytes_per_sample,
                                  try_ptp, sp.have_mediaclk ? sp.mediaclk_offset : 0);
    udp_receiver *rx = jb ? udp_receiver_create(s, jb) : NULL;
    if (!jb || !rx || udp_receiver_start(rx) != 0) {
        fprintf(stderr, "A fogado nem indithato.\n");
        udp_receiver_destroy(rx);
        jb_destroy(jb);
        session_close(s);
        if (ptp_started) ptp_slave_stop();
        return 1;
    }

    /* ---- 5. Lejatszas ---- */
    audio_output *ao = audio_output_create(s, jb, device, link_ms);
    if (!ao || audio_output_start(ao) != 0) {
        fprintf(stderr, "Az audio kimenet nem indithato (ALSA eszkoz? -d plughw:0,0).\n");
        audio_output_destroy(ao);
        udp_receiver_destroy(rx);
        jb_destroy(jb);
        session_close(s);
        if (ptp_started) ptp_slave_stop();
        return 1;
    }
    printf("Az AES67 vevo fut. Ctrl+C a leallitashoz.\n");
    fflush(stdout);

    /* ---- 6. Fo ciklus ---- */
    uint64_t last_rx = 0;
    int idle = 0, was_locked = locked, mode_reported = -1, pkt_reported = 0;

    while (!g_stop) {
        sleep(1);
        if (g_stop) break;

        if (use_ptp) {
            int l = ptp_slave_is_locked();
            if (l != was_locked) {
                if (l) printf("PTP lock (offset %+ld ns). Ha a kuldo is PTP-szinkron, a vevo "
                              "nehany masodpercen belul PTP modra valt.\n", ptp_slave_get_offset_ns());
                else   fprintf(stderr, "FIGYELEM: PTP lock elveszett. A lejatszas a rendszerorarol "
                                       "folytatodik (holdover).\n");
                was_locked = l;
            }
            jb_set_ptp_allowed(jb, l);
            char g2[32];
            if (ptp_slave_get_gmid(g2, sizeof(g2)) == 0 && strcmp(g2, gm) != 0) {
                if (gm[0]) fprintf(stderr, "Megjegyzes: grandmaster valtas: %s -> %s\n", gm, g2);
                snprintf(gm, sizeof(gm), "%s", g2);
                if (sp.gmid[0] && strcasecmp(sp.gmid, gm) != 0 && !sp.traceable)
                    fprintf(stderr, "Megjegyzes: a vevo grandmastere elter a stream leirasatol (8.2).\n");
            }
        }

        udp_rx_stats rs; udp_receiver_get_stats(rx, &rs);
        jb_stats     js; jb_get_stats(jb, &js);
        audio_stats  as; audio_output_get_stats(ao, &as);

        if (js.mode != mode_reported && js.ready) {
            printf("Lejatszasi mod: %s\n", js.mode == JB_MODE_PTP
                   ? "PTP media ora (AES67)" : "erkezesi ido alapu (a kuldo nem PTP-szinkron)");
            mode_reported = js.mode;
        }
        if (!pkt_reported && js.packet_frames) {
            printf("Tenyleges csomagmeret: %u minta (%.3f ms)\n", js.packet_frames,
                   js.packet_frames * 1000.0 / sp.sample_rate);
            pkt_reported = 1;
        }

        char ptpbuf[32];
        if (!use_ptp)                   snprintf(ptpbuf, sizeof(ptpbuf), "ki");
        else if (ptp_slave_is_locked()) snprintf(ptpbuf, sizeof(ptpbuf), "%+ld ns", ptp_slave_get_offset_ns());
        else snprintf(ptpbuf, sizeof(ptpbuf), "NINCS LOCK (%+ld ns)", ptp_slave_get_offset_ns());

        printf("RX %-7llu ok %-7llu lost %-4llu late %-4llu dup %-3llu | jit %5.2f ms fill %5.1f ms | "
               "%s L=%.1f ms err %+7.1f us corr %+7.1f ppm xrun %llu resync %llu | PTP %s\n",
               (unsigned long long)rs.packets_received, (unsigned long long)rs.packets_pushed,
               (unsigned long long)js.packets_lost, (unsigned long long)js.packets_late,
               (unsigned long long)js.packets_duplicate,
               js.jitter_ms, js.fill_ms,
               !as.synced ? "----" : js.mode == JB_MODE_PTP ? "PTP " : "ARR ",
               as.link_offset_ms, as.sync_error_us, as.correction_ppm,
               (unsigned long long)as.xruns, (unsigned long long)as.resyncs, ptpbuf);
        if (rs.packets_wrong_pt || rs.packets_wrong_ssrc || rs.packets_parse_error)
            printf("   (eldobva: rossz PT %llu, idegen SSRC %llu, hibas RTP %llu)\n",
                   (unsigned long long)rs.packets_wrong_pt,
                   (unsigned long long)rs.packets_wrong_ssrc,
                   (unsigned long long)rs.packets_parse_error);
        fflush(stdout);

        /* 10.2, 4. megjegyzes: csend eseten IGMP tagsag megujitasa */
        if (rs.packets_received == last_rx) {
            idle++;
            if (idle == 3 || (idle > 3 && idle % 10 == 0)) {
                if (session_rejoin(s) == 0 && idle == 3)
                    fprintf(stderr, "Nincs bejovo csomag: IGMP tagsag ujrakuldve.\n");
            }
            if (idle == 10)
                fprintf(stderr, "FIGYELEM: 10 s ota nincs csomag. Fut a kuldo? Jo az interfesz "
                                "(-i) es a multicast cim?\n");
        } else {
            idle = 0;
            last_rx = rs.packets_received;
        }
    }

    printf("Leallitas...\n");
    audio_output_stop(ao);
    audio_output_destroy(ao);
    udp_receiver_stop(rx);           /* RTCP BYE */
    udp_receiver_destroy(rx);
    jb_destroy(jb);
    session_close(s);
    if (ptp_started) ptp_slave_stop();
    return 0;
}
