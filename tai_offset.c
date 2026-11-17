/*
 * tai_offset - a kernel TAI-UTC kulonbsegenek beallitasa / lekerdezese.
 *
 * A PTP (es az AES67 media ora, 5. fejezet) a TAI idoskalan fut. Linuxon a
 * CLOCK_TAI = CLOCK_REALTIME + kernel TAI offset. Ha ez az offset 0 (gyakori,
 * ha nem fut phc2sys vagy leap-masodperc-tudatos NTP), a CLOCK_TAI valojaban UTC,
 * es a PipeWire (clock.id = "tai") timestampjei 37 s-mal elcsusznak.
 *
 *   sudo ./tai_offset        -> 37 s beallitasa
 *   sudo ./tai_offset 37     -> megadott ertek
 *   ./tai_offset -q          -> csak lekerdezes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timex.h>

int main(int argc, char **argv)
{
    struct timex tx;
    memset(&tx, 0, sizeof(tx));

    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        if (adjtimex(&tx) < 0) { perror("adjtimex"); return 1; }
        printf("kernel TAI offset: %d s\n", tx.tai);
        return 0;
    }

    int off = (argc > 1) ? atoi(argv[1]) : 37;
    tx.modes = ADJ_TAI;
    tx.constant = off;
    if (adjtimex(&tx) < 0) { perror("adjtimex (root jog kell)"); return 1; }

    memset(&tx, 0, sizeof(tx));
    adjtimex(&tx);
    printf("kernel TAI offset: %d s\n", tx.tai);
    return tx.tai == off ? 0 : 1;
}
