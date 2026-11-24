#!/usr/bin/env python3
"""rtpcheck.py - AES67 RTP-idobelyeg ellenorzo (a vevo gepen futtasd).

Hasznalat:  python3 rtpcheck.py [csoport] [port] [interfesz] [masodperc]
Pelda:      python3 rtpcheck.py 239.69.0.1 5004 eth0 20

Masodpercenkent kiirja:
  - minta/csomag (1 ms-nal 48)
  - hanyszor ugrott az RTP-idobelyeg (ket egymast koveto csomag kozott nem
    pontosan minta/csomag a kulonbseg) es mekkorat (mintaban)
  - az erkezesi ido (a helyi PTP/TAI ora szerinti mediaora) es az RTP-idobelyeg
    kulonbseget ms-ban: ha a kuldo a PTP-orat koveti, ez kicsi pozitiv szam
    (az atviteli ido, nehany ms), es masodpercrol masodpercre alig valtozik.
Az aes67rx mellett is futtathato (SO_REUSEADDR); ha bind hibat ad, allitsd le a vevot.
"""
import socket
import struct
import sys
import time

GROUP = sys.argv[1] if len(sys.argv) > 1 else "239.69.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5004
IFACE = sys.argv[3] if len(sys.argv) > 3 else "eth0"
SECS = int(sys.argv[4]) if len(sys.argv) > 4 else 20
RATE = 48000


def tai_media():
    """A helyi CLOCK_TAI-bol szamolt 32 bites mediaora (mediaclk:direct=0)."""
    return (time.clock_gettime_ns(time.CLOCK_TAI) * RATE // 1_000_000_000) & 0xFFFFFFFF


def sdiff(a, b):
    """Elojeles 32 bites kulonbseg: a - b."""
    d = (a - b) & 0xFFFFFFFF
    return d - 0x100000000 if d >= 0x80000000 else d


s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind((GROUP, PORT))
except OSError as e:
    sys.exit(f"bind hiba: {e}\nFut meg az aes67rx? Allitsd le: sudo systemctl stop aes67rx")
mreqn = struct.pack("4s4si", socket.inet_aton(GROUP), socket.inet_aton("0.0.0.0"),
                    socket.if_nametoindex(IFACE))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreqn)
s.settimeout(2.0)

print(f"Figyeles: {GROUP}:{PORT} ({IFACE}), {SECS} s  (Ctrl+C: kilepes)")
last_seq = last_ts = spp = None
n = lost = jumps = 0
jump_list, offs = [], []
t_end = time.monotonic() + SECS
t_rep = time.monotonic() + 1.0
try:
    while time.monotonic() < t_end:
        try:
            data, _ = s.recvfrom(9000)
        except socket.timeout:
            print("2 s ota nem jott csomag")
            continue
        now = tai_media()
        if len(data) < 12 or (data[0] >> 6) != 2:
            continue
        seq, ts = struct.unpack("!HI", data[2:8])
        n += 1
        if last_seq is not None:
            ds = (seq - last_seq) & 0xFFFF
            dt = sdiff(ts, last_ts)
            if ds == 1:
                if spp is None:
                    spp = dt
                elif dt != spp:
                    jumps += 1
                    jump_list.append(dt - spp)
            elif 1 < ds < 1000:
                lost += ds - 1
        last_seq, last_ts = seq, ts
        offs.append(sdiff(now, ts))
        if time.monotonic() >= t_rep:
            if offs:
                lo = min(offs) / RATE * 1000
                hi = max(offs) / RATE * 1000
                print(f"{n:7d} csomag | {spp} minta/csomag | ugras {jumps:3d} {jump_list[:6]} "
                      f"| vesztes {lost} | erkezes - ts: {lo:+10.2f} .. {hi:+10.2f} ms")
            offs.clear()
            jump_list.clear()
            jumps = 0
            t_rep += 1.0
except KeyboardInterrupt:
    print("\nleallitva")
