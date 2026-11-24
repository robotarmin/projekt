# Telepítési és indítási útmutató: AES67 adó és vevő

Ez az útmutató bármilyen Linuxos eszközre szól (laptop, asztali PC, Raspberry Pi, más ARM-lap). Az adó a PipeWire beépített AES67-küldője, a vevő az `aes67rx`. Az első rész szoftveres PTP-vel dolgozik, ez minden hálózati kártyán működik. A [10. fejezet](#10-hardveres-ptp-mi-változik) írja le, mi változik hardveres PTP-időbélyegzésnél.

```
 ADÓ (PTP grandmaster)                          VEVŐ (egy vagy több)
 ptp4l, priority1 100   ── PTP  UDP 319/320 ──▶ ptp4l (az aes67rx indítja)
 PipeWire PTP0-Driver   ── RTP  239.69.0.1:5004 ▶ aes67rx  ──▶ ALSA ──▶ hangszóró
 PipeWire rtp-sap       ── SAP  239.255.255.255:9875 ▶
```

---

## 1. Hálózat

- **Vezetékes Ethernet kell.** Wi-Fi-n a PTP pontatlan, a multicast pedig megbízhatatlan.
- Egy switch elég. Buta (nem menedzselt) switch-en a multicast minden portra eljut, kis hálózaton ez nem gond.
- **Menedzselt switch, bekapcsolt IGMP snoopinggal**: kell egy *IGMP querier* is (a switchen vagy a routeren), különben a switch néhány perc után elfelejti a feliratkozást, és megszakad a hang. A vevő csendnél újraküldi a tagságot, de a querier a biztos megoldás.
- QoS-képes switchnél: DSCP 46 (EF) prioritás a PTP-nek, DSCP 34 (AF41) a hangnak. Mindkét oldal így jelöli a csomagokat.
- Ha a hálózaton már van PTP grandmaster (például Dante- vagy más AES67-eszköz), ne indíts mellé másikat. Lásd [9. fejezet](#9-több-eszköz-meglévő-aes67-hálózat).
- Mindkét gépnek legyen IPv4-címe ugyanabban a hálózatban. Közvetlen kábelnél, DHCP nélkül adj fix címet (például `sudo ip addr add 192.168.50.1/24 dev <interfész>` és `.2` a másik gépen).

## 2. Közös előkészítés (mindkét gépen)

### Csomagok

| Disztribúció | Parancs |
|---|---|
| Debian, Ubuntu, Raspberry Pi OS | `sudo apt install build-essential libasound2-dev linuxptp ethtool` |
| Arch, Manjaro | `sudo pacman -S base-devel alsa-lib linuxptp ethtool` |
| Fedora | `sudo dnf install gcc make alsa-lib-devel linuxptp ethtool` |

Az adón ezen felül PipeWire 1.0 vagy újabb kell (`pipewire --version`). A legtöbb mai disztribúció ezt használja alapból.

### A program lefordítása

```bash
cd projekt
make                     # aes67rx és tai_offset
sudo install -m755 tai_offset /usr/local/bin/tai_offset
```

Az adón csak a `tai_offset` kell, ha ott nincs ALSA-fejléc: `make tai_offset`.

### Az interfész neve és képességei

```bash
ip -br link              # pl. eth0, enp3s0, enp0s13f0u3u1
ethtool -T <interfész>
```

- Csak `software-transmit` / `software-receive`: szoftveres PTP (1–9. fejezet).
- `hardware-transmit`, `hardware-receive` és `PTP Hardware Clock: N`: hardveres PTP is lehetséges (10. fejezet).

### A linuxptp programok helye

```bash
command -v ptp4l phc2sys
```

Arch-on `/usr/bin/`, Debianon és Fedorán `/usr/sbin/`. Az alábbi systemd-unitokban az `ExecStart` útvonalát ehhez igazítsd.

### Energiatakarékos Ethernet kikapcsolása

```bash
sudo ethtool --set-eee <interfész> eee off   # ha a kártya nem tudja, hibát ír: nem gond
```

### Tűzfal

Ha fut tűzfal, engedélyezd a helyi hálózaton: UDP 319, 320 (PTP), 5004, 5005 (RTP, RTCP), 9875 (SAP). Például `ufw`-vel:

```bash
sudo ufw allow from 192.168.0.0/16 proto udp to any port 319,320,5004,5005,9875
```

### TAI-eltolás

A PTP és az AES67 médiaórája TAI-időskálán fut, ami jelenleg 37 s-mal jár az UTC előtt. A kernel ezt az értéket bootkor nem ismeri, a `tai_offset` állítja be. Ez nem módosítja a rendszeróra UTC-idejét.

```bash
sudo tai_offset 37
tai_offset -q            # kernel TAI offset: 37 s
```

Tartósan az adón a ptp4l szolgáltatás, a vevőn a `tai-offset.service` állítja be (lent).

---

## 3. Az adó: PTP grandmaster

A konfiguráció: `kuldo/aes67-gm.cfg` (domain 0, priority1 100, Sync 8/s, Announce 0,5/s, UDPv4, E2E, szoftveres időbélyeg, DSCP 46, UTC-offset 37).

```bash
sudo mkdir -p /etc/linuxptp
sudo install -m644 kuldo/aes67-gm.cfg /etc/linuxptp/aes67-gm.cfg
```

Próbaindítás kézzel (ha a szolgáltatás már fut, előbb állítsd le: `sudo systemctl stop ptp4l-aes67@<interfész>`, mert két ptp4l nem futhat egyszerre, és a kézi példány törli a szolgáltatás kezelő-socketjét, a `/var/run/ptp4l`-t):

```bash
sudo ptp4l -f /etc/linuxptp/aes67-gm.cfg -i <interfész> -m
```

Jó jel: `assuming the grand master role`. Ha `timed out while polling for tx timestamp` jelenik meg, emeld a `tx_timestamp_timeout` értékét a cfg-ben 100–200-ra. A próba után `Ctrl+C`, majd indítsd a szolgáltatást.

### Szolgáltatásként

A `kuldo/ptp4l-aes67@.service` unit USB-s hálózati kártyánál az interfésszel együtt indul és áll le:

```bash
sudo install -m644 'kuldo/ptp4l-aes67@.service' /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ptp4l-aes67@<interfész>
```

Ellenőrzés:

```bash
systemctl status ptp4l-aes67@<interfész> --no-pager   # active (running), enabled
pgrep -a ptp4l                                         # pontosan egy sor
tai_offset -q                                          # 37 s
```

Az NTP (systemd-timesyncd, chrony) az adón maradhat bekapcsolva.

### A grandmaster azonosítója (GMID)

```bash
sudo pmc -u -b 0 'GET DEFAULT_DATA_SET' | grep clockIdentity
#   clockIdentity  a0cec8.fffe.123456
```

A ptp4l naplójában is látszik: `selected local clock a0cec8.fffe.123456 as best master`. SDP-formában nagybetűvel, kötőjelekkel, pontok nélkül: `A0-CE-C8-FF-FE-12-34-56`. Ez a hálózati kártya MAC-címéből származik, ezért másik kártyánál más lesz.

## 4. Az adó: PipeWire AES67-küldő

```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d
cp kuldo/50-aes67.conf ~/.config/pipewire/pipewire.conf.d/
```

Szerkeszd a fájlt:

| Kulcs | Érték |
|---|---|
| `local.ifname` (két helyen) | a saját interfészed |
| `sess.ts-refclk` | `"ptp=IEEE1588-2008:<GMID>:0"` a 3. fejezetből |
| `sess.name` | a stream neve, a vevő ezzel keresi (alap: `Laptop AES67`) |
| `destination.ip` | multicast-cím, alap `239.69.0.1`. Több adónál mindegyik kapjon sajátot |
| `audio.channels`, `audio.position` | csatornaszám, alap 2 (`[ FL FR ]`) |

A packet time 1 ms (`sess.min-ptime = 1`, `sess.max-ptime = 1`, `rtp.ptime = 1`), az AES67 7.2.1 szerint ez kötelező. A formátum L24, 48 kHz.

**Két buktató, amelyet a fájl már kezel. Ne módosítsd őket:**

- **Időbélyeg:** a `stream.props` blokkban szerepel a `sess.ts-direct = true`, a `sess.ts-offset = 0` és a `rtp.sender-ts-offset = 0`. A PipeWire a küldő eltolását a `rtp.sender-ts-offset` kulcsból olvassa, és ha az hiányzik, minden induláskor véletlen eltolást választ. Az SDP-ben ilyenkor is `mediaclk:direct=0` áll, ezért a vevő nem tud PTP módra váltani.
- **Egy meghajtó:** a PTP0-Driver blokkban nincs `object.export = true`, mert attól a fő démonban két PTP0-Driver jön létre.

```bash
systemctl --user restart pipewire pipewire-pulse wireplumber
```

A PipeWire felhasználói szolgáltatás, a bejelentkezéssel indul. Ha bejelentkezés nélkül is futnia kell (például fej nélküli gépen): `sudo loginctl enable-linger <felhasználó>`.

**Ha az interfész IP-címe megváltozik** (az USB-s csatoló ki- és bedugása, DHCP), a PipeWire a régi forráscímmel küld tovább. Ilyenkor indítsd újra a PipeWire-t a fenti paranccsal.

### Hang küldése

```bash
pactl set-default-sink aes67-sink        # minden hang az AES67-re
pw-play --target aes67-sink zene.wav     # vagy csak egy fájl
```

### Ellenőrzés az adón

```bash
pw-cli ls Module | grep -E 'rtp-sap|rtp-sink'     # mindkét modul betöltve
timeout 3 pw-top -b | grep -E 'PTP0|aes67'        # EGY PTP0-Driver, alatta "+ aes67-sink"
pw-dump | grep -E '"(sess.ts-offset|sess.ts-direct|rtp.sender-ts-offset)"'
sudo tcpdump -i <interfész> -n -c 3 udp port 5004
#   <a gép IP-címe>.xxxxx > 239.69.0.1.5004: UDP, length 300   (12 B fejléc + 48 × 2 × 3 B = 1 ms)
sudo timeout 15 tcpdump -i <interfész> -n udp port 9875   # SAP-bejelentés néhány másodpercenként
```

A `pw-top -b` másodpercenként újra kiírja a listát, ezért ugyanaz a sor többször is látszik. Kettő meghajtó csak akkor van, ha egy frissítésen belül két különböző azonosító szerepel. Más csatornaszámnál az RTP-csomag hossza 12 + 144 × csatornaszám bájt.

---

## 5. A vevő: rendszer-előkészítés

### NTP kikapcsolása

A vevőn a ptp4l állítja a rendszerórát, ezért az NTP-nek nem szabad belenyúlnia:

```bash
sudo timedatectl set-ntp false
sudo systemctl disable --now chrony 2>/dev/null   # ha chrony fut
```

### TAI-offset szolgáltatás

```bash
sudo install -m644 vevo/tai-offset.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now tai-offset
```

A vevő e nélkül is működik (felismeri és kiegyenlíti a 37 s-os eltérést), de így szabványos.

### A hangkimenet kiválasztása

```bash
aplay -l
```

Az eszköz neve `plughw:<KÁRTYA>,<ESZKÖZ>` formában:

| Eszköz | Tipikus név | Ajánlott beállítás |
|---|---|---|
| Raspberry Pi analóg (jack) | `plughw:Headphones,0` (alapértelmezett) | alap (4 periódus), **ne 2** |
| Raspberry Pi HDMI | `plughw:vc4hdmi0,0` | alap |
| USB hangkártya, DAC | `plughw:Device,0` vagy a gyártó neve | alap |
| PC beépített hangkártya | `plughw:PCH,0` | alap |
| I2S DAC-HAT (Pi) | a HAT neve, pl. `plughw:sndrpihifiberry,0` | alap |

Próba: `speaker-test -D plughw:<KÁRTYA>,0 -c 2 -t sine`.

Ha a vevőn asztali környezet és PipeWire vagy PulseAudio fut, az lefoglalhatja a hangkártyát. Ilyenkor vagy egy másik kártyát használj, vagy állítsd le a hangszervert a vevőn.

A Pi analóg kimenete nagy (10 ms-os) periódussal dolgozik, és legalább 30–40 ms ALSA-puffer kell neki: 2 periódussal (20 ms) a kimenet jelzés nélkül kihagy, a hang pattog. Ezért ott a link offset kb. 50–56 ms. USB vagy I2S DAC-kal ez jóval kisebb.

## 6. A vevő: indítás

### Kézzel, először

```bash
sudo ./aes67rx -i <interfész> -L                          # a hálózaton bejelentett streamek
sudo ./aes67rx -i <interfész> -d plughw:<KÁRTYA>,0 "sap:Laptop AES67"
```

Az SDP-t mindig SAP-ból vedd (`sap:<név>`), így az mindig az adó aktuális leírása.

Fontosabb kapcsolók: `-d` ALSA-eszköz, `-l MS` fix link offset, `-D N` PTP domain, `-T NS` lock küszöb, `-w S` várakozás a lockra, `-x` külső ptp4l, `-v` a ptp4l teljes naplója. A teljes lista a `README.md`-ben.

### Jó állapot

```
Lejatszasi mod: PTP media ora (AES67)
Tenyleges csomagmeret: 48 minta (1.000 ms)
RX ... lost 0 late 0 ... | PTP  L=… ms err … corr … xrun 0 resync 0 | PTP +… ns
```

A státuszsorban két különböző dolog szerepel:

- **a sor közepén `PTP` vagy `ARR`:** a lejátszási mód. `PTP` akkor, ha a küldő RTP-időbélyege a PTP-órát követi;
- **a sor végén `PTP +… ns` vagy `NINCS LOCK`:** a Pi PTP-szinkronjának állapota.

Lehet úgy is lock, hogy közben a lejátszás `ARR` módban marad. Ilyenkor a küldő időbélyegével van gond, lásd a 8. fejezetet.

### Szolgáltatásként

A `vevo/aes67rx.service` unitban írd át az elérési utat, az interfészt, az eszközt és a stream nevét, majd:

```bash
sudo install -m644 vevo/aes67rx.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now aes67rx
journalctl -u aes67rx -f
```

Az `AES67_ALSA_PERIODS=4` az alapértéket rögzíti; a Pi analóg kimenetén ne vedd 2-re. Más eszköznél add hozzá: `Environment=AES67_ALSA_DEVICE=plughw:<KÁRTYA>,0`.

Ha az adó még nem fut, a vevő a SAP-várakozás után kilép, és 3 s múlva újrapróbálja, így a bekapcsolási sorrend mindegy.

### Az RTP-időbélyeg ellenőrzése

```bash
python3 tools/rtpcheck.py 239.69.0.1 5004 eth0 20
```

Jó állapotban: `48 minta/csomag`, `ugras 0`, `vesztes 0`, és az `erkezes - ts` néhány tized ms vagy néhány ms, másodpercről másodpercre stabil. Ha ez az érték ±37 000 ms körül van, akkor TAI/UTC-eltérésről van szó, amit a vevő kiegyenlít. Ha ezres nagyságrendű másodperc, és a PipeWire minden újraindításakor más, akkor hiányzik a `rtp.sender-ts-offset` (4. fejezet).

## 7. Indítási sorrend, napi használat

Szolgáltatásokkal nincs teendő: mindkét gép bootkor magától indul, a sorrend mindegy.

1. Adó: a `ptp4l-aes67@…` fut, a PipeWire a bejelentkezéssel indul.
2. Vevő: `tai-offset`, majd `aes67rx`, ez elindítja a saját ptp4l-jét.
3. Bármilyen hang, ami az `aes67-sink`-re megy, a vevőn szól.

Leállítás, kikapcsolás:

```bash
# vevő
sudo systemctl stop aes67rx            # most
sudo systemctl disable aes67rx         # bootkor se

# adó
sudo systemctl disable --now ptp4l-aes67@<interfész>
mv ~/.config/pipewire/pipewire.conf.d/50-aes67.conf{,.off}
systemctl --user restart pipewire pipewire-pulse wireplumber
```

## 8. Hibaelhárítás

| Jelenség | Ok, teendő |
|---|---|
| a vevő `-L` listája üres | tűzfal (9875), más interfész, vagy a PipeWire nem töltötte be a konfigot (`journalctl --user -u pipewire`) |
| a vevő `ARR` módban marad, pedig a sor végén `PTP +… ns` (lock) áll | a küldő RTP-időbélyege nem a PTP-órát követi: futtasd az `rtpcheck.py`-t (6. fejezet), és ellenőrizd a `rtp.sender-ts-offset` kulcsot (4. fejezet) |
| a vevő `ARR` módban marad, és a sor végén `NINCS LOCK` áll | az adón nem fut a ptp4l, tűzfal (319/320), vagy eltérő domain (`-D`) |
| a `pw-top`-ban két PTP0-Driver (két különböző ID) | `object.export = true` a PTP0-Driver blokkban, töröld |
| a tcpdump szerint a csomagok forráscíme nem az interfész jelenlegi címe | IP-váltás után nem indult újra a PipeWire |
| `sendmsg() failed: A hálózat elérhetetlen` a PipeWire naplójában | az interfész lement vagy nincs IPv4-címe |
| „eltérő grandmaster” megjegyzés | a `sess.ts-refclk` GMID-je nem az adó tényleges azonosítója |
| „pontosan +37 s-mal tér el” | az egyik gépen nincs beállítva a TAI-offset |
| `foreign master not using PTP timescale` | szoftveres grandmasternél várható, nem hiba |
| `uds: removed existing /var/run/ptp4l` a kézi ptp4l-ben | előző futásból maradt socket; ha közben a szolgáltatás is fut, két ptp4l megy: állítsd le a kézit, és indítsd újra a szolgáltatást |
| a hang néhány perc után megszakad | menedzselt switch IGMP querier nélkül |
| `late` folyamatosan nő, a link offset sokat nő | hálózati terhelés, Wi-Fi, vagy túl kicsi ALSA-puffer: emeld az `AES67_ALSA_PERIODS` értékét (6–8) |
| `xrun` nő | túl kicsi ALSA-puffer vagy terhelt CPU; emeld az `AES67_ALSA_PERIODS` értékét |
| a `corr` a ±1000 ppm-es határon áll, az `err` folyamatosan csúszik, a `resync` pár másodpercenként nő, a hang pattog | túl kicsi az ALSA-puffer, a kimenet jelzés nélkül kihagy (Pi jack: 2 periódus). Emeld az `AES67_ALSA_PERIODS` értékét 4-re vagy többre. Ellenőrzés: `aplay --period-size=480 --buffer-size=960` 60 s hanggal 60,4 s-ig tart, 1920-szal 60,0 s-ig |
| a ptime nem 1 ms (tcpdump hossza nem 300) | régi PipeWire |
| az adó hangja szaggat | `sess.latency.msec` 5–10 |
| `Device or resource busy` | a hangkártyát más program (PipeWire, PulseAudio) foglalja |
| a hang fémes, fázisos, „bitcrush”-szerű, pedig nincs vesztés és a `corr` kicsi | egy másik program is lejátssza ugyanazt a streamet (pl. a vevő PipeWire-jében egy `module-rtp-source`), és a két, időben eltolt példány összeadódik. Ellenőrzés: `sudo fuser -v /dev/snd/*`; csak az `aes67rx` használhatja a kimenetet |
| a vevő ptp4l `rms` értéke > 100 µs | USB-s hálózati kártya zaja; beépített Ethernet vagy hardveres PTP javít |

## 9. Több eszköz, meglévő AES67-hálózat

- **Több vevő ugyanarra a streamre**: mindegyiken ugyanaz a parancs. A multicast miatt az adó terhelése nem nő. Szoftveres PTP-nél a vevők közötti fáziseltérés a PTP pontosságával egyezik (tíz–száz µs).
- **Több adó**: mindegyik kapjon saját `destination.ip`-t és `sess.name`-et. Grandmaster csak egy legyen: a többi adón a cfg-ben `priority1 200`, vagy egyáltalán ne fusson grandmasterként, hanem timeReceiverként (`slaveOnly 1`) kövesse az elsőt.
- **Meglévő AES67-hálózat** (Dante AES67 módban, Ravenna, stb.): ott általában már van grandmaster, ezért a laptopon ne fusson a `ptp4l-aes67`. Helyette timeReceiverként szinkronizálódjon, és a `sess.ts-refclk`-ben a hálózati grandmaster GMID-je és domainje álljon. A külső eszközök szoftveres PTP mellett csak korlátozottan működnek együtt; ehhez a gyakorlatban hardveres PTP kell (10. fejezet).
- **Windows, macOS**: PipeWire nincs, ott kereskedelmi AES67-meghajtó (virtuális hangkártya) kell. A vevő bármilyen szabványos AES67-adót fogad.

---

## 10. Hardveres PTP: mi változik?

### Melyik kártya tudja?

`ethtool -T <interfész>` kimenetében `hardware-transmit`, `hardware-receive`, `hardware-raw-clock` és `PTP Hardware Clock: N` (N a `/dev/ptpN` sorszáma). Tipikusan az Intel i210, i211, i225, i226 kártyák tudják. A Raspberry Pi-k közül a CM4 és a Pi 5 Ethernetje támogatja, a Pi 4-é nem. USB-s hálózati csatolók ritkán. Mindig az `ethtool -T` dönt.

### Mi javul

| | Szoftveres | Hardveres |
|---|---|---|
| PTP-pontosság | 10–1000 µs | 1 µs alatt (jó switch mellett) |
| Vevők közötti fáziseltérés | tíz–száz µs | 1 µs körül, gyakorlatilag mintapontos |
| Időskála | a grandmaster UTC-alapú (ARB) időt hirdet | valódi PTP (TAI) időskála |
| Együttműködés más AES67-eszközzel | korlátozott | szabványos |
| Lock ideje | 20–40 s | néhány s |

A hangminőség és a hálózati jitter nem változik. A link offsetet továbbra is a hálózati jitter és a hangkártya puffere határozza meg, a Pi analóg kimenetén továbbra is ~50 ms.

### Az elv

Hardveres PTP-nél a ptp4l nem a rendszerórát, hanem a hálózati kártya saját óráját (PHC) hangolja, ez TAI-ban jár. A rendszerórát a `phc2sys` köti hozzá:

- **adón**: rendszeróra → PHC (a grandmaster ideje a rendszerórából jön)
- **vevőn**: PHC → rendszeróra (a vevő a rendszerórából számolja a médiaórát)

A `phc2sys -a` automatikus módban a ptp4l-től kérdezi le a port állapotát és a TAI–UTC különbséget, így kézi `-O 37` sem kell. Ehhez a ptp4l-nek már futnia kell; ha még nem fut, a phc2sys kilép, és a `Restart=always` újraindítja.

### Az adón

A `/etc/linuxptp/aes67-gm.cfg`-ben:

```ini
time_stamping           hardware
tx_timestamp_timeout    10
```

Új szolgáltatás, `/etc/systemd/system/phc2sys-aes67.service`:

```ini
[Unit]
Description=phc2sys (AES67 ado)
After=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/phc2sys -a -rr -m
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

A `-rr` azt jelenti: a rendszeróra lehet forrás is, ha a gép grandmaster. Engedélyezés: `sudo systemctl enable --now phc2sys-aes67`.

A PipeWire-driverben (`50-aes67.conf`) a `clock.id = "tai"` sor helyett közvetlenül a kártya óráját is olvashatod:

```
clock.interface = "<interfész>"
```

Ez pontosabb, mert kihagyja a phc2sys-láncot. A `clock.id = "tai"` is működik, ha a phc2sys fut.

A GMID a hardveres kártya MAC-címéből jön: ha kártyát cseréltél, kérdezd le újra (3. fejezet), és írd át a `sess.ts-refclk`-ben.

### A vevőn

A vevő beépített ptp4l-je szoftveres (`-S`). Hardveres PTP-nél a ptp4l és a phc2sys külön szolgáltatásként fut, az `aes67rx` pedig `-x` kapcsolóval csak figyeli.

`/etc/linuxptp/aes67-rx.cfg`:

```ini
[global]
domainNumber            0
slaveOnly               1
network_transport       UDPv4
delay_mechanism         E2E
time_stamping           hardware
announceReceiptTimeout  3
logMinDelayReqInterval  0
dscp_event              46
dscp_general            46
```

`/etc/systemd/system/ptp4l-aes67rx@.service`:

```ini
[Unit]
Description=AES67 PTP timeReceiver (%i)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/sbin/ptp4l -f /etc/linuxptp/aes67-rx.cfg -i %i -m
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

`/etc/systemd/system/phc2sys-aes67rx.service`:

```ini
[Unit]
Description=phc2sys (AES67 vevo)
After=network-online.target

[Service]
Type=simple
ExecStart=/usr/sbin/phc2sys -a -r -m
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now tai-offset ptp4l-aes67rx@eth0 phc2sys-aes67rx
```

Az `aes67rx.service`-ben az ExecStart:

```
ExecStart=/home/<felhasználó>/projekt/aes67rx -x -T 10000 -i eth0 "sap:Laptop AES67"
```

és az `[Unit]` részbe: `After=ptp4l-aes67rx@eth0.service phc2sys-aes67rx.service`.

- `-x`: a már futó ptp4l-t figyeli, nem indít sajátot.
- `-T 10000`: a lock küszöb 10 µs-ra szűkíthető (szoftveresen 2 ms).
- A phc2sys **kötelező**: nélküle a ptp4l lockol, de a rendszeróra nem követi, és a vevő `ARR` módba esik.

### A hálózat

Hardveres PTP-nél a switch válik a legnagyobb hibaforrássá. Egy sima switch néhány µs-os, terheléstől függő késést okoz. Ha 1 µs alatti pontosság kell nagyobb hálózaton, PTP-képes (transparent vagy boundary clock) switch kell.

### Ami hardveres PTP-vel is marad

- A hangkártya saját kristályról jár, nem a PTP-ről. A vevő ezt továbbra is resamplinggel egyenlíti ki (a `corr` érték). Valódi stúdióeszközben a DAC óráját egy PTP-hez kötött PLL hajtja.
- A rendszeróra–hangkártya időzítés mérése (ALSA) user-space-ben történik, ez néhány tíz µs zajt ad. A hallható eredményt ez nem rontja.
- Az USB-s hálózati kártya zaja eltűnik, ha hardveres időbélyegzésű, beépített kártyára váltasz.
