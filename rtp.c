#include "aes67.h"

/* Az RTP fix fejlecenek merete bajtban (RFC 3550). */
#define RTP_FIXED_HEADER_SIZE 12u

int rtp_parse(const uint8_t *packet, size_t length, rtp_header *out)
{
    /* 0. Alapellenorzesek: ne szegmenseld a hivot. */
    if (packet == NULL || out == NULL)
        return -1;
    if (length < RTP_FIXED_HEADER_SIZE)
        return -1;

    /* 1. 1. bajt: V(2) P(1) X(1) CC(4). */
    uint8_t b0 = packet[0];
    out->version    = (uint8_t)((b0 >> 6) & 0x03u);
    out->padding    = (uint8_t)((b0 >> 5) & 0x01u);
    out->extension  = (uint8_t)((b0 >> 4) & 0x01u);
    out->csrc_count = (uint8_t)( b0        & 0x0Fu);

    if (out->version != 2u)
        return -1;

    /* 2. 2. bajt: M(1) PT(7). */
    uint8_t b1 = packet[1];
    out->marker       = (uint8_t)((b1 >> 7) & 0x01u);
    out->payload_type = (uint8_t)( b1        & 0x7Fu);

    /* 3. Sequence (2 bajt, big-endian). */
    out->sequence = (uint16_t)(((uint16_t)packet[2] << 8) | (uint16_t)packet[3]);

    /* 4. Timestamp es SSRC (4-4 bajt, big-endian). */
    out->timestamp = ((uint32_t)packet[4] << 24)
                   | ((uint32_t)packet[5] << 16)
                   | ((uint32_t)packet[6] <<  8)
                   |  (uint32_t)packet[7];
    out->ssrc      = ((uint32_t)packet[8]  << 24)
                   | ((uint32_t)packet[9]  << 16)
                   | ((uint32_t)packet[10] <<  8)
                   |  (uint32_t)packet[11];

    /* 5. CSRC lista atugrasa (CSRC-darabszam * 4 bajt). */
    size_t offset = RTP_FIXED_HEADER_SIZE + 4u * (size_t)out->csrc_count;
    if (offset > length)
        return -1;

    /* 6. Fejlec-kiterjesztes atugrasa.
     *    4 bajt fejlec: 2 bajt profil + 2 bajt hossz (4 bajtos szavakban). */
    if (out->extension) {
        if (offset + 4u > length)
            return -1;
        size_t ext_words = ((size_t)packet[offset + 2] << 8)
                         |  (size_t)packet[offset + 3];
        /* Tulcsordulas-vedelem: offset + 4 + 4*ext_words ne lepje tul a size_t-et. */
        if (ext_words > (SIZE_MAX - offset - 4u) / 4u)
            return -1;
        offset += 4u + 4u * ext_words;
        if (offset > length)
            return -1;
    }

    /* 7. Padding levagasa.
     *    Az utolso bajt a padding hossza (a sajat magat is szamitva). */
    size_t end = length;
    if (out->padding) {
        uint8_t pad = packet[length - 1u];
        if (pad == 0u)
            return -1;
        if ((size_t)pad > end - offset)
            return -1;
        end -= pad;
    }

    /* 8. Eredmeny. */
    out->payload_offset = offset;
    out->payload_length = end - offset;
    out->payload        = packet + offset;
    return 0;
}