/*
 * Media ora szerint cimzett jitter puffer.
 *
 * Minden minta a 64 bitre kiterjesztett RTP timestamp (minusz mediaclk offset)
 * szerinti pozicioba kerul egy gyurus pufferben. Kovetkezmenyek:
 *   - tetszoleges csomagmeret / packet time (7.2.0: a ptime-ra nem tamaszkodunk),
 *   - ujrarendezes, duplikatum, veszteseg termeszetesen kezelt (hianyzo minta = csend),
 *   - a 32 bites RTP ora atfordulasa a PTP media orahoz igazitva (5. fejezet),
 *   - puffer-kapacitas ~700 ms (7.5: legalabb 3x ill. 20x packet time / 20 ms).
 *
 * Ket mod:
 *   JB_MODE_PTP      az RTP timestamp a PTP media ora (+ mediaclk offset):
 *                    stream pozicio S lejatszasi ideje = S + link offset
 *   JB_MODE_ARRIVAL  a kuldo nem PTP-szinkron (nem AES67-megfelelo): az eltolast
 *                    az erkezesi idokbol becsuljuk (csuszo ablakos minimum)
 * PTP modban az elso csomagokbol ellenorizzuk, hogy a timestampek valoban a helyi
 * PTP orahoz igazodnak-e; ha nem, automatikusan ARRIVAL modra valtunk.
 */
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define JB_RING_MS      700
#define JB_SEQ_WINDOW   1024
#define JB_READY_PKTS   32
#define JB_PLAUS_PKTS   64
#define JB_BLOCK_SEC    2

struct jitter_buffer {
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bytes_per_sample;
    size_t   frame_bytes;
    size_t   ring_frames;          /* 2 hatvanya */
    int32_t *ring;

    uint32_t mediaclk_offset;
    int      mode;
    uint32_t generation;

    int      initialized;
    int      ready;
    uint32_t pkts_since_reset;
    uint32_t last_frames;

    int64_t  floor_pos;            /* ez alatt minden slot torolt */
    int64_t  end_pos;              /* a legmagasabb beerkezett minta + 1 */
    int64_t  last_ext;

    /* sorszam-kovetes (RFC 3550 A.1) */
    uint16_t base_seq;
    uint32_t ext_high;
    uint8_t  seen[JB_SEQ_WINDOW / 8];
    uint64_t uniq;
    uint64_t lost_carry, recv_carry;

    /* erkezesi ido: x = media_now - (S + n) */
    int64_t  blk_cur, blk_prev, blk_start;
    /* PTP modban x maximuma (atviteli ido) ugyanazokban a 2 s-os blokkokban */
    int64_t  xmax_cur, xmax_prev;
    int64_t  xp_max;               /* erkezesi modban: a PTP-hez mert x maximuma az
                                      illeszkedo sorozatban (atvaltaskor ez az indulo ertek) */
    int      have_prev;
    int      plaus_total, plaus_good, plaus_done;

    /* RFC 3550 jitter (mintaegysegben) */
    int      have_transit;
    int64_t  prev_transit;
    double   jitter;

    uint64_t dup, late, underrun, resets;
    int      tai_checked;

    /* erkezesi modban passziv figyeles: a kuldo timestampje illeszkedik-e a PTP orahoz */
    int      ptp_allowed;
    int64_t  ptp_good_frames;      /* egymas utani illeszkedo csomagok mintaszama */
    int64_t  ptp_need_frames;      /* ennyi kell az atvaltashoz */
    int64_t  frames_since_reset;

    pthread_mutex_t lock;
};

/* ---------------------------------------------------------- */

static inline int seen_get(const jitter_buffer *jb, uint16_t seq)
{
    unsigned i = seq & (JB_SEQ_WINDOW - 1);
    return (jb->seen[i >> 3] >> (i & 7)) & 1;
}

static inline void seen_set(jitter_buffer *jb, uint16_t seq, int v)
{
    unsigned i = seq & (JB_SEQ_WINDOW - 1);
    if (v) jb->seen[i >> 3] |= (uint8_t)(1u << (i & 7));
    else   jb->seen[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static uint32_t lost_now(const jitter_buffer *jb)
{
    if (!jb->uniq) return 0;
    uint32_t expected = jb->ext_high - jb->base_seq + 1;
    return expected > jb->uniq ? (uint32_t)(expected - jb->uniq) : 0;
}

/* A [from, to) poziciok slotjainak torlese. Csak beerkezett (< end_pos) poziciok
 * slotjaiban lehet adat, ezert to-t end_pos-ra korlatozzuk: igy a meg el nem ert,
 * ervenyes adatot tarolo slotok sosem torlodnek. */
static void clear_frames(jitter_buffer *jb, int64_t from, int64_t to)
{
    if (to > jb->end_pos) to = jb->end_pos;
    if (to <= from) return;
    const int64_t ring = (int64_t)jb->ring_frames;
    const size_t  fb = jb->channels * sizeof(int32_t);
    if (to - from >= ring) {
        memset(jb->ring, 0, jb->ring_frames * fb);
        return;
    }
    int64_t a = from & (ring - 1), b = to & (ring - 1);
    if (a < b) {
        memset(&jb->ring[(size_t)a * jb->channels], 0, (size_t)(b - a) * fb);
    } else {
        memset(&jb->ring[(size_t)a * jb->channels], 0, (size_t)(ring - a) * fb);
        memset(jb->ring, 0, (size_t)b * fb);
    }
}

/* A puffer tartalmanak athelyezese delta mintaval eltolt pozicio-tengelyre
 * (erkezesi -> PTP mod valtas). A gyuru indexei (pos & maszk) szerint forgatunk. */
static void rebase_locked(jitter_buffer *jb, int64_t delta)
{
    const size_t rf = jb->ring_frames, fb = jb->channels * sizeof(int32_t);
    const size_t r = (size_t)(delta & (int64_t)(rf - 1));
    if (r) {
        int32_t *tmp = malloc(rf * fb);
        if (tmp) {
            /* uj[(i + r) mod rf] = regi[i] */
            memcpy(&tmp[r * jb->channels], jb->ring, (rf - r) * fb);
            memcpy(tmp, &jb->ring[(rf - r) * jb->channels], r * fb);
            memcpy(jb->ring, tmp, rf * fb);
            free(tmp);
        } else {
            memset(jb->ring, 0, rf * fb);             /* vegszukseg: csend */
        }
    }
    jb->floor_pos += delta;
    jb->end_pos   += delta;
    jb->last_ext  += delta;
}

static void reset_locked(jitter_buffer *jb)
{
    jb->lost_carry += lost_now(jb);
    jb->recv_carry += jb->uniq;
    memset(jb->ring, 0, jb->ring_frames * jb->channels * sizeof(int32_t));
    jb->initialized = 0;
    jb->ready = 0;
    jb->pkts_since_reset = 0;
    jb->uniq = 0;
    jb->have_transit = 0;
    jb->jitter = 0.0;
    jb->have_prev = 0;
    jb->plaus_total = jb->plaus_good = jb->plaus_done = 0;
    jb->ptp_good_frames = 0;
    jb->xp_max = INT64_MIN;
    jb->frames_since_reset = 0;
    jb->generation++;
    jb->resets++;
}

jitter_buffer *jb_create(uint32_t sample_rate, uint8_t channels, uint8_t bytes_per_sample,
                         int ptp_mode, uint32_t mediaclk_offset)
{
    if (!sample_rate || !channels) return NULL;
    if (bytes_per_sample != 2 && bytes_per_sample != 3) return NULL;

    jitter_buffer *jb = calloc(1, sizeof(*jb));
    if (!jb) return NULL;

    jb->sample_rate      = sample_rate;
    jb->channels         = channels;
    jb->bytes_per_sample = bytes_per_sample;
    jb->frame_bytes      = (size_t)channels * bytes_per_sample;
    jb->mediaclk_offset  = mediaclk_offset;
    jb->mode             = ptp_mode ? JB_MODE_PTP : JB_MODE_ARRIVAL;
    jb->ptp_allowed      = ptp_mode;
    jb->ptp_need_frames  = 2 * (int64_t)sample_rate;
    jb->xp_max           = INT64_MIN;

    size_t want = (size_t)sample_rate * JB_RING_MS / 1000u, rf = 1;
    while (rf < want) rf <<= 1;
    jb->ring_frames = rf;
    jb->ring = calloc(rf * channels, sizeof(int32_t));
    if (!jb->ring) { free(jb); return NULL; }

    pthread_mutex_init(&jb->lock, NULL);
    return jb;
}

void jb_destroy(jitter_buffer *jb)
{
    if (!jb) return;
    pthread_mutex_destroy(&jb->lock);
    free(jb->ring);
    free(jb);
}

void jb_reset(jitter_buffer *jb)
{
    if (!jb) return;
    pthread_mutex_lock(&jb->lock);
    reset_locked(jb);
    pthread_mutex_unlock(&jb->lock);
}

/* ---------------------------------------------------------- */

int jb_push(jitter_buffer *jb, const rtp_header *h, uint64_t now_ptp_u, uint64_t now_raw_u)
{
    if (!jb || !h || !h->payload) return -1;
    if (h->payload_length == 0 || h->payload_length > JB_MAX_PACKET_BYTES) return -1;
    if (h->payload_length % jb->frame_bytes != 0) {
        static int cnt = 0;
        if (cnt++ < 5)
            fprintf(stderr, "JB: a payload (%zu B) nem oszthato a frame merettel (%zu B). "
                            "Az SDP csatornaszama vagy kodolasa nem egyezik a streammel?\n",
                    h->payload_length, jb->frame_bytes);
        return -1;
    }

    const int64_t  n         = (int64_t)(h->payload_length / jb->frame_bytes);
    const int64_t  rate      = (int64_t)jb->sample_rate;
    const int64_t  ring      = (int64_t)jb->ring_frames;
    if (n > ring / 4) return -1;                 /* tul hosszu csomag a gyuruhoz */

    pthread_mutex_lock(&jb->lock);

restart:;
    /* PTP modban a PTP media ora, erkezesi modban a nem hangolt helyi ora a referencia:
     * igy a ptp4l induleskori frekvenciahangolasa nem zavarja az erkezesi modot */
    const int64_t  media_now = (int64_t)(jb->mode == JB_MODE_PTP ? now_ptp_u : now_raw_u);
    const uint32_t ts_adj = h->timestamp - jb->mediaclk_offset;
    int new_epoch = 0;
    int64_t S;
    if (!jb->initialized) {
        new_epoch = 1;
        jb->initialized = 1;
        jb->base_seq = h->sequence;
        jb->ext_high = h->sequence;
        jb->uniq = 0;
        memset(jb->seen, 0, sizeof(jb->seen));
        jb->blk_cur = INT64_MAX;
        jb->blk_prev = INT64_MAX;
        jb->xmax_cur = INT64_MIN;
        jb->xmax_prev = INT64_MIN;
        jb->blk_start = media_now;
        /* PTP modban a helyi media orahoz legkozelebbi 64 bites ertek (5.: atfordulasok) */
        S = (jb->mode == JB_MODE_PTP) ? media_now + rtp_ts_diff(ts_adj, (uint32_t)media_now)
                                      : (int64_t)ts_adj;
        /* Gyakori hiba: a kuldo vagy a vevo kernelje nem ismeri a TAI-UTC kulonbseget
         * (37 s), igy az egyik UTC-t, a masik TAI-t hasznal. Ilyenkor a timestampek
         * pontosan egesz masodpercnyivel ternek el: ezt felismerjuk es kiegyenlitjuk. */
        if (jb->mode == JB_MODE_PTP && !jb->tai_checked) {
            jb->tai_checked = 1;
            const int64_t x0 = media_now - (S + n);
            const int64_t k = (x0 >= 0 ? x0 + rate / 2 : x0 - rate / 2) / rate;
            const int64_t rest = x0 - k * rate;
            if (k != 0 && k >= -40 && k <= 40 && rest > -rate / 20 && rest < rate / 20) {
                fprintf(stderr,
                    "JB: a kuldo timestampje pontosan %+lld s-mal ter el a PTP media oratol "
                    "(TAI/UTC keveredes, lasd README). Automatikusan kiegyenlitve.\n",
                    (long long)k);
                jb->mediaclk_offset -= (uint32_t)(k * rate);
                S += k * rate;
            }
        }
        jb->last_ext  = S;
        jb->floor_pos = S - 8 * n;       /* indulaskori kisebb ujrarendezes megengedett */
        jb->end_pos   = S;
    } else {
        S = jb->last_ext + rtp_ts_diff(ts_adj, (uint32_t)jb->last_ext);
        if (S > jb->end_pos + ring / 2 || S + n < jb->floor_pos - ring / 2) {
            reset_locked(jb);            /* kuldo ujraindult vagy nagy ugras a timestampben */
            goto restart;
        }
        if (S > jb->last_ext) jb->last_ext = S;
    }

    /* --- sorszam, duplikatum --- */
    if (!new_epoch) {
        int delta = rtp_seq_diff(h->sequence, (uint16_t)jb->ext_high);
        if (delta <= 0) {
            if (delta <= -JB_SEQ_WINDOW) { jb->late++; goto out; }
            if (seen_get(jb, h->sequence)) { jb->dup++; goto out; }
        } else {
            if (delta >= JB_SEQ_WINDOW) memset(jb->seen, 0, sizeof(jb->seen));
            else for (int i = 1; i < delta; i++)
                seen_set(jb, (uint16_t)(jb->ext_high + (uint32_t)i), 0);
            jb->ext_high += (uint32_t)delta;
        }
    }
    seen_set(jb, h->sequence, 1);
    jb->uniq++;
    jb->last_frames = (uint32_t)n;

    /* --- (reszben) mar lejatszott helyre esik: kesve erkezett --- */
    if (S < jb->floor_pos) {
        jb->late++;
        if (S + n <= jb->floor_pos) goto out;
    }

    /* --- a lejatszas lemaradt: a gyuru elejet eldobjuk --- */
    if (S + n > jb->floor_pos + ring) {
        int64_t nf = S + n - ring;
        clear_frames(jb, jb->floor_pos, nf);
        jb->floor_pos = nf;
    }

    /* --- mintak dekodolasa (big-endian L16/L24 -> int32, 24 bites skala) --- */
    {
        const uint8_t *p = h->payload;
        const size_t ch = jb->channels;
        for (int64_t f = 0; f < n; f++) {
            const int64_t pos = S + f;
            if (pos < jb->floor_pos) { p += jb->frame_bytes; continue; }
            int32_t *dst = &jb->ring[(size_t)(pos & (ring - 1)) * ch];
            if (jb->bytes_per_sample == 3) {
                for (size_t c = 0; c < ch; c++, p += 3) {
                    int32_t v = (int32_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
                    dst[c] = (v & 0x800000) ? v - 0x1000000 : v;
                }
            } else {
                for (size_t c = 0; c < ch; c++, p += 2)
                    dst[c] = (int32_t)(int16_t)(uint16_t)(((uint16_t)p[0] << 8) | p[1]) * 256;
            }
        }
    }
    if (S + n > jb->end_pos) jb->end_pos = S + n;

    /* --- erkezesi ido statisztika --- */
    {
        const int64_t x = media_now - (S + n);
        if (x < jb->blk_cur) jb->blk_cur = x;
        if (x > jb->xmax_cur) jb->xmax_cur = x;

        if (jb->mode == JB_MODE_PTP && !jb->plaus_done) {
            jb->plaus_total++;
            if (x >= -rate * 30 / 1000 && x <= rate / 4) jb->plaus_good++;
            if (jb->plaus_total >= JB_PLAUS_PKTS && jb->frames_since_reset + n >= rate / 4) {
                if (jb->plaus_good * 10 < jb->plaus_total * 9) {
                    fprintf(stderr,
                        "JB: a kuldo RTP timestampje nem a helyi PTP media orat koveti "
                        "(a kuldo nincs PTP-szinkronban, mas grandmaster, vagy hibas "
                        "mediaclk). Atallas erkezesi-ido alapu modra.\n");
                    jb->mode = JB_MODE_ARRIVAL;
                    jb->ptp_need_frames = 30 * rate;     /* ujraprobalas csak hosszabb egyezes utan */
                    reset_locked(jb);
                    goto restart;
                }
                jb->plaus_done = 1;
            }
        }

        if (media_now - jb->blk_start >= rate * JB_BLOCK_SEC) {
            if (jb->mode == JB_MODE_PTP && jb->plaus_done &&
                (jb->blk_cur < -rate / 10 || jb->blk_cur > rate * 2)) {
                fprintf(stderr, "JB: a kuldo es a vevo PTP ideje szetvalt. "
                                "Atallas erkezesi-ido alapu modra.\n");
                jb->mode = JB_MODE_ARRIVAL;
                jb->ptp_need_frames = 30 * rate;
                reset_locked(jb);
                goto restart;
            }
            jb->blk_prev = jb->blk_cur;
            jb->have_prev = 1;
            jb->blk_cur = INT64_MAX;
            jb->xmax_prev = jb->xmax_cur;
            jb->xmax_cur = INT64_MIN;
            jb->blk_start = media_now;
        }

        /* RFC 3550 interarrival jitter */
        const int64_t transit = media_now - S;
        if (jb->have_transit) {
            int64_t d = transit - jb->prev_transit;
            if (d < 0) d = -d;
            jb->jitter += ((double)d - jb->jitter) / 16.0;
        }
        jb->prev_transit = transit;
        jb->have_transit = 1;
    }

    /* --- erkezesi modban: illeszkedik-e a kuldo a PTP orahoz? Ha a vevo PTP-je
     *     lockolt, es a timestampek ~2 s-on at (egesz masodperces TAI/UTC elteressel
     *     egyutt) a PTP media orat kovetik, atvaltunk PTP modra. --- */
    if (jb->mode == JB_MODE_ARRIVAL && jb->ptp_allowed) {
        const int64_t np = (int64_t)now_ptp_u;
        const int64_t Sp = np + rtp_ts_diff(ts_adj, (uint32_t)np);
        int64_t xp = np - (Sp + n);
        const int64_t k = (xp >= 0 ? xp + rate / 2 : xp - rate / 2) / rate;
        if (k >= -40 && k <= 40) xp -= k * rate;
        if (xp >= -rate * 30 / 1000 && xp <= rate / 4) {
            jb->ptp_good_frames += n;
            if (xp > jb->xp_max) jb->xp_max = xp;
        } else {
            jb->ptp_good_frames = 0;
            jb->xp_max = INT64_MIN;
        }
        if (jb->ptp_good_frames >= jb->ptp_need_frames) {
            /* Atvaltas a puffer eldobasa nelkul: a tartalmat az uj (PTP) idotengelyre
             * toljuk at, igy nincs csend, csak a lejatszasi pont all at egyszer. */
            const int64_t Sp_fix = Sp + k * rate;           /* TAI/UTC javitassal */
            const int64_t delta  = Sp_fix - S;
            if (k != 0) {
                fprintf(stderr, "JB: a kuldo timestampje pontosan %+lld s-mal ter el a PTP "
                                "media oratol (TAI/UTC keveredes). Automatikusan kiegyenlitve.\n",
                        (long long)k);
                jb->mediaclk_offset -= (uint32_t)(k * rate);
            }
            rebase_locked(jb, delta);
            jb->mode = JB_MODE_PTP;
            jb->tai_checked = 1;
            jb->plaus_done = 1;                           /* az illeszkedes mar igazolt */
            jb->ready = 1;
            jb->blk_cur = INT64_MAX;
            jb->blk_prev = INT64_MAX;
            jb->xmax_cur = jb->xp_max;                    /* a mar mert atviteli ido */
            jb->xmax_prev = INT64_MIN;
            jb->xp_max = INT64_MIN;
            jb->have_prev = 0;
            jb->blk_start = np;
            jb->have_transit = 0;
            jb->ptp_good_frames = 0;
            jb->generation++;                             /* a kimenet ujraszinkronizal */
            fprintf(stderr, "JB: a kuldo timestampje a PTP media orat koveti: "
                            "atallas PTP modra (AES67).\n");
            goto out;
        }
    }

    jb->pkts_since_reset++;
    jb->frames_since_reset += n;
    if (!jb->ready)
        jb->ready = (jb->mode == JB_MODE_PTP)
                  ? jb->plaus_done
                  : (jb->pkts_since_reset >= JB_READY_PKTS && jb->frames_since_reset >= rate / 2);
out:
    pthread_mutex_unlock(&jb->lock);
    return 0;
}

/* ---------------------------------------------------------- */

void jb_set_ptp_allowed(jitter_buffer *jb, int allowed)
{
    if (!jb) return;
    pthread_mutex_lock(&jb->lock);
    jb->ptp_allowed = allowed;
    if (!allowed) { jb->ptp_good_frames = 0; jb->xp_max = INT64_MIN; }
    pthread_mutex_unlock(&jb->lock);
}

int jb_get_clock(jitter_buffer *jb, int64_t *offset, int *ready, uint32_t *generation,
                 int *mode)
{
    if (!jb) return -1;
    pthread_mutex_lock(&jb->lock);
    int64_t off = 0;
    if (jb->mode == JB_MODE_ARRIVAL) {
        off = jb->blk_cur;
        if (jb->have_prev && jb->blk_prev < off) off = jb->blk_prev;
        if (off == INT64_MAX) off = 0;
    }
    if (offset)     *offset = off;
    if (ready)      *ready = jb->ready && jb->initialized;
    if (generation) *generation = jb->generation;
    if (mode)       *mode = jb->mode;
    pthread_mutex_unlock(&jb->lock);
    return 0;
}

int jb_read(jitter_buffer *jb, int64_t start, size_t nframes, int32_t *out)
{
    if (!jb || !out) return 0;
    const size_t ch = jb->channels;
    const size_t fb = ch * sizeof(int32_t);
    int missing = 0;

    pthread_mutex_lock(&jb->lock);
    const int64_t ring = (int64_t)jb->ring_frames;
    for (size_t i = 0; i < nframes; i++) {
        const int64_t pos = start + (int64_t)i;
        int32_t *dst = &out[i * ch];
        if (!jb->initialized || pos < jb->floor_pos || pos >= jb->floor_pos + ring) {
            memset(dst, 0, fb);
        } else if (pos >= jb->end_pos) {
            memset(dst, 0, fb);
            missing++;
        } else {
            memcpy(dst, &jb->ring[(size_t)(pos & (ring - 1)) * ch], fb);
        }
    }
    jb->underrun += (uint64_t)missing;
    pthread_mutex_unlock(&jb->lock);
    return missing;
}

void jb_advance(jitter_buffer *jb, int64_t new_floor)
{
    if (!jb) return;
    pthread_mutex_lock(&jb->lock);
    if (jb->initialized && new_floor > jb->floor_pos) {
        clear_frames(jb, jb->floor_pos, new_floor);
        jb->floor_pos = new_floor;
    }
    pthread_mutex_unlock(&jb->lock);
}

void jb_get_stats(jitter_buffer *jb, jb_stats *out)
{
    if (!jb || !out) return;
    pthread_mutex_lock(&jb->lock);
    memset(out, 0, sizeof(*out));
    out->packets_received  = jb->recv_carry + jb->uniq;
    out->packets_lost      = jb->lost_carry + lost_now(jb);
    out->packets_duplicate = jb->dup;
    out->packets_late      = jb->late;
    out->underrun_frames   = jb->underrun;
    out->resets            = jb->resets;
    out->jitter_ms         = jb->jitter * 1000.0 / (double)jb->sample_rate;
    out->fill_ms = (jb->initialized && jb->end_pos > jb->floor_pos)
                 ? (double)(jb->end_pos - jb->floor_pos) * 1000.0 / (double)jb->sample_rate
                 : 0.0;
    out->packet_frames = jb->last_frames;
    out->mode  = jb->mode;
    out->ready = jb->ready && jb->initialized;
    if (jb->mode == JB_MODE_PTP && jb->initialized) {
        int64_t xm = jb->xmax_cur > jb->xmax_prev ? jb->xmax_cur : jb->xmax_prev;
        if (xm != INT64_MIN) {
            out->have_transit_max = 1;
            out->transit_max_ms = (double)xm * 1000.0 / (double)jb->sample_rate;
        }
    }
    pthread_mutex_unlock(&jb->lock);
}

int jb_get_rtcp(jitter_buffer *jb, uint32_t *ext_highest_seq, uint32_t *expected,
                uint32_t *received, uint32_t *jitter_samples)
{
    if (!jb) return -1;
    pthread_mutex_lock(&jb->lock);
    if (!jb->initialized || !jb->uniq) { pthread_mutex_unlock(&jb->lock); return -1; }
    if (ext_highest_seq) *ext_highest_seq = jb->ext_high;
    if (expected)        *expected = jb->ext_high - jb->base_seq + 1;
    if (received)        *received = (uint32_t)jb->uniq;
    if (jitter_samples)  *jitter_samples = (uint32_t)(jb->jitter + 0.5);
    pthread_mutex_unlock(&jb->lock);
    return 0;
}

int64_t jb_get_end(jitter_buffer *jb)
{
    if (!jb) return 0;
    pthread_mutex_lock(&jb->lock);
    int64_t e = jb->initialized ? jb->end_pos : INT64_MIN;
    pthread_mutex_unlock(&jb->lock);
    return e;
}
