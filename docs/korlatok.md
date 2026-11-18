# Korlátok

- **Szoftveres időbélyegzés**: a PTP pontossága tipikusan 10–1000 µs (USB-s hálózati
  csatolón rosszabb), szemben a hardveres időbélyegzés 1 µs alatti pontosságával. A
  lock küszöb ezért alapértelmezésben 2 ms (`-T`). Ezen belül a lejátszás időzítése a
  PTP-hez kötött, de több vevő között a fázisegyezés csak ennyire pontos.
- **Csak vevő**: küldés (és SIP) nincs. Az AES67 megengedi a csak vevő eszközt (7.0).
- A media profile (A melléklet) timeTransmitter-oldali követelményei, például a
  clockClass-ok, egy csak timeReceiver eszközre nem vonatkoznak.
- **Ha a küldő nem szabványos**: a PipeWire alapbeállítással ~4,29 ms-os csomagokat
  küld `ts-refclk` és `mediaclk` nélkül, ami nem AES67-megfelelő. A vevő ezt is
  lejátssza, érkezési idő alapú módban. Teljes AES67-láncot akkor kapsz, ha a küldőt
  is PTP-re szinkronizálod és 1 ms-os packet time-ra állítod; ennek módja a PipeWire
  verziódtól függ, ezért nézd meg a dokumentációjában.

[← Vissza a README-hez](../README.md)
