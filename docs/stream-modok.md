# Támogatott stream módok (7.2.0: termékdokumentáció)

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

[← Vissza a README-hez](../README.md)
