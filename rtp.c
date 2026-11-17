#include "rtp.h"

/* Az RTP fix fejlecenek merete bajtban. */
#define RTP_FIXED_HEADER_SIZE 12

int rtp_parse(const uint8_t *packet, size_t length, rtp_header *out)
{
    /* 1. Egyaltalan elfer-e benne a fix fejlec? */
    if (length < RTP_FIXED_HEADER_SIZE)
        return -1;

    /* 2. Az 1. bajt: VVPXCCCC (V=verzio 2 bit, P=padding, X=extension, C=csrc darab 4 bit) */
    out->version    = packet[0] >> 6;            /* felso 2 bit */
    out->padding    = (packet[0] >> 5) & 1;      /* kovetkezo bit */
    out->extension  = (packet[0] >> 4) & 1;
    out->csrc_count = packet[0] & 0x0F;          /* also 4 bit */

    if (out->version != 2)
        return -1;

    /* 3. A 2. bajt: MPPPPPPP (M=marker 1 bit, P=payload type 7 bit) */
    out->marker       = packet[1] >> 7;
    out->payload_type = packet[1] & 0x7F;

    /* 4. Sorszam: 2 bajt, "big-endian": az elso bajt a nagyobb helyiertek. */
    out->sequence = (uint16_t)((packet[2] << 8) | packet[3]);

    /* 5. Idobelyeg es SSRC: 4-4 bajt, szinten big-endian. */
    out->timestamp = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16)
                   | ((uint32_t)packet[6] << 8)  |  (uint32_t)packet[7];
    out->ssrc      = ((uint32_t)packet[8] << 24) | ((uint32_t)packet[9] << 16)
                   | ((uint32_t)packet[10] << 8) |  (uint32_t)packet[11];

    /* 6. Hol kezdodik a hangadat? A fix fejlec + a CSRC lista (4 bajt darabonkent) utan. */
    size_t offset = RTP_FIXED_HEADER_SIZE + 4u * out->csrc_count;
    if (offset > length)
        return -1;

    /* 7. Ha van kiterjesztes: 4 bajtos fejlec, amiben a 3-4. bajt a hossza 4 bajtos szavakban. */
    if (out->extension) {
        if (offset + 4 > length)
            return -1;
        size_t ext_words = (size_t)((packet[offset + 2] << 8) | packet[offset + 3]);
        offset += 4 + 4 * ext_words;
        if (offset > length)
            return -1;
    }

    /* 8. Ha van padding: az utolso bajt megmondja, hany bajtot kell a vegerol levagni. */
    size_t end = length;
    if (out->padding) {
        size_t pad = packet[length - 1];
        if (pad == 0 || pad > end - offset)
            return -1;
        end -= pad;
    }

    out->payload_offset = offset;
    out->payload_length = end - offset;
    return 0;
}
