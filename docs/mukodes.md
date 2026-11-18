# Működés

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

[← Vissza a README-hez](../README.md)
