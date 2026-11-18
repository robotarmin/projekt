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

## Dokumentáció

- [Működés](docs/mukodes.md)
- [Támogatott stream módok (7.2.0: termékdokumentáció)](docs/stream-modok.md)
- [Korlátok](docs/korlatok.md)
- [Tesztelés](docs/teszteles.md)
- [A küldő laptop beállítása lépésenként](docs/KULDO.md)
