/*
 * ALSA kimenet, a PTP media orahoz kotott lejatszassal.
 *
 * AES67 7.4: a link offset a kuldo bemenete (RTP timestamp) es a vevo kimenete
 * (DAC) kozotti ido; a vevo tartsa allandoan. Ennek megfeleloen a stream S
 * pozicioju mintajanak a DAC-on a
 *        t(S) = S + offset + L               (media ora mintaban)
 * idopontban kell megszolalnia, ahol L a link offset, offset pedig PTP modban 0,
 * erkezesi modban a jitter puffer becslese.
 *
 * A hangkartya sajat kristalyrol jar, ami nem a PTP ora. Ezert periodusonkent
 * megmerjuk, melyik stream-pozicio szol eppen (beirt pozicio - ALSA delay),
 * osszevetjuk a celallapottal, es egy PI szabalyozo finoman hangolja egy
 * kobos (Catmull-Rom) interpolacios resampler aranyat (+/- 1000 ppm).
 * Nagy elteres (> 20 ms) eseten azonnali ujraszinkronizalas.
 *
 * Tamogatott kimeneti formatumok: S32_LE, S24_LE, S24_3LE, S16_LE (TPDF ditherrel).
 * Csatornaeltereskor: a kimeneti c. csatorna = stream (c mod N). csatornaja.
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <alsa/asoundlib.h>

#define AO_DEFAULT_DEVICE    "plughw:Headphones,0"
#define AO_PERIOD_FRAMES_48K 128           /* ~2.7 ms */
#define AO_PERIODS           4
#define AO_TAU_FAST_S        1.0           /* szabalyozo idoallando szinkron utan... */
#define AO_TAU_SLOW_S        5.0           /* ...es AO_TAU_RAMP_S alatt erre lassul */
#define AO_TAU_RAMP_S        20.0
#define AO_TAU_FILT_S        0.5           /* hibajel-szuro idoallando */
#define AO_MAX_CORR          1000e-6       /* +/- 1000 ppm */
#define AO_RESYNC_MS         20.0
#define AO_MAX_LINK_MS       200.0
#define AO_LINK_STEP_MS      2.0
#define AO_LINK_SLEW         500e-6        /* link offset valtoztatasa: max 500 ppm sebesseggel */
#define AO_GRACE_S           5.0           /* szinkron utan ennyi ideig a kesesek nem szamitanak */
#ifndef AO_CALM_START_S
#define AO_CALM_START_S      30.0          /* ennyi nyugodt ido utan probalunk csokkenteni */
#endif
#define AO_CALM_MAX_S        600.0
#define AO_LINK_DEC_MAX_MS   10.0          /* egy lepesben legfeljebb ennyit csokkentunk */
#define AO_MARGIN_SAFETY_MS  1.5           /* ennyi tartalek mindig marad a legkesobbi csomagig */

struct audio_output {
    aes67_session  *session;
    jitter_buffer  *jb;

    snd_pcm_t      *pcm;
    char            device[128];
    snd_pcm_format_t fmt;
    unsigned        out_bps;              /* kimeneti bajt / minta */
    unsigned        out_ch;
    uint32_t        rate;
    uint8_t         in_ch;
    snd_pcm_uframes_t period, buffer;
    snd_pcm_sframes_t dmax;               /* a mert legnagyobb ALSA delay (keret) */

    double          link_req_ms;          /* <= 0: automatikus */
    double          link_ms;

    int32_t        *src;
    size_t          src_cap;
    uint8_t        *outbuf;
    uint32_t        dither;

    pthread_t       thread;
    volatile int    running;
    pthread_mutex_t lock;
    audio_stats     stats;
};

/* ---------------------------------------------------------- */

static int alsa_setup(audio_output *a)
{
    int rc;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);

    if ((rc = snd_pcm_hw_params_any(a->pcm, hw)) < 0) goto err;
    if ((rc = snd_pcm_hw_params_set_access(a->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        goto err;

    static const struct { snd_pcm_format_t f; unsigned bps; } fmts[] = {
        { SND_PCM_FORMAT_S32_LE, 4 }, { SND_PCM_FORMAT_S24_LE, 4 },
        { SND_PCM_FORMAT_S24_3LE, 3 }, { SND_PCM_FORMAT_S16_LE, 2 },
    };
    rc = -EINVAL;
    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        if (snd_pcm_hw_params_test_format(a->pcm, hw, fmts[i].f) == 0) {
            a->fmt = fmts[i].f;
            a->out_bps = fmts[i].bps;
            rc = snd_pcm_hw_params_set_format(a->pcm, hw, a->fmt);
            break;
        }
    }
    if (rc < 0) goto err;

    unsigned ch = a->in_ch;
    if (snd_pcm_hw_params_set_channels(a->pcm, hw, ch) < 0) {
        if ((rc = snd_pcm_hw_params_set_channels_near(a->pcm, hw, &ch)) < 0) goto err;
        fprintf(stderr, "audio: az eszkoz %u csatornas, a stream %u csatornas: lekepezes\n",
                ch, a->in_ch);
    }
    a->out_ch = ch;

    unsigned r = a->rate;
    if ((rc = snd_pcm_hw_params_set_rate_near(a->pcm, hw, &r, 0)) < 0) goto err;
    if (r != a->rate) {
        fprintf(stderr, "audio: az eszkoz nem tud %u Hz-et (legkozelebbi: %u). "
                        "Hasznalj 'plughw:' eszkozt.\n", a->rate, r);
        rc = -EINVAL;
        goto err;
    }

    snd_pcm_uframes_t per = (snd_pcm_uframes_t)AO_PERIOD_FRAMES_48K * a->rate / 48000;
    const char *e = getenv("AES67_ALSA_PERIOD");
    if (e && atoi(e) > 0) per = (snd_pcm_uframes_t)atoi(e);
    if ((rc = snd_pcm_hw_params_set_period_size_near(a->pcm, hw, &per, 0)) < 0) goto err;
    unsigned nper = AO_PERIODS;
    const char *ep = getenv("AES67_ALSA_PERIODS");       /* kisebb puffer = kisebb link offset */
    if (ep && atoi(ep) >= 2 && atoi(ep) <= 16) nper = (unsigned)atoi(ep);
    snd_pcm_uframes_t buf = per * nper;
    if ((rc = snd_pcm_hw_params_set_buffer_size_near(a->pcm, hw, &buf)) < 0) goto err;
    if ((rc = snd_pcm_hw_params(a->pcm, hw)) < 0) goto err;

    snd_pcm_hw_params_get_period_size(hw, &a->period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &a->buffer);
    if (a->period == 0 || a->buffer < a->period) { rc = -EINVAL; goto err; }

    if ((rc = snd_pcm_sw_params_current(a->pcm, sw)) < 0) goto err;
    snd_pcm_uframes_t start = (a->buffer / a->period) * a->period;
    if ((rc = snd_pcm_sw_params_set_start_threshold(a->pcm, sw, start)) < 0) goto err;
    if ((rc = snd_pcm_sw_params_set_stop_threshold(a->pcm, sw, a->buffer)) < 0) goto err;
    if ((rc = snd_pcm_sw_params_set_avail_min(a->pcm, sw, a->period)) < 0) goto err;
    if ((rc = snd_pcm_sw_params(a->pcm, sw)) < 0) goto err;
    if ((rc = snd_pcm_prepare(a->pcm)) < 0) goto err;
    return 0;

err:
    fprintf(stderr, "audio: ALSA beallitasi hiba (%s): %s\n", a->device, snd_strerror(rc));
    return -1;
}

/* ---------------------------------------------------------- */

static inline uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static void put_sample(audio_output *a, uint8_t *dst, float v)
{
    /* v: 24 bites skala */
    if (a->fmt == SND_PCM_FORMAT_S16_LE) {
        float d = ((float)(xorshift(&a->dither) & 0xFFFF) - (float)(xorshift(&a->dither) & 0xFFFF))
                  * (1.0f / 65536.0f);                            /* TPDF, +/- 1 LSB */
        float s = v * (1.0f / 256.0f) + d;
        long q = lrintf(s);
        if (q > 32767) q = 32767; else if (q < -32768) q = -32768;
        dst[0] = (uint8_t)q; dst[1] = (uint8_t)(q >> 8);
        return;
    }
    long q = lrintf(v);
    if (q > 8388607) q = 8388607; else if (q < -8388608) q = -8388608;
    if (a->fmt == SND_PCM_FORMAT_S32_LE) {
        uint32_t u = (uint32_t)q << 8;
        dst[0] = (uint8_t)u; dst[1] = (uint8_t)(u >> 8);
        dst[2] = (uint8_t)(u >> 16); dst[3] = (uint8_t)(u >> 24);
    } else if (a->fmt == SND_PCM_FORMAT_S24_LE) {
        uint32_t u = (uint32_t)q;
        dst[0] = (uint8_t)u; dst[1] = (uint8_t)(u >> 8);
        dst[2] = (uint8_t)(u >> 16); dst[3] = (q < 0) ? 0xFF : 0x00;
    } else {                                                     /* S24_3LE */
        uint32_t u = (uint32_t)q;
        dst[0] = (uint8_t)u; dst[1] = (uint8_t)(u >> 8); dst[2] = (uint8_t)(u >> 16);
    }
}

/* Teljes periodus kiirasa. 0 = ok, -1 = xrun (ujraszinkron kell), -2 = vegzetes. */
static int write_period(audio_output *a)
{
    const size_t fb = (size_t)a->out_ch * a->out_bps;
    snd_pcm_uframes_t done = 0;
    int xrun = 0;
    while (done < a->period && a->running) {
        snd_pcm_sframes_t r = snd_pcm_writei(a->pcm, a->outbuf + done * fb, a->period - done);
        if (r == -EAGAIN) { usleep(500); continue; }
        if (r == -EPIPE || r == -ESTRPIPE) {
            pthread_mutex_lock(&a->lock);
            a->stats.xruns++;
            pthread_mutex_unlock(&a->lock);
            if (r == -ESTRPIPE) {
                int rr;
                while ((rr = snd_pcm_resume(a->pcm)) == -EAGAIN) usleep(10000);
                if (rr < 0) snd_pcm_prepare(a->pcm);
            } else {
                snd_pcm_prepare(a->pcm);
            }
            xrun = 1;
            continue;
        }
        if (r < 0) {
            fprintf(stderr, "audio: writei hiba: %s\n", snd_strerror((int)r));
            if (snd_pcm_recover(a->pcm, (int)r, 1) < 0) return -2;
            xrun = 1;
            continue;
        }
        done += (snd_pcm_uframes_t)r;
    }
    pthread_mutex_lock(&a->lock);
    a->stats.frames_written += done;
    pthread_mutex_unlock(&a->lock);
    return xrun ? -1 : 0;
}

static void fill_silence(audio_output *a)
{
    memset(a->outbuf, 0, (size_t)a->period * a->out_ch * a->out_bps);
    if (a->fmt == SND_PCM_FORMAT_S16_LE) {          /* dither csendben is (egyenletes zajpadlo) */
        for (size_t i = 0; i < (size_t)a->period * a->out_ch; i++)
            put_sample(a, a->outbuf + i * 2, 0.0f);
    }
}

/* Mennyivel a szolo minta elott olvasunk a jitter pufferbol: a teljes ALSA puffer (vagy
 * a mert legnagyobb delay, ha az nagyobb; egyes eszkozok a pufferen felul is
 * kesleltetnek) PLUSZ egy periodus, amit a kovetkezo irashoz elore kiszamolunk.
 * Nagy periodusu eszkozon (Raspberry Pi analog kimenet: 10 ms) ez donto. */
static double alsa_latency_ms(const audio_output *a)
{
    double b = (double)a->buffer, d = (double)a->dmax;
    return ((d > b ? d : b) + (double)a->period) * 1000.0 / a->rate;
}

static void track_delay(audio_output *a)
{
    snd_pcm_sframes_t d = 0;
    if (snd_pcm_delay(a->pcm, &d) == 0 && d > a->dmax &&
        d < (snd_pcm_sframes_t)(a->buffer * 4)) a->dmax = d;
}

static double auto_link_ms(audio_output *a, const jb_stats *js)
{
    double alsa_ms = alsa_latency_ms(a);
    double pkt_ms  = (double)(js->packet_frames ? js->packet_frames : a->rate / 1000) * 1000.0 / a->rate;
    double jit_ms  = 4.0 * js->jitter_ms;
    if (jit_ms < 3.0) jit_ms = 3.0;
    double L = alsa_ms + pkt_ms + jit_ms + (js->mode == JB_MODE_PTP ? 2.0 : 0.0);
    L = ceil(L * 2.0) / 2.0;                         /* 0.5 ms-ra kerekitve */
    return L > AO_MAX_LINK_MS ? AO_MAX_LINK_MS : L;
}

/* ---------------------------------------------------------- */

static void *ao_thread_main(void *arg)
{
    audio_output *a = arg;
    aes67_set_rt_priority(70, "audio szal");

    const double rate  = (double)a->rate;
    const double dt    = (double)a->period / rate;
    const double alpha = dt / AO_TAU_FILT_S;
    const double off_alpha = dt / 2.0;
    const size_t in_ch = a->in_ch;

    int      synced = 0, first_sync = 1;
    uint32_t gen = 0;
    int64_t  rpos_i = 0;        /* olvasasi pozicio egesz resze (stream mintaban) */
    double   rpos_f = 0.0;      /* tort resze [0,1) */
    int64_t  off_base = 0;      /* erkezesi modu eltolas: egesz + szurt kis resz */
    double   off_d = 0.0;
    double   ratio = 1.0, icorr = 0.0, ef = 0.0, t_sync = 0.0;
    double   L = 0.0;           /* aktualis link offset mintaban (a cel fele csuszik) */
    uint64_t late_prev = 0;     /* kesve erkezett csomagok szama (jitter puffer) */
    uint64_t rx_prev = 0;
    int      grace_done = 0;    /* a turelmi ido vegen a tartalek-meres ujraindul */
    double   late_score = 0.0, win_t = 0.0;
    /* adaptiv link offset: a legkisebb erkezesi tartalek (mintaban) a nyugodt ablakban */
    double   margin_min = 1e18, calm_t = 0.0, calm_need = AO_CALM_START_S, since_dec = 1e9;

    while (a->running) {
        int64_t off = 0; int ready = 0, mode = JB_MODE_ARRIVAL; uint32_t g = 0;
        jb_get_clock(a->jb, &off, &ready, &g, &mode);

        if (!ready) {
            synced = 0;
            fill_silence(a);
            if (write_period(a) == -2) break;
            track_delay(a);
            pthread_mutex_lock(&a->lock);
            a->stats.synced = 0;
            pthread_mutex_unlock(&a->lock);
            continue;
        }

        if (!synced || g != gen) {
            jb_stats js;
            jb_get_stats(a->jb, &js);
            if (a->link_req_ms > 0.0) a->link_ms = a->link_req_ms;
            else if (a->link_ms <= 0.0) a->link_ms = auto_link_ms(a, &js);
            L = a->link_ms * rate / 1000.0;

            snd_pcm_sframes_t d = 0;
            if (snd_pcm_delay(a->pcm, &d) < 0 || d < 0) d = 0;
            const int64_t now = (int64_t)(mode == JB_MODE_PTP ? ptp_media_clock_now(a->rate)
                                                              : raw_media_clock_now(a->rate));
            off_base = off;
            off_d = 0.0;
            /* a most beirt minta d keret mulva szol: celpozicio = now + d - off - L */
            double start = (double)(now - off_base) - L + (double)d;
            double fl = floor(start);
            rpos_i = (int64_t)fl;
            rpos_f = start - fl;
            /* a hangkartya oraelterese tartos: az integratort (a tanult aranyt)
             * ujraszinkronnal sem dobjuk el, csak a hibaszurot */
            ef = 0.0;
            t_sync = 0.0;
            if (first_sync) icorr = 0.0;
            ratio = 1.0 - icorr;
            if (!first_sync) {
                pthread_mutex_lock(&a->lock);
                a->stats.resyncs++;
                pthread_mutex_unlock(&a->lock);
            }
            first_sync = 0;
            synced = 1;
            gen = g;
            late_prev = js.packets_late;
            rx_prev = js.packets_received;
            grace_done = 0;
            late_score = 0.0;
            win_t = 0.0;
            calm_t = 0.0;
            margin_min = 1e18;
        }

        /* link offset noveles: hirtelen ugras (kattanas) helyett lassu csusztatas;
         * a szabalyozo ezalatt legfeljebb ~500 ppm-mel lassit (hallhatatlan) */
        double dtarget = 0.0;            /* a cel-pozicio elmozdulasa ebben a periodusban */
        {
            double Lt = a->link_ms * rate / 1000.0;
            double step = AO_LINK_SLEW * (double)a->period;
            double L0 = L;
            if (L < Lt) L = (Lt - L > step) ? L + step : Lt;
            else if (L > Lt) L = (L - Lt > step) ? L - step : Lt;
            dtarget -= L - L0;
        }

        /* erkezesi modban az eltolas lassu kovetese (kuldo oraja != PTP) */
        if (off != off_base) {
            double target = (double)(off - off_base);
            if (fabs(target - off_d) > AO_RESYNC_MS * rate / 1000.0) { synced = 0; continue; }
            double o0 = off_d;
            off_d += (target - off_d) * off_alpha;
            dtarget -= off_d - o0;
        }
        /* elorecsatolas: a cel ismert mozgasat azonnal kovetjuk, a PI csak a maradekot */
        double r_use = ratio + dtarget / (double)a->period;
        if (r_use > 1.0 + 2 * AO_MAX_CORR) r_use = 1.0 + 2 * AO_MAX_CORR;
        if (r_use < 1.0 - 2 * AO_MAX_CORR) r_use = 1.0 - 2 * AO_MAX_CORR;

        /* --- forrasmintak (Catmull-Rom interpolacio) --- */
        const size_t need = (size_t)ceil((double)a->period * r_use + rpos_f) + 4;
        if (need > a->src_cap) { synced = 0; continue; }
        const int64_t base = rpos_i - 1;
        int miss = jb_read(a->jb, base, need, a->src);
        {
            /* erkezesi tartalek: mennyivel elozi meg a legujabb beerkezett minta a most
             * kiolvasott utolsot. Ha ez tartosan nagy, a link offset csokkentheto. */
            int64_t end = jb_get_end(a->jb);
            if (!grace_done && t_sync >= AO_GRACE_S) { grace_done = 1; margin_min = 1e18; }
            if (end != INT64_MIN) {
                double margin = (double)(end - (base + (int64_t)need));
                if (margin < margin_min) margin_min = margin;
            }
        }

        const size_t ofb = (size_t)a->out_ch * a->out_bps;
        double p = rpos_f + 1.0;              /* src[1] = rpos_i */
        for (size_t i = 0; i < a->period; i++) {
            size_t k = (size_t)p;
            float t = (float)(p - (double)k);
            const int32_t *y0 = &a->src[(k - 1) * in_ch], *y1 = &a->src[k * in_ch];
            const int32_t *y2 = &a->src[(k + 1) * in_ch], *y3 = &a->src[(k + 2) * in_ch];
            uint8_t *dst = a->outbuf + i * ofb;
            for (size_t c = 0; c < a->out_ch; c++) {
                size_t sc = c % in_ch;
                float v0 = (float)y0[sc], v1 = (float)y1[sc], v2 = (float)y2[sc], v3 = (float)y3[sc];
                float c1 = 0.5f * (v2 - v0);
                float c2 = v0 - 2.5f * v1 + 2.0f * v2 - 0.5f * v3;
                float c3 = 0.5f * (v3 - v0) + 1.5f * (v1 - v2);
                float v  = ((c3 * t + c2) * t + c1) * t + v1;
                put_sample(a, dst + c * a->out_bps, v);
            }
            p += r_use;
        }
        {
            double adv = rpos_f + (double)a->period * r_use;
            double fl = floor(adv);
            rpos_i += (int64_t)fl;
            rpos_f = adv - fl;
        }
        jb_advance(a->jb, rpos_i - 2);

        int wr = write_period(a);
        if (wr == -2) break;
        if (wr == -1) { synced = 0; continue; }

        /* --- idozites merese es szabalyozas --- */
        snd_pcm_sframes_t d = 0;
        if (snd_pcm_delay(a->pcm, &d) < 0) { synced = 0; continue; }
        const int64_t now = (int64_t)(mode == JB_MODE_PTP ? ptp_media_clock_now(a->rate)
                                                          : raw_media_clock_now(a->rate));
        if (d > a->dmax && d < (snd_pcm_sframes_t)(a->buffer * 4)) a->dmax = d;
        /* szolo pozicio - cel: (rpos - d*ratio) - (now - off - L) */
        double err = (double)(rpos_i - now + off_base) + rpos_f - (double)d * r_use + off_d + L;

        if (fabs(err) > AO_RESYNC_MS * rate / 1000.0) { synced = 0; continue; }

        /* PI szabalyozo, kritikus csillapitas: kp = 1/(f*tau), ki = 1/(4 f tau^2).
         * Szinkron utan gyors (1 s), majd 20 s alatt lassu (5 s) idoallandora valt,
         * hogy a kezdeti tranziens kicsi, a vegallapot pedig zajmentes legyen.
         * Az integralt kozvetlenul korrekcio-egysegben tartjuk, igy az erositesvaltas
         * nem okoz ugrast. */
        t_sync += dt;
        double tau = AO_TAU_FAST_S + (AO_TAU_SLOW_S - AO_TAU_FAST_S) *
                     (t_sync >= AO_TAU_RAMP_S ? 1.0 : t_sync / AO_TAU_RAMP_S);
        double kp = 1.0 / (rate * tau);
        double ki = 1.0 / (4.0 * tau * tau * rate);
        ef += (err - ef) * alpha;
        icorr += ki * ef * dt;
        if (icorr > AO_MAX_CORR) icorr = AO_MAX_CORR; else if (icorr < -AO_MAX_CORR) icorr = -AO_MAX_CORR;
        double corr = -(kp * ef + icorr);
        if (corr > AO_MAX_CORR) corr = AO_MAX_CORR; else if (corr < -AO_MAX_CORR) corr = -AO_MAX_CORR;
        ratio = 1.0 + corr;

        /* --- adaptiv link offset (7.4: a vevo tartsa allandoan, de a halozati
         *     viszonyokhoz igazodhat) ---
         *  - novelés: ha csomagok a lejatszasi pont utan erkeznek (elveszett csomag nem
         *    szamit). A szinkron utani AO_GRACE_S ideig a kesesek (indulaskori loket)
         *    figyelmen kivul maradnak.
         *  - csokkentes: calm_need ideig tarto nyugalom utan, a mert legkisebb erkezesi
         *    tartalek alapjan, mindig AO_MARGIN_SAFETY_MS tartalekot hagyva.
         *  - ha egy csokkentes utan hamar ujra novelni kell, a szukseges nyugalmi ido
         *    duplazodik (nincs ide-oda lengés).
         *  Minden valtozas lassan csuszik (AO_LINK_SLEW), hallhato ugras nelkul. */
        win_t += dt;
        if (win_t >= 1.0) {
            jb_stats js;
            jb_get_stats(a->jb, &js);
            double late_delta = (double)(js.packets_late - late_prev);
            double rx_delta = (double)(js.packets_received - rx_prev);
            late_prev = js.packets_late;
            rx_prev = js.packets_received;
            /* indulaskori loket: a turelmi idoben az elszort kesesek nem szamitanak,
             * a tomeges (a csomagok > 25%-a) viszont igen: az rossz beallitast jelez */
            if (t_sync < AO_GRACE_S && late_delta < 0.25 * rx_delta) late_delta = 0.0;
            late_score = late_score * 0.9 + late_delta;
            since_dec += win_t;
            win_t = 0.0;

            if (a->link_req_ms <= 0.0) {
                const double Lt = a->link_ms * rate / 1000.0;
                if (late_score >= 2.0) {
                    /* a mert tartalekhiany alapjan annyival novelunk, amennyi kell;
                     * nagy hiany (> 5 ms) eseten egyetlen gyors ujraszinkronnal, hogy ne
                     * kelljen percekig rossz hangot hallgatni a lassu csusztatas alatt */
                    double inc = AO_LINK_STEP_MS;
                    if (margin_min < 1e17) {
                        double deficit = AO_MARGIN_SAFETY_MS - margin_min * 1000.0 / rate;
                        if (deficit > inc) inc = ceil(deficit);
                    }
                    int hard = inc > 5.0;
                    if (a->link_ms + inc > AO_MAX_LINK_MS) inc = AO_MAX_LINK_MS - a->link_ms;
                    if (inc > 0.0) {
                        a->link_ms += inc;
                        if (since_dec < 2.0 * calm_need) {
                            calm_need *= 2.0;
                            if (calm_need > AO_CALM_MAX_S) calm_need = AO_CALM_MAX_S;
                        }
                        fprintf(stderr, "audio: kesve erkezo csomagok, link offset novelve: "
                                        "%.1f ms%s\n", a->link_ms, hard ? " (gyors korrekcio)" : "");
                        if (hard) synced = 0;
                    }
                    late_score = 0.0;
                    calm_t = 0.0;
                    margin_min = 1e18;
                } else if (late_delta > 0.0) {
                    calm_t = 0.0;
                } else {
                    calm_t += 1.0;
                    if (calm_t >= calm_need && fabs(L - Lt) < 1.0 && margin_min < 1e17) {
                        const double floor_ms = alsa_latency_ms(a) +
                            (double)(js.packet_frames ? js.packet_frames : a->rate / 1000) *
                            1000.0 / rate + 0.5;
                        const double mm_ms = margin_min * 1000.0 / rate;
                        double dec = 0.5 * (mm_ms - AO_MARGIN_SAFETY_MS);
                        if (dec > AO_LINK_DEC_MAX_MS) dec = AO_LINK_DEC_MAX_MS;
                        if (a->link_ms - dec < floor_ms) dec = a->link_ms - floor_ms;
                        dec = floor(dec * 10.0) / 10.0;
                        if (dec >= 0.2) {
                            a->link_ms -= dec;
                            since_dec = 0.0;
                            printf("audio: nyugodt halozat (legkisebb tartalek %.1f ms), "
                                   "link offset csokkentve: %.1f ms\n", mm_ms, a->link_ms);
                        }
                        calm_t = 0.0;
                        margin_min = 1e18;
                    }
                }
            }
        }

        pthread_mutex_lock(&a->lock);
        a->stats.synced = 1;
        a->stats.alsa_delay_frames = (int)d;
        a->stats.correction_ppm = (r_use - 1.0) * 1e6;   /* teljes sebessegkorrekcio */
        a->stats.sync_error_us = ef / rate * 1e6;
        a->stats.link_offset_ms = L * 1000.0 / rate;
        a->stats.underrun_frames += (uint64_t)miss;
        pthread_mutex_unlock(&a->lock);
    }

    snd_pcm_drop(a->pcm);
    return NULL;
}

/* ---------------------------------------------------------- */

audio_output *audio_output_create(aes67_session *s, jitter_buffer *jb,
                                  const char *device, double link_offset_ms)
{
    if (!s || !jb) return NULL;
    const session_params *sp = session_get_params(s);
    if (!sp) return NULL;

    audio_output *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->session     = s;
    a->jb          = jb;
    a->rate        = sp->sample_rate;
    a->in_ch       = sp->channels;
    a->link_req_ms = link_offset_ms;
    a->dither      = 0x9E3779B9u;
    pthread_mutex_init(&a->lock, NULL);

    if (!device || !*device) device = getenv("AES67_ALSA_DEVICE");
    if (!device || !*device) device = AO_DEFAULT_DEVICE;
    snprintf(a->device, sizeof(a->device), "%s", device);

    int rc = snd_pcm_open(&a->pcm, a->device, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        fprintf(stderr, "audio: snd_pcm_open(%s) hiba: %s\n", a->device, snd_strerror(rc));
        a->pcm = NULL;
        goto fail;
    }
    if (alsa_setup(a) != 0) goto fail;

    a->src_cap = (size_t)((double)a->period * (1.0 + 2 * AO_MAX_CORR)) + 8;
    a->src     = calloc(a->src_cap * a->in_ch, sizeof(int32_t));
    a->outbuf  = calloc((size_t)a->period * a->out_ch, a->out_bps);
    if (!a->src || !a->outbuf) goto fail;

    a->stats.out_rate = a->rate;
    a->stats.out_channels = a->out_ch;
    snprintf(a->stats.out_format, sizeof(a->stats.out_format), "%s",
             snd_pcm_format_name(a->fmt));
    if (a->link_req_ms > 0.0) a->stats.link_offset_ms = a->link_req_ms;

    printf("ALSA: %s, %s, %u Hz, %u ch, periodus %lu (%.2f ms), puffer %lu (%.2f ms)\n",
           a->device, snd_pcm_format_name(a->fmt), a->rate, a->out_ch,
           (unsigned long)a->period, a->period * 1000.0 / a->rate,
           (unsigned long)a->buffer, a->buffer * 1000.0 / a->rate);
    return a;

fail:
    if (a->pcm) snd_pcm_close(a->pcm);
    free(a->src);
    free(a->outbuf);
    pthread_mutex_destroy(&a->lock);
    free(a);
    return NULL;
}

int audio_output_start(audio_output *a)
{
    if (!a || a->running) return -1;
    a->running = 1;
    if (pthread_create(&a->thread, NULL, ao_thread_main, a) != 0) { a->running = 0; return -1; }
    return 0;
}

void audio_output_stop(audio_output *a)
{
    if (!a || !a->running) return;
    a->running = 0;
    pthread_join(a->thread, NULL);
}

void audio_output_destroy(audio_output *a)
{
    if (!a) return;
    if (a->running) audio_output_stop(a);
    if (a->pcm) snd_pcm_close(a->pcm);
    free(a->src);
    free(a->outbuf);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

void audio_output_get_stats(audio_output *a, audio_stats *out)
{
    if (!a || !out) return;
    pthread_mutex_lock(&a->lock);
    *out = a->stats;
    pthread_mutex_unlock(&a->lock);
}
