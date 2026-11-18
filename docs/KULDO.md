# A küldő laptop beállítása AES67-hez (PipeWire + linuxptp)

Cél: a laptop PTP grandmasterként szinkronizálja a hálózatot, a PipeWire pedig
1 ms-os csomagokat küldjön, a PTP-órához kötött RTP-időbélyeggel, és SAP-pal hirdesse
a `ts-refclk` és `mediaclk` sorokat tartalmazó SDP-t.

```
 Laptop (grandmaster)                         Raspberry Pi (vevő)
 ptp4l -S, priority1 100  ── PTP (319/320) ──▶ ptp4l -S -s (aes67rx indítja)
 PipeWire PTP0-Driver (TAI)                    rendszeróra = PTP
 rtp-sink 1 ms, L24       ── RTP 239.69.0.1 ─▶ aes67rx "sap:Laptop AES67"
 rtp-sap                  ── SAP 239.255.255.255 ─▶
```

Eszközök:

- **linuxptp** (`ptp4l`, `pmc`, `phc2sys`): a szabványos Linuxos PTP-megvalósítás.
- **PipeWire** `module-rtp-sink` és `module-rtp-sap`: beépített AES67-küldő SAP-bejelentéssel.
- Alternatívák: az AES67 Linux daemon (Merging RAVENNA ALSA driver alapú, kernelmodult
  igényel), Windowson/macOS-en a Merging RAVENNA/AES67 Virtual Sound Card. Linuxon a
  PipeWire a legegyszerűbb.

---

## 1. Csomagok

```bash
# Arch
sudo pacman -S linuxptp pipewire
# Debian / Ubuntu
sudo apt install linuxptp pipewire
```

PipeWire 1.0 vagy újabb kell (`pipewire --version`), a 0.3.x AES67-támogatása hiányos.

## 2. A hálózati kártya ellenőrzése

```bash
ip -br link                    # az interfész neve, pl. enp0s13f0u3u1
ethtool -T enp0s13f0u3u1
```

- Ha csak `software-transmit` / `software-receive` szerepel, és `PTP Hardware Clock: none`:
  **szoftveres** időbélyegzés kell (ez az útmutató ezt követi).
- A `u3u1` végződés USB-s hálózati csatolót jelez. Ha van beépített Ethernet, azzal
  kisebb a jitter.

Kapcsold ki az energiatakarékos Ethernetet (EEE), mert rontja az időzítést:

```bash
sudo ethtool --set-eee enp0s13f0u3u1 eee off   # ha a kártya nem támogatja, hibát ír: nem gond
```

## 3. TAI-eltolás és a PTP grandmaster

A PTP és az AES67 médiaórája TAI időskálán fut. A Linux `CLOCK_TAI` órája csak akkor
helyes, ha a kernel ismeri a TAI–UTC különbséget (37 s). Ezt a csomag `tai_offset`
programja állítja be (a fő mappában `make tai_offset`):

```bash
sudo ./tai_offset 37
./tai_offset -q                # kernel TAI offset: 37 s
```

Próbaindítás kézzel:

```bash
sudo ptp4l -f kuldo/aes67-gm.cfg -i enp0s13f0u3u1 -m
```

Jó jel: `assuming the grand master role`. Ha `timed out while polling for tx
timestamp` jelenik meg, emeld a `tx_timestamp_timeout` értékét a cfg-ben (100–200).

Tartós beállítás systemd szolgáltatással (a `tai_offset`-et is lefuttatja):

```bash
sudo install -m644 kuldo/aes67-gm.cfg /etc/linuxptp/aes67-gm.cfg
sudo install -m755 tai_offset /usr/local/bin/tai_offset
sudo install -m644 'kuldo/ptp4l-aes67@.service' /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ptp4l-aes67@enp0s13f0u3u1
```

Tűzfal (ha van): engedélyezd az UDP 319 és 320 (PTP), az 5004/5005 (RTP/RTCP) és a 9875
(SAP) portokat a helyi hálózaton.

A laptopon az NTP (systemd-timesyncd vagy chrony) maradhat. Grandmasterként a ptp4l nem
állít az órán, csak hirdeti.

## 4. A laptop PTP-azonosítójának lekérdezése (GMID)

```bash
sudo pmc -u -b 0 'GET DEFAULT_DATA_SET' | grep clockIdentity
#   clockIdentity  a0cec8.fffe.123456
```

Írd át SDP-formára: nagybetű, kötőjelek, a pontok nélkül:
`a0cec8.fffe.123456` → `A0-CE-C8-FF-FE-12-34-56`

## 5. A PipeWire küldő

```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d
cp kuldo/50-aes67.conf ~/.config/pipewire/pipewire.conf.d/
```

Szerkeszd a fájlt:

- `enp0s13f0u3u1` → a saját interfészed (két helyen)
- `sess.ts-refclk = "ptp=IEEE1588-2008:<GMID>:0"` → a 4. lépés azonosítója

A packet time beállítása a fájlban: `sess.min-ptime = 1`, `sess.max-ptime = 1`,
`rtp.ptime = 1`, vagyis 48 minta/csomag (AES67 7.2.1, kötelező).

```bash
systemctl --user restart pipewire pipewire-pulse wireplumber
```

## 6. Hang küldése

```bash
pactl set-default-sink aes67-sink          # minden hang az AES67-re megy
# vagy csak egy fájl:
pw-play --target aes67-sink zene.wav
```

Ellenőrzés:

```bash
pw-top                     # az aes67-sink a PTP0-Driver alatt fusson, ERR = 0
sudo tcpdump -i enp0s13f0u3u1 -n -c 3 udp port 5004
#   UDP, length 300  -> 12 B RTP fejléc + 288 B (48 minta x 2 csatorna x 3 B) = 1 ms
```

## 7. A vevő (Raspberry Pi)

```bash
sudo timedatectl set-ntp false     # FONTOS: az NTP ne állítsa az órát, a ptp4l teszi
sudo ./tai_offset 37               # ajánlott (a vevő enélkül is kezeli)
sudo ./aes67rx -i eth0 -L          # a "Laptop AES67" stream: ptime=1 ms refclk=PTP
sudo ./aes67rx -i eth0 "sap:Laptop AES67"
```

Ha minden rendben van, ezt kell látnod:

```
Lejatszasi mod: PTP media ora (AES67)
Tenyleges csomagmeret: 48 minta (1.000 ms)
RX ... | PTP  L=15.0 ms err ... | PTP +1234 ns
```

ptime-, `ts-refclk`- vagy `mediaclk`-figyelmeztetés nélkül.

## Hibaelhárítás

| Jelenség | Ok, teendő |
|---|---|
| a vevő `ARR` módban marad | a laptopon nem fut a ptp4l, vagy nem a PTP0-Driver hajtja az aes67-sinket (`pw-top`) |
| „pontosan +37 s-mal tér el” üzenet | az egyik gépen nincs beállítva a TAI-offset. A vevő kiegyenlíti, de futtasd a `tai_offset 37`-et |
| a vevő ptp4l-je: `foreign master not using PTP timescale` | szoftveres időbélyegzésű grandmasternél ez várható: a ptp4l UTC-alapú (ARB) időt hirdet. Mindkét gép rendszerórája UTC lesz, a TAI-t a kernel TAI-offsetje adja (`tai_offset 37`), az eltérést a vevő amúgy is felismeri |
| a vevőn `rms` > 100 000 (100 µs) | a laptop USB-s hálózati kártyájának időbélyeg-zaja; beépített Ethernettel vagy hardveres időbélyegzéssel sokkal jobb |
| a vevő lassan (> 20 s) lockol | a vevő a zajtűrő `linreg` szervót használja; ha mégis lassú, a grandmaster cfg-ben próbáld a `logSyncInterval 0` értéket (az A.1 táblázat −4…+1 tartománya megengedi) |
| a vevő PTP-je nem lockol | tűzfal, vagy eltérő domain (mindkét oldalon 0); a vevő a `-D` kapcsolóval állítható |
| a ptime nem 1 ms | régi PipeWire; nézd meg a verziódhoz tartozó `libpipewire-module-rtp-sink` man oldalt |
| szaggat a laptop hangja | emeld a `sess.latency.msec` értékét 5–10-re (a vevő link offsete ehhez igazodik) |
| az SDP-ben rossz a ts-refclk | `./aes67rx -L` mutatja; javítsd a `sess.ts-refclk` értékét |

## Hardveres időbélyegzés (ha később lesz rá lehetőség)

Ha az `ethtool -T` kimenetében szerepel `PTP Hardware Clock: N`:

- a cfg-ben `time_stamping hardware`
- `phc2sys -s CLOCK_REALTIME -c enp... -O 37 -m`: a grandmaster a kártya óráját a
  rendszeróráról hajtja
- a PipeWire driverben `clock.id` helyett `clock.interface = "enp..."`

Ezzel a pontosság 1 µs alá javul.
