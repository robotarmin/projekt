#ifndef RTP_H
#define RTP_H

#include <stdint.h>   /* uint8_t, uint16_t, uint32_t: fix meretu szamtipusok */
#include <stddef.h>   /* size_t: meretek, indexek tipusa */

/* Az RTP fejlec adatai, mar "emberi" formaban kicsomagolva. */
typedef struct {
    uint8_t  version;          /* RTP verzio, AES67-nel mindig 2 */
    uint8_t  padding;          /* 1, ha a csomag vegen kitoltes van */
    uint8_t  extension;        /* 1, ha van fejlec-kiterjesztes */
    uint8_t  csrc_count;       /* hany tovabbi forras-azonosito van a fejlec utan */
    uint8_t  marker;           /* jelzobit (hasznalata a formatumtol fugg) */
    uint8_t  payload_type;     /* a hangformatum azonositoja (pl. 97) */
    uint16_t sequence;         /* sorszam: csomagonkent 1-gyel no */
    uint32_t timestamp;        /* idobelyeg: MINTAkban merve, nem masodpercben */
    uint32_t ssrc;             /* a kuldo azonositoja */
    size_t   payload_offset;   /* hanyadik bajtnal kezdodik a hangadat */
    size_t   payload_length;   /* hany bajt a hangadat */
} rtp_header;

/*
 * Ertelmezi az RTP csomagot.
 *   packet : a csomag bajtjai
 *   length : a csomag hossza bajtban
 *   out    : ide irja az eredmenyt
 * Visszaadott ertek: 0 ha jo a csomag, -1 ha hibas (ekkor 'out' tartalma ne hasznald).
 */
int rtp_parse(const uint8_t *packet, size_t length, rtp_header *out);

#endif
