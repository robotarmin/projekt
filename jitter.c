/*
 * A kód mesterséges intelligencia segítségével készült. Ezt az egyetemi szabályzatnak megfelelően itt jelölöm. [AI1]
 * Media ora szerint cimzett jitter puffer.
 *
 * Minden minta a 64 bitre kiterjesztett RTP timestamp (minusz mediaclk offset) szerinti
 * S pozicioba kerul egy gyurus pufferben (~700 ms, 7.5). Igy tetszoleges csomagmeret
 * (7.2.0: a ptime-ra nem tamaszkodunk), ujrarendezes, duplikatum es veszteseg (csend)
 * termeszetesen kezelt.
 *
 * PTP mod (AES67): S a PTP media orahoz legkozelebbi 64 bites ertek, ezzel a 32 bites
 *   RTP ora atfordulasait helyesen kezeljuk (5.); az S minta lejatszasi ideje S + link.
 * Erkezesi mod: a kuldo nem a PTP orat koveti (nem AES67): az eltolast az erkezesi
 *   idokbol becsuljuk (2-4 s-os ablak minimuma), a helyi nem hangolt ora szerint.
 *
 * Minden csomagnal ellenorizzuk, illeszkedik-e a timestamp a helyi PTP orahoz
 * (-30 ms ... +250 ms atviteli ido). PTP modbol 0,25 s folyamatos elteres utan erkezesi
 * modra valtunk; vissza, ha a vevo lockolt es 2 s (egy hiba utan 30 s) ota illeszkedik.
 * Ha az elteres pontosan egesz masodperc (TAI/UTC keveredes valamelyik gepen, tipikusan
 * 37 s), kiegyenlitjuk.
 */
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_MS 700
#define SEQ_WIN 1024
#define BLOCK_S 2

struct jb {
    pthread_mutex_t lock;
    uint32_t rate;
    uint8_t  ch, bps;
    size_t   fbytes;
    int64_t  ring_len;                  /* 2 hatvanya */
    int32_t *ring;

    uint32_t mediaclk;                  /* + esetleges TAI/UTC javitas */
    int      mode, ptp_ok, init, ready;
    uint32_t gen;
    int64_t  good, bad, need;           /* egymas utani PTP-illeszkedo / nem illeszkedo minta */
    int64_t  floor, end, last, frames;  /* floor alatt minden torolve; end = utolso + 1 */

    uint16_t base_seq;                  /* RFC 3550 A.1 */
    uint32_t ext_seq;
    uint64_t uniq;
    uint8_t  seen[SEQ_WIN / 8];
    uint64_t lost_carry, recv_carry, dup, late;

    int64_t  xmin[2], xmax[2], blk_start;   /* x = most - (S + n): aktualis es elozo blokk */
    int64_t  prev_transit;
    int      have_transit;
    double   jitter;
    uint32_t pkt_frames;
};

static inline int32_t ts_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
static inline int seen_get(jb_t *jb, uint32_t s) { s &= SEQ_WIN - 1; return jb->seen[s >> 3] >> (s & 7) & 1; }
static inline void seen_put(jb_t *jb, uint32_t s, int v)
{
    s &= SEQ_WIN - 1;
    if (v) jb->seen[s >> 3] |= (uint8_t)(1u << (s & 7));
    else   jb->seen[s >> 3] &= (uint8_t)~(1u << (s & 7));
}

static uint32_t expected(const jb_t *jb) { return jb->init ? jb->ext_seq - jb->base_seq + 1 : 0; }

/* [from, to) slotok torlese; csak beerkezett (< end) helyen lehet adat. */
static void clear(jb_t *jb, int64_t from, int64_t to)
{
    if (to > jb->end) to = jb->end;
    if (to - from >= jb->ring_len) to = from + jb->ring_len;
    for (int64_t p = from; p < to; p++)
        memset(&jb->ring[(p & (jb->ring_len - 1)) * jb->ch], 0, jb->ch * sizeof(int32_t));
}

static void reset_locked(jb_t *jb)
{
    uint32_t e = expected(jb);
    jb->lost_carry += e > jb->uniq ? e - jb->uniq : 0;
    jb->recv_carry += jb->uniq;
    memset(jb->ring, 0, (size_t)jb->ring_len * jb->ch * sizeof(int32_t));
    jb->init = jb->ready = 0;
    jb->uniq = 0;
    jb->good = jb->bad = jb->frames = 0;
    jb->have_transit = 0;
    jb->jitter = 0.0;
    jb->gen++;
}

jb_t *jb_create(uint32_t rate, uint8_t channels, uint8_t bps, uint32_t mediaclk, int ptp)
{
    jb_t *jb = calloc(1, sizeof(*jb));
    if (!jb) return NULL;
    jb->rate = rate;
    jb->ch = channels;
    jb->bps = bps;
    jb->fbytes = (size_t)channels * bps;
    jb->mediaclk = mediaclk;
    jb->mode = jb->ptp_ok = ptp ? JB_PTP : JB_ARR;
    jb->need = 2 * (int64_t)rate;
    for (jb->ring_len = 1; jb->ring_len < (int64_t)rate * RING_MS / 1000; jb->ring_len <<= 1) ;
    jb->ring = calloc((size_t)jb->ring_len * channels, sizeof(int32_t));
    if (!jb->ring) { free(jb); return NULL; }
    pthread_mutex_init(&jb->lock, NULL);
    return jb;
}

void jb_destroy(jb_t *jb)
{
    if (!jb) return;
    pthread_mutex_destroy(&jb->lock);
    free(jb->ring);
    free(jb);
}

void jb_reset(jb_t *jb)
{
    pthread_mutex_lock(&jb->lock);
    reset_locked(jb);
    pthread_mutex_unlock(&jb->lock);
}

void jb_allow_ptp(jb_t *jb, int on)
{
    pthread_mutex_lock(&jb->lock);
    jb->ptp_ok = on;
    pthread_mutex_unlock(&jb->lock);
}

static void tai_fix(jb_t *jb, int64_t k)
{
    fprintf(stderr, "JB: a kuldo idobelyege pontosan %+lld s-mal ter el a PTP media oratol "
                    "(TAI/UTC keveredes az egyik gepen), kiegyenlitve.\n", (long long)k);
    jb->mediaclk -= (uint32_t)(k * jb->rate);
}

void jb_push(jb_t *jb, uint32_t ts, uint16_t seq, const uint8_t *data, size_t len,
             uint64_t now_ptp, uint64_t now_raw)
{
    const int64_t rate = jb->rate, n = (int64_t)(len / jb->fbytes);
    if (!len || len % jb->fbytes || n > jb->ring_len / 4) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "JB: %zu B payload nem illik a leirashoz (%u csatorna, %u bajt/minta)\n",
                    len, jb->ch, jb->bps);
        return;
    }
    pthread_mutex_lock(&jb->lock);

restart:;
    /* illeszkedes a helyi PTP media orahoz, egesz masodperces elteressel egyutt */
    const uint32_t tsa = ts - jb->mediaclk;
    const int64_t np = (int64_t)now_ptp;
    const int64_t sp = np + ts_diff(tsa, (uint32_t)np);
    const int64_t xp = np - (sp + n);
    const int64_t k = (xp + (xp >= 0 ? rate / 2 : -rate / 2)) / rate;
    const int64_t xr = xp - k * rate;
    const int fits = k >= -40 && k <= 40 && xr >= -rate * 30 / 1000 && xr <= rate / 4;
    if (fits) { jb->good += n; jb->bad = 0; } else { jb->bad += n; jb->good = 0; }

    if (jb->mode == JB_PTP) {
        if (fits && k) {
            tai_fix(jb, k);
            reset_locked(jb);
            goto restart;
        }
        if (jb->bad >= rate / 4) {
            fprintf(stderr, "JB: a kuldo RTP-idobelyege nem a helyi PTP media orat koveti (nem "
                            "PTP-szinkron, mas grandmaster vagy hibas mediaclk): erkezesi mod.\n");
            jb->mode = JB_ARR;
            jb->need = 30 * rate;
            reset_locked(jb);
            goto restart;
        }
    } else if (jb->ptp_ok && jb->good >= jb->need) {
        if (k) tai_fix(jb, k);
        fprintf(stderr, "JB: a kuldo idobelyege a PTP media orat koveti: PTP mod (AES67).\n");
        jb->mode = JB_PTP;
        reset_locked(jb);
        goto restart;
    }

    const int64_t now = jb->mode == JB_PTP ? np : (int64_t)now_raw;
    int64_t S;
    if (!jb->init) {
        S = jb->mode == JB_PTP ? sp : (int64_t)tsa;
        jb->init = 1;
        jb->base_seq = seq;
        jb->ext_seq = seq;
        memset(jb->seen, 0, sizeof(jb->seen));
        jb->floor = S - 8 * n;          /* indulaskor kisebb ujrarendezes megengedett */
        jb->end = jb->last = S;
        jb->xmin[0] = jb->xmin[1] = INT64_MAX;
        jb->xmax[0] = jb->xmax[1] = INT64_MIN;
        jb->blk_start = now;
    } else {
        S = jb->last + ts_diff(tsa, (uint32_t)jb->last);
        if (S > jb->end + jb->ring_len / 2 || S + n < jb->floor - jb->ring_len / 2) {
            reset_locked(jb);           /* a kuldo ujraindult, vagy nagy ugras */
            goto restart;
        }
        if (S > jb->last) jb->last = S;
        int d = (int16_t)(uint16_t)(seq - (uint16_t)jb->ext_seq);
        if (d <= 0) {
            if (d <= -SEQ_WIN) { jb->late++; goto out; }
            if (seen_get(jb, seq)) { jb->dup++; goto out; }
        } else {
            for (int i = 1; i < d && i <= SEQ_WIN; i++) seen_put(jb, jb->ext_seq + (uint32_t)i, 0);
            jb->ext_seq += (uint32_t)d;
        }
    }
    seen_put(jb, seq, 1);
    jb->uniq++;
    jb->pkt_frames = (uint32_t)n;

    if (S < jb->floor) {                /* (reszben) mar lejatszott helyre esik */
        jb->late++;
        if (S + n <= jb->floor) goto out;
    }
    if (S + n > jb->floor + jb->ring_len) {   /* a lejatszas lemaradt */
        clear(jb, jb->floor, S + n - jb->ring_len);
        jb->floor = S + n - jb->ring_len;
    }

    /* big-endian L16/L24 -> int32, 24 bites skala */
    const uint8_t *p = data;
    for (int64_t f = 0; f < n; f++) {
        int32_t *dst = &jb->ring[((S + f) & (jb->ring_len - 1)) * jb->ch];
        for (int c = 0; c < jb->ch; c++, p += jb->bps) {
            if (S + f < jb->floor) continue;
            dst[c] = jb->bps == 3 ? (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8) >> 8
                                  : (int32_t)(int16_t)(p[0] << 8 | p[1]) * 256;
        }
    }
    if (S + n > jb->end) jb->end = S + n;

    /* erkezesi ido: blokkonkenti minimum (erkezesi modu eltolas) es maximum (atvitel) */
    const int64_t x = now - (S + n);
    if (x < jb->xmin[0]) jb->xmin[0] = x;
    if (x > jb->xmax[0]) jb->xmax[0] = x;
    if (now - jb->blk_start >= BLOCK_S * rate) {
        jb->xmin[1] = jb->xmin[0];
        jb->xmax[1] = jb->xmax[0];
        jb->xmin[0] = INT64_MAX;
        jb->xmax[0] = INT64_MIN;
        jb->blk_start = now;
    }
    const int64_t transit = now - S;    /* RFC 3550 6.4.1 interarrival jitter */
    if (jb->have_transit)
        jb->jitter += ((double)llabs(transit - jb->prev_transit) - jb->jitter) / 16.0;
    jb->prev_transit = transit;
    jb->have_transit = 1;

    jb->frames += n;
    if (!jb->ready)
        jb->ready = jb->mode == JB_PTP ? jb->good >= rate / 10 : jb->frames >= rate / 2;
out:
    pthread_mutex_unlock(&jb->lock);
}

void jb_stats(jb_t *jb, jb_stats_t *o)
{
    pthread_mutex_lock(&jb->lock);
    memset(o, 0, sizeof(*o));
    const double ms = 1000.0 / jb->rate;
    int64_t mn = jb->xmin[0] < jb->xmin[1] ? jb->xmin[0] : jb->xmin[1];
    int64_t mx = jb->xmax[0] > jb->xmax[1] ? jb->xmax[0] : jb->xmax[1];
    o->ready = jb->ready;
    o->mode = jb->mode;
    o->gen = jb->gen;
    o->off = jb->init && jb->mode == JB_ARR && mn != INT64_MAX ? mn : 0;
    o->transit_ms = jb->init && mx != INT64_MIN ? (double)(mx - o->off) * ms : 0.0;
    o->jitter_ms = jb->jitter * ms;
    o->fill_ms = jb->init && jb->end > jb->floor ? (double)(jb->end - jb->floor) * ms : 0.0;
    o->pkt_frames = jb->pkt_frames;
    o->expected = expected(jb);
    o->uniq = (uint32_t)jb->uniq;
    o->ext_seq = jb->ext_seq;
    o->jitter_ts = (uint32_t)(jb->jitter + 0.5);
    o->received = jb->recv_carry + jb->uniq;
    o->lost = jb->lost_carry + (o->expected > jb->uniq ? o->expected - jb->uniq : 0);
    o->dup = jb->dup;
    o->late = jb->late;
    pthread_mutex_unlock(&jb->lock);
}

int jb_read(jb_t *jb, int64_t pos, size_t n, int32_t *out)
{
    int missing = 0;
    const size_t fb = jb->ch * sizeof(int32_t);
    pthread_mutex_lock(&jb->lock);
    for (size_t i = 0; i < n; i++, pos++) {
        int32_t *dst = &out[i * jb->ch];
        if (!jb->init || pos < jb->floor || pos >= jb->end) {
            memset(dst, 0, fb);
            missing += jb->init && pos >= jb->end;
        } else
            memcpy(dst, &jb->ring[(pos & (jb->ring_len - 1)) * jb->ch], fb);
    }
    pthread_mutex_unlock(&jb->lock);
    return missing;
}

void jb_release(jb_t *jb, int64_t pos)
{
    pthread_mutex_lock(&jb->lock);
    if (jb->init && pos > jb->floor) {
        clear(jb, jb->floor, pos);
        jb->floor = pos;
    }
    pthread_mutex_unlock(&jb->lock);
}
