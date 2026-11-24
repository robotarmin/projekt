# aes67rx

AES67-2023 vevő Linuxra (C, ALSA), és PipeWire-alapú teszt-adó (`kuldo/`).

```bash
make                                   # függőség: libasound2-dev (alsa-lib), linuxptp
./aes67rx -i eth0 -L                   # SAP-pal bejelentett streamek
sudo ./aes67rx -i eth0 "sap:Laptop AES67"
sudo ./aes67rx -i eth0 -d plughw:0,0 stream.sdp
```

Kapcsolók: `./aes67rx -h`. Szolgáltatásként a vevőn: `vevo/aes67rx.service`. Az adó beállítása: `kuldo/` (az install-lépések a fájlok fejlécében).

- **PTP:** a vevő saját `ptp4l`-t indít. Ha a hálózati kártya tud hardveres időbélyegzést, akkor `-H` módban, és a médiaórát közvetlenül a kártya órájából olvassa. Ha nem, akkor szoftveres módban fut (ilyenkor az NTP legyen kikapcsolva). Már futó ptp4l esetén: `-x`.
- **Lejátszás:** a PTP-médiaórához kötve, állandó link offsettel. Ha a küldő nem PTP-szinkron, érkezési idő alapján játszik (ez nem AES67-mód).
- **Vétel:** L16/L24, 44,1/48/96 kHz, 1–64 csatorna. Bármilyen packet time, a csomag legfeljebb ~170 ms lehet (G.2.2 leíró: `{44100,48000,96000}-{L16,L24}-{1..8}-{21..4000}`). A vevőpuffer ~700 ms.
- **Csak vevő:** nem küld, és SIP-et nem használ. A streamet SAP-ból vagy SDP-fájlból veszi; ha a küldő SAP-ban új leírást hirdet, újraindul.

 <sub><sup>A kód mesterséges intelligencia segítségével készült. Ezt az egyetemi szabályzatnak megfelelően itt jelölöm. [AI1]</sup></sub>
