# aes67rx – AES67-2023 vevő szoftveres PTP-vel

Linuxos AES67 vevő (receiver) ALSA kimenettel, Raspberry Pi-re és általános PC-re.
A program az AES67-2023 szabvány vevőkre vonatkozó előírásait követi, amennyire ez
**szoftveres PTP-időbélyegzéssel** lehetséges.

## Fordítás

```bash
sudo apt install build-essential libasound2-dev linuxptp
make
```

## Futtatás

```bash
# kézzel írt / exportált SDP fájllal
sudo ./aes67rx -i eth0 stream.sdp

# a küldő SAP-bejelentéséből (ajánlott: mindig a küldő aktuális leírása)
sudo ./aes67rx -i eth0 sap
sudo ./aes67rx -i eth0 "sap:PipeWire"

# a hálózaton bejelentett streamek listája
./aes67rx -i eth0 -L
```

A root jog a `ptp4l` indításához (rendszeróra hangolása), a valós idejű ütemezéshez és
a memória rögzítéséhez kell.

### Opciók

| Opció | Jelentés |
|---|---|
| `-i IFACE` | hálózati interfész a PTP-hez és a multicasthoz (alap: `eth0`) |
| `-d ESZKÖZ` | ALSA eszköz (alap: `$AES67_ALSA_DEVICE` vagy `plughw:Headphones,0`) |
| `-l MS` | állandó link offset ms-ban; alapértelmezésben automatikus |
| `-D N` | PTP domain felülírása (alap: az SDP `ts-refclk` szerint, különben 0) |
| `-x` | már futó `ptp4l` szolgáltatás használata (`pmc`-vel figyelve) |
| `-n` | PTP nélkül (csak érkezési idő alapú, nem AES67-megfelelő mód) |
| `-T NS` | PTP lock küszöb ns-ban (alap: 2 000 000) |
| `-w S` | PTP lockra várakozás induláskor (alap: 15 s); későbbi locknál a vevő magától PTP módra vált |
| `-L` | SAP-bejelentések listázása |
| `-v` | a ptp4l teljes naplójának kiírása (PTP-hibakereséshez) |

Környezeti változók: `AES67_ALSA_DEVICE`, `AES67_ALSA_PERIOD` (periódusméret keretben),
`AES67_ALSA_PERIODS` (periódusok száma a pufferben, 2–16, alap 4),
`AES67_TAI_OFFSET` (TAI–UTC másodpercben, alap 37), `AES67_PTP4L_ARGS` (további
`ptp4l` kapcsolók).

### A kimenet olvasása

```
RX 2329 ok 2329 lost 0 late 4 dup 0 | jit 0.04 ms fill 6.0 ms | PTP  L=20.0 ms err -4.0 us corr +62.1 ppm xrun 0 resync 0 | PTP +812 ns
```

- `lost` / `late` / `dup`: elveszett, a lejátszás után beérkezett, duplikált csomagok
- `jit`: RFC 3550 szerinti érkezési jitter
- `fill`: a pufferben lévő, még le nem játszott hang
- `PTP` / `ARR`: lejátszási mód (PTP médiaóra, illetve érkezési idő alapú)
- `L`: link offset (AES67 7.4): az RTP-időbélyeg és a DAC-kimenet közötti állandó késleltetés
- `err`: a tényleges és az előírt lejátszási idő szűrt eltérése
- `corr`: a hangkártya órájának kiegyenlítésére alkalmazott sebességkorrekció

## Működés

1. **SDP** fájlból vagy SAP-ból (9. fejezet, E.2 melléklet). Ellenőrzi a 7. és 8.
   fejezet előírásait, és figyelmeztet, ha a küldő leírása nem szabványos.
2. **PTP timeReceiver** (4. fejezet, A melléklet): `ptp4l -S -s -4 -E --clock_servo=linreg`
   (a linreg szervó a szoftveres időbélyegzés zaját jobban tűri; `AES67_PTP_SERVO=pi`
   visszaállítja), a lock a port SLAVE állapotából és az offsetből (mintánkénti vagy
   összegző `rms` sor) adódik, a domain az
   SDP-ből, `announceReceiptTimeout=3`, `logMinDelayReqInterval=0`, PTP-üzenetek
   DSCP EF (46) jelöléssel. A `ptp4l` szoftveres időbélyegzéssel közvetlenül a
   rendszerórát hangolja.
3. **8.2 kapcsolódási döntés**: a domain, a grandmaster-azonosító (GMID) és a
   `traceable` jelzés összevetése a vevő saját PTP-állapotával. Futás közben is
   figyeli a lock elvesztését és a grandmaster-váltást.
4. **Vétel** (6. fejezet): IPv4 unicast és multicast, IGMP (a kernel IGMPv3-mal indul,
   v2-re visszaesik), csend esetén a tagság újraküldése (10.2, 4. megjegyzés).
   CSRC, fejléc-kiterjesztés és padding tolerálva. RTCP receiver report, SDES és BYE,
   DSCP AF41 jelöléssel.
5. **Jitter puffer**: a médiaóra szerint címzett gyűrű (~700 ms). Bármilyen csomagméretet
   kezel, a `ptime`-ra nem támaszkodik (7.2.0). A 32 bites RTP-óra átfordulását a PTP
   médiaórához igazítja (5. fejezet).
6. **Lejátszás** (5. és 7.4): a stream S pozíciójú mintája az `S + mediaclk + L`
   PTP-időpontban szól. A hangkártya kristályórájának eltérését periódusonként mért
   időzítési hiba alapján PI szabályozó és Catmull-Rom interpolációs resampler
   egyenlíti ki (±1000 ppm).
7. **Adaptív link offset** (automatikus módban, `-l` nélkül):
   - ha csomagok a lejátszási pont után érkeznek, 2 ms-mal nő;
   - a szinkron utáni első 5 s késései (indulási löket) nem számítanak;
   - 30 s nyugalom után a mért legkisebb érkezési tartalék alapján csökken, mindig
     1,5 ms tartalékot hagyva, legfeljebb 10 ms-os lépésben;
   - ha egy csökkentés után hamar újra nőnie kell, a szükséges nyugalmi idő
     duplázódik (legfeljebb 10 percig), így nem leng ide-oda;
   - minden változás lassan csúszik (legfeljebb 500 ppm), hallható ugrás nélkül.
8. **Automatikus PTP-módváltás**: ha induláskor a PTP még nem lockolt, a vevő érkezési
   módban kezd, a nem hangolt helyi órával (`CLOCK_MONOTONIC_RAW`), így a ptp4l
   konvergencia közbeni frekvenciahangolása nem zavarja. A lock után passzívan
   ellenőrzi, hogy a küldő időbélyege a PTP-órát követi-e, és ~2 s egyezés után
   egyetlen újraszinkronnal PTP módra vált.
9. **TAI/UTC-keveredés felismerése**: ha a küldő időbélyege pontosan egész
   másodpercekkel (tipikusan 37 s) tér el a PTP médiaórától, mert valamelyik gépen nincs
   beállítva a kernel TAI-offsetje, a vevő ezt jelzi és kiegyenlíti.

Ha a küldő RTP-időbélyege nem a PTP médiaórát követi (például nem PTP-szinkron, vagy
hiányzik a `ts-refclk`/`mediaclk`), a vevő az első ~64 csomag után ezt felismeri, és
**érkezési idő alapú módra** vált. Ekkor is folyamatos, kattanásmentes a lejátszás, de
a késleltetés nem determinisztikus, ezért ez a mód nem AES67-megfelelő.

## Támogatott stream módok (7.2.0: termékdokumentáció)

Vétel:

- mintavételi frekvencia: 44,1 / 48 / 96 kHz
- kódolás: L16 és L24 minden frekvencián
- csatornaszám: 1–64 (a kimeneti eszköz csatornáira leképezve)
- packet time: bármely csomagméret 1 mintától 4096 bájt payloadig. Ebbe a 2. táblázat
  minden értéke beletartozik (125 µs, 250 µs, 333 µs, 1 ms, 4 ms); futás közbeni
  változást is kezel
- vevőpuffer kapacitása: ~700 ms (a 7.5 alapján legalább 20 packet time vagy 20 ms
  kell)

Stream mode capability descriptor (G.2.2), a szabványos tartományra:
`{44100,48000,96000}-{L16,L24}-{1..8}-{21..4000}`. Ennél hosszabb csomag is
fogadható, amíg a payload legfeljebb 4096 bájt.

## A küldő beállítása

A küldő laptop PTP-grandmaster és PipeWire-beállítását lépésenként a
[kuldo/KULDO.md](kuldo/KULDO.md) írja le. A `tai_offset` segédprogram
(`make` lefordítja) a kernel TAI-UTC különbségét állítja be.

## Korlátok

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

## Tesztelés

A vezérlési hurkot szimulált hangkártyával (±150 ppm eltérés, durva pointer-felbontás)
és szimulált küldővel teszteltük PTP és érkezési módban is: a hurok konvergál, az
időzítési hiba néhány tíz µs-ra áll be, és stabil állapotban nincs kattanás.
Valós hálózaton, valódi ptp4l-lel ez még nincs kipróbálva.
