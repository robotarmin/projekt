/*
 * ALSA kimenet, a PTP media orahoz kotott lejatszassal (AES67 5., 7.4).
 *
 * A stream S pozicioju mintaja a DAC-on a t(S) = S + off + L idopontban szol (media ora
 * mintaban), ahol L a link offset, off PTP modban 0, erkezesi modban a jitter puffer
 * becslese. A link offset allando (7.4): automatikus modban indulaskor a mert ertekekbol
 * szamoljuk, es csak akkor no, ha csomagok a lejatszasi pont utan erkeznek; soha nem
 * csokken (minden csokkentes kesesekhez, azaz kattanasokhoz vezetne).
 *
 * A hangkartya sajat kristalyrol jar. Periodusonkent merjuk, melyik stream-pozicio szol
 * (irasi pozicio - ALSA delay), es PI szabalyozo hangol egy 32 pontos, Kaiser-ablakos
 * sinc resamplert (+/- 1000 ppm). 20 ms-nal nagyobb hiba: ujraszinkronizalas.
 * Kimeneti formatumok: S32_LE, S24_LE, S24_3LE, S16_LE (TPDF ditherrel);
 * kimeneti c. csatorna = stream (c mod N). csatornaja.
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <alsa/asoundlib.h>

#define DEF_DEVICE   "plughw:Headphones,0"
#define DEF_PERIODS  3            /* Csak ezen merve: Raspberry Pi 4 Model B Rev 1.2, bcm2835 Headphones
                                   * (plughw:Headphones,0), kernel 6.18.50+rpt-rpi-v8, 2026-11-24.
                                   * A driver legkisebb periodusa 10 ms. 2 periodus: kattogas (xrun
                                   * nelkul, a firmware oldalan), 3: tiszta, L = 35 ms; 4: L = 45 ms.
                                   * Mas eszkozon/kernelen ellenorizni; kattogasnal AES67_ALSA_PERIODS=4. */
#define MAX_CORR     1000e-6
#define TAU_FAST     1.0          /* PI idoallando szinkron utan, ... */
#define TAU_SLOW     5.0          /* ... TAU_RAMP alatt erre lassul */
#define TAU_RAMP     20.0
#define TAU_FILT     0.5          /* hibajel-szuro */
#define RESYNC_MS    20.0
#define MAX_LINK_MS  200.0
#define LINK_SLEW    500e-6       /* a link offset valtozasa: legfeljebb 500 ppm */
#define GRACE_S      5.0          /* szinkron utan ennyi ideig a kesesek nem szamitanak */
#define TAPS         32
#define HALF         (TAPS / 2)
#define PHASES       256
#define BETA         9.0

struct audio {
    jb_t       *jb;
    snd_pcm_t  *pcm;
    char        dev[128];
    snd_pcm_format_t fmt;
    unsigned    obps, och, ich;
    uint32_t    rate;
    snd_pcm_uframes_t period, buffer;
    snd_pcm_sframes_t dmax;       /* a mert legnagyobb ALSA delay */
    double      link_req, link_ms;
    int32_t    *src;
    size_t      src_cap;
    float      *tab, *val;
    uint8_t    *out;
    uint32_t    dither;
    pthread_t   th;
    volatile int run;
    pthread_mutex_t lock;
    audio_stats_t st;
};

static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 60 && t > s * 1e-17; k++) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
    }
    return s;
}

/* A ph. sor a ph/PHASES tortpoziciohoz tartozo TAPS egyutthato (+1 sor az interpolaciohoz). */
static float *make_table(void)
{
    float *tab = malloc(sizeof(float) * (PHASES + 1) * TAPS);
    if (!tab) return NULL;
    for (int ph = 0; ph <= PHASES; ph++)
        for (int j = 0; j < TAPS; j++) {
            double d = (double)(j - (HALF - 1)) - (double)ph / PHASES;
            double w = 1.0 - (d / HALF) * (d / HALF);
            double sn = fabs(d) < 1e-12 ? 1.0 : sin(M_PI * d) / (M_PI * d);
            tab[ph * TAPS + j] = w <= 0.0 ? 0.0f : (float)(sn * bessel_i0(BETA * sqrt(w)) / bessel_i0(BETA));
        }
    return tab;
}

static int alsa_setup(audio_t *a)
{
    static const struct { snd_pcm_format_t f; unsigned bps; } fmts[] = {
        { SND_PCM_FORMAT_S32_LE, 4 }, { SND_PCM_FORMAT_S24_LE, 4 },
        { SND_PCM_FORMAT_S24_3LE, 3 }, { SND_PCM_FORMAT_S16_LE, 2 },
    };
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);
    int rc = snd_pcm_hw_params_any(a->pcm, hw);
    if (rc >= 0) rc = snd_pcm_hw_params_set_access(a->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) goto err;

    rc = -EINVAL;
    for (size_t i = 0; i < 4 && rc < 0; i++)
        if (snd_pcm_hw_params_test_format(a->pcm, hw, fmts[i].f) == 0) {
            a->fmt = fmts[i].f;
            a->obps = fmts[i].bps;
            rc = snd_pcm_hw_params_set_format(a->pcm, hw, a->fmt);
        }
    if (rc < 0) goto err;

    a->och = a->ich;
    if (snd_pcm_hw_params_set_channels(a->pcm, hw, a->och) < 0 &&
        (rc = snd_pcm_hw_params_set_channels_near(a->pcm, hw, &a->och)) < 0) goto err;

    unsigned r = a->rate;
    if ((rc = snd_pcm_hw_params_set_rate_near(a->pcm, hw, &r, 0)) < 0) goto err;
    if (r != a->rate) {
        fprintf(stderr, "audio: az eszkoz nem tud %u Hz-et (plughw: eszkozt hasznalj)\n", a->rate);
        return -1;
    }

    const char *e = getenv("AES67_ALSA_PERIOD"), *ep = getenv("AES67_ALSA_PERIODS");
    snd_pcm_uframes_t per = e && atoi(e) > 0 ? (snd_pcm_uframes_t)atoi(e) : 128 * a->rate / 48000;
    unsigned nper = ep && atoi(ep) >= 2 && atoi(ep) <= 16 ? (unsigned)atoi(ep) : DEF_PERIODS;
    if ((rc = snd_pcm_hw_params_set_period_size_near(a->pcm, hw, &per, 0)) < 0) goto err;
    snd_pcm_uframes_t buf = per * nper;
    if ((rc = snd_pcm_hw_params_set_buffer_size_near(a->pcm, hw, &buf)) < 0) goto err;
    if ((rc = snd_pcm_hw_params(a->pcm, hw)) < 0) goto err;
    snd_pcm_hw_params_get_period_size(hw, &a->period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &a->buffer);

    if ((rc = snd_pcm_sw_params_current(a->pcm, sw)) < 0) goto err;
    snd_pcm_sw_params_set_start_threshold(a->pcm, sw, a->buffer / a->period * a->period);
    snd_pcm_sw_params_set_stop_threshold(a->pcm, sw, a->buffer);
    snd_pcm_sw_params_set_avail_min(a->pcm, sw, a->period);
    if ((rc = snd_pcm_sw_params(a->pcm, sw)) < 0 || (rc = snd_pcm_prepare(a->pcm)) < 0) goto err;
    return 0;
err:
    fprintf(stderr, "audio: ALSA beallitasi hiba (%s): %s\n", a->dev, snd_strerror(rc));
    return -1;
}

static inline uint32_t xorshift(uint32_t *s)
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

/* v: 24 bites skala */
static void put_sample(audio_t *a, uint8_t *d, float v)
{
    if (a->fmt == SND_PCM_FORMAT_S16_LE) {
        float tpdf = ((float)(xorshift(&a->dither) & 0xFFFF) - (float)(xorshift(&a->dither) & 0xFFFF)) / 65536.0f;
        long q = lrintf(v / 256.0f + tpdf);
        q = q > 32767 ? 32767 : q < -32768 ? -32768 : q;
        d[0] = (uint8_t)q; d[1] = (uint8_t)(q >> 8);
        return;
    }
    long q = lrintf(v);
    q = q > 8388607 ? 8388607 : q < -8388608 ? -8388608 : q;
    uint32_t u = a->fmt == SND_PCM_FORMAT_S32_LE ? (uint32_t)q << 8 : (uint32_t)q;
    for (unsigned i = 0; i < a->obps; i++) d[i] = (uint8_t)(u >> (8 * i));
}

/* Egy periodus kiirasa. 0 = ok, -1 = xrun (ujraszinkron kell), -2 = vegzetes hiba. */
static int write_period(audio_t *a)
{
    snd_pcm_uframes_t done = 0;
    int xrun = 0;
    while (done < a->period && a->run) {
        snd_pcm_sframes_t r = snd_pcm_writei(a->pcm, a->out + done * a->och * a->obps, a->period - done);
        if (r == -EAGAIN) continue;
        if (r < 0) {
            pthread_mutex_lock(&a->lock);
            a->st.xruns++;
            pthread_mutex_unlock(&a->lock);
            if (snd_pcm_recover(a->pcm, (int)r, 1) < 0) {
                fprintf(stderr, "audio: irasi hiba: %s\n", snd_strerror((int)r));
                return -2;
            }
            xrun = 1;
            continue;
        }
        done += (snd_pcm_uframes_t)r;
    }
    return xrun ? -1 : 0;
}

static void silence(audio_t *a)
{
    for (size_t i = 0; i < a->period * a->och; i++) put_sample(a, a->out + i * a->obps, 0.0f);
}

/* Automatikus link offset: a legkesobbi kiszamolt minta az ALSA pufferen (vagy a mert
 * nagyobb kesleltetesen) at szol; ennyivel elobb, plusz egy csomagnyi ideje, plusz a
 * mert legnagyobb atvitel, plusz tartalek mar meg kell lennie. */
static double auto_link(const audio_t *a, const jb_stats_t *js)
{
    double alsa = fmax((double)a->buffer, (double)a->dmax) * 1000.0 / a->rate;
    double pkt = (js->pkt_frames ? js->pkt_frames : a->rate / 1000) * 1000.0 / a->rate;
    double L = alsa + pkt + js->transit_ms + fmax(3.0, 4.0 * js->jitter_ms) + 1.0;
    return fmin(ceil(L * 2.0) / 2.0, MAX_LINK_MS);
}

static void *audio_thread(void *arg)
{
    audio_t *a = arg;
    const double rate = a->rate, dt = a->period / rate;
    int synced = 0, first = 1, mode = JB_PTP;
    uint32_t gen = 0;
    int64_t pos_i = 0, off0 = 0;   /* olvasasi pozicio egesz resze; kezdeti eltolas */
    double pos_f = 0, offd = 0;    /* tortresz; az eltolas szurt valtozasa (erkezesi mod) */
    double L = 0, ratio = 1, icorr = 0, ef = 0, t_sync = 0, t_win = 0;
    uint64_t late0 = 0, resyncs = 0, missing = 0;

    while (a->run) {
        int w = snd_pcm_wait(a->pcm, 200);    /* csak friss helynel szamolunk: kisebb kesleltetes */
        jb_stats_t js;
        jb_stats(a->jb, &js);
        if (w < 0 || !js.ready) {
            pthread_mutex_lock(&a->lock);
            a->st.synced = synced = 0;
            a->st.xruns += w < 0;
            pthread_mutex_unlock(&a->lock);
            if (w < 0) { snd_pcm_recover(a->pcm, w, 1); continue; }
            silence(a);
            if (write_period(a) == -2) break;
            continue;
        }

        if (!synced || js.gen != gen) {
            if (a->link_req > 0) a->link_ms = a->link_req;
            else a->link_ms = fmax(a->link_ms, auto_link(a, &js));
            L = a->link_ms * rate / 1000.0;
            mode = js.mode;
            snd_pcm_sframes_t d = 0;
            if (snd_pcm_delay(a->pcm, &d) < 0 || d < 0) d = 0;
            const int64_t now = (int64_t)(mode == JB_PTP ? media_now(a->rate) : raw_now(a->rate));
            off0 = js.off;
            offd = 0;
            const double start = (double)(now - off0) - L + (double)d;   /* d keret mulva szol */
            pos_i = (int64_t)floor(start);
            pos_f = start - (double)pos_i;
            ef = t_sync = t_win = 0;
            ratio = 1.0 - icorr;           /* a hangkartya tanult oraelterese megmarad */
            resyncs += !first;
            first = 0;
            gen = js.gen;
            late0 = js.late;
            synced = 1;
        }

        /* a link offset es az erkezesi modu eltolas valtozasat elorecsatoljuk */
        double move = 0, L0 = L, step = LINK_SLEW * a->period;
        L += fmax(-step, fmin(step, a->link_ms * rate / 1000.0 - L));
        move -= L - L0;
        if (js.off != off0) {
            double tgt = (double)(js.off - off0), o0 = offd;
            if (fabs(tgt - offd) > RESYNC_MS * rate / 1000.0) { synced = 0; continue; }
            offd += (tgt - offd) * dt / 2.0;
            move -= offd - o0;
        }
        double r = fmax(1.0 - 2 * MAX_CORR, fmin(1.0 + 2 * MAX_CORR, ratio + move / a->period));

        /* resampling: a p poziciohoz a floor(p) koruli TAPS bemeneti minta kell */
        const size_t need = (size_t)ceil(a->period * r + pos_f) + TAPS + 1;
        if (need > a->src_cap) { synced = 0; continue; }
        missing += (uint64_t)jb_read(a->jb, pos_i - (HALF - 1), need, a->src);
        double p = pos_f + (HALF - 1);
        for (size_t i = 0; i < a->period; i++, p += r) {
            size_t k = (size_t)p;
            double fp = (p - (double)k) * PHASES;
            int ph = (int)fp < PHASES ? (int)fp : PHASES - 1;
            float fa = (float)(fp - ph);
            const float *c0 = &a->tab[ph * TAPS], *c1 = c0 + TAPS;
            const int32_t *s = &a->src[(k - (HALF - 1)) * a->ich];
            for (unsigned c = 0; c < a->ich; c++) {
                float s0 = 0, s1 = 0;
                for (int j = 0; j < TAPS; j++) {
                    float v = (float)s[j * a->ich + c];
                    s0 += v * c0[j];
                    s1 += v * c1[j];
                }
                a->val[c] = s0 + (s1 - s0) * fa;
            }
            uint8_t *dst = a->out + i * a->och * a->obps;
            for (unsigned c = 0; c < a->och; c++) put_sample(a, dst + c * a->obps, a->val[c % a->ich]);
        }
        double adv = pos_f + a->period * r;
        pos_i += (int64_t)floor(adv);
        pos_f = adv - floor(adv);
        jb_release(a->jb, pos_i - HALF);

        int wr = write_period(a);
        if (wr == -2) break;
        snd_pcm_sframes_t d = 0;
        if (wr < 0 || snd_pcm_delay(a->pcm, &d) < 0) { synced = 0; continue; }
        if (d > a->dmax && d < (snd_pcm_sframes_t)a->buffer * 4) a->dmax = d;

        /* hiba = szolo pozicio - cel = (pos - d*r) - (most - off - L) */
        const int64_t now = (int64_t)(mode == JB_PTP ? media_now(a->rate) : raw_now(a->rate));
        double err = (double)(pos_i - now + off0) + pos_f - (double)d * r + offd + L;
        if (fabs(err) > RESYNC_MS * rate / 1000.0) { synced = 0; continue; }

        /* PI, kritikus csillapitas (kp = 1/(f tau), ki = 1/(4 f tau^2)); az integralt
         * korrekcio-egysegben tartjuk, igy az idoallando valtasa nem okoz ugrast */
        t_sync += dt;
        const double tau = TAU_FAST + (TAU_SLOW - TAU_FAST) * fmin(1.0, t_sync / TAU_RAMP);
        ef += (err - ef) * dt / TAU_FILT;
        icorr = fmax(-MAX_CORR, fmin(MAX_CORR, icorr + ef * dt / (4.0 * tau * tau * rate)));
        ratio = 1.0 + fmax(-MAX_CORR, fmin(MAX_CORR, -(ef / (rate * tau) + icorr)));

        /* kesve erkezo csomagok: a link offset novelese (csak automatikus modban) */
        if ((t_win += dt) >= 1.0) {
            t_win = 0;
            if (a->link_req <= 0 && t_sync >= GRACE_S && js.late > late0) {
                double nl = fmin(MAX_LINK_MS, fmax(a->link_ms + 2.0, auto_link(a, &js)));
                if (nl > a->link_ms) {
                    int hard = nl - a->link_ms > 5.0;
                    a->link_ms = nl;
                    fprintf(stderr, "audio: kesve erkezo csomagok, link offset: %.1f ms%s\n",
                            nl, hard ? " (ujraszinkron)" : "");
                    if (hard) synced = 0;
                }
            }
            late0 = js.late;
        }

        pthread_mutex_lock(&a->lock);
        a->st.synced = synced;
        a->st.link_ms = L * 1000.0 / rate;
        a->st.err_us = ef / rate * 1e6;
        a->st.ppm = (r - 1.0) * 1e6;
        a->st.resyncs = resyncs;
        a->st.missing = missing;
        pthread_mutex_unlock(&a->lock);
    }
    snd_pcm_drop(a->pcm);
    return NULL;
}

audio_t *audio_start(jb_t *jb, uint32_t rate, uint8_t channels, const char *dev, double link_ms)
{
    audio_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->jb = jb;
    a->rate = rate;
    a->ich = channels;
    a->link_req = link_ms;
    a->dither = 0x9E3779B9u;
    pthread_mutex_init(&a->lock, NULL);
    if (!dev || !*dev) dev = getenv("AES67_ALSA_DEVICE");
    snprintf(a->dev, sizeof(a->dev), "%s", dev && *dev ? dev : DEF_DEVICE);

    int rc = snd_pcm_open(&a->pcm, a->dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        fprintf(stderr, "audio: %s nem nyithato meg: %s\n", a->dev, snd_strerror(rc));
        a->pcm = NULL;
        goto fail;
    }
    if (alsa_setup(a)) goto fail;
    a->src_cap = (size_t)(a->period * (1.0 + 2 * MAX_CORR)) + TAPS + 8;
    a->src = calloc(a->src_cap * a->ich, sizeof(int32_t));
    a->out = calloc(a->period * a->och, a->obps);
    a->val = calloc(a->ich, sizeof(float));
    a->tab = make_table();
    if (!a->src || !a->out || !a->val || !a->tab) goto fail;

    printf("ALSA: %s, %s, %u Hz, %u csatorna, periodus %.2f ms, puffer %.2f ms\n",
           a->dev, snd_pcm_format_name(a->fmt), rate, a->och,
           a->period * 1000.0 / rate, a->buffer * 1000.0 / rate);
    if (a->och != a->ich)
        printf("ALSA: a stream %u csatornas, az eszkoz %u: lekepezes (c mod %u)\n",
               a->ich, a->och, a->ich);
    a->run = 1;
    if (thread_start(&a->th, audio_thread, a, 70) == 0) return a;
fail:
    a->run = 0;
    audio_stop(a);
    return NULL;
}

void audio_stop(audio_t *a)
{
    if (!a) return;
    if (a->run) { a->run = 0; pthread_join(a->th, NULL); }
    if (a->pcm) snd_pcm_close(a->pcm);
    free(a->src); free(a->out); free(a->val); free(a->tab);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

void audio_get_stats(audio_t *a, audio_stats_t *out)
{
    pthread_mutex_lock(&a->lock);
    *out = a->st;
    pthread_mutex_unlock(&a->lock);
}
