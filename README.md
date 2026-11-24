# aes67rx

AES67-2023 vevő Linuxra (Raspberry Pi, PC): a hálózati AES67 streamet PTP-hez
időzítve lejátssza egy ALSA hangkártyán. Szoftveres PTP-vel működik.

Telepítés (adó és vevő): [docs/TELEPITES.md](docs/TELEPITES.md)

## Fordítás

```bash
sudo apt install build-essential libasound2-dev linuxptp
make                     # aes67rx és tai_offset
```

## Használat

```bash
./aes67rx -i eth0 -L                           # bejelentett (SAP) streamek listája
sudo ./aes67rx -i eth0 "sap:Laptop AES67"      # stream név szerint
sudo ./aes67rx -i eth0 sap                     # az első bejelentett stream
sudo ./aes67rx -i eth0 stream.sdp              # SDP fájlból
```

| Opció | Jelentés |
|---|---|
| `-i IFACE` | hálózati interfész (alap: `eth0`) |
| `-d ESZKÖZ` | ALSA eszköz (alap: `plughw:Headphones,0`) |
| `-l MS` | fix link offset ms-ban (alap: automatikus) |
| `-D N` | PTP domain (alap: az SDP szerint) |
| `-x` | már futó `ptp4l` használata |
| `-n` | PTP nélkül, érkezési idő alapján |
| `-T NS` | PTP lock küszöb (alap: 2 000 000 ns) |
| `-w S` | várakozás a PTP lockra induláskor (alap: 15 s) |
| `-v` | a ptp4l naplójának kiírása |

Környezeti változók: `AES67_ALSA_DEVICE`, `AES67_ALSA_PERIOD` (periódus keretben),
`AES67_ALSA_PERIODS` (periódusok száma, alap 4; a Pi jack kimenetén ne legyen 2),
`AES67_TAI_OFFSET` (alap 37), `AES67_PTP4L_ARGS`, `AES67_PTP_SERVO`.

Szolgáltatásként a vevőn: `vevo/aes67rx.service` és `vevo/tai-offset.service`.

## Státuszsor

```
RX 9035 ok 9035 lost 0 late 0 dup 0 | jit 0.18 ms fill 12.0 ms | PTP  L=56.0 ms err +9.9 us corr -4.0 ppm xrun 0 resync 0 | PTP +8165 ns
```

- `PTP` / `ARR` (középen): lejátszás PTP médiaórához, illetve érkezési időhöz kötve
- `L`: link offset, `err`: időzítési hiba, `corr`: hangkártya-sebesség korrekció
- `PTP +… ns` / `NINCS LOCK` (a végén): a vevő PTP-szinkronja

## Dokumentáció

- [Telepítés és hibaelhárítás](docs/TELEPITES.md)
- [Működés és korlátok](docs/mukodes.md)
- [Támogatott stream módok](docs/stream-modok.md)
- `tools/rtpcheck.py`: a beérkező RTP-időbélyegek ellenőrzése
