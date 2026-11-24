# Támogatott stream módok (7.2.0: termékdokumentáció)

Vétel:

- mintavételi frekvencia: 44,1 / 48 / 96 kHz
- kódolás: L16 és L24 minden frekvencián
- csatornaszám: 1–64 (a kimeneti eszköz csatornáira leképezve)
- packet time: bármely csomagméret 1 mintától a legnagyobb UDP datagramig (~64 KiB
  payload), legfeljebb a vevőpuffer negyedéig (~170 ms). Ebbe a 2. táblázat minden
  értéke beletartozik (125 µs, 250 µs, 333 µs, 1 ms, 4 ms); futás közbeni változást is
  kezel. Az 1440 bájtnál nagyobb payloadot (6.3) a vevő elfogadja, de jelzi
- vevőpuffer kapacitása: ~700 ms (a 7.5 alapján legalább 20 packet time vagy 20 ms
  kell)

Stream mode capability descriptor (G.2.2), a szabványos tartományra:
`{44100,48000,96000}-{L16,L24}-{1..8}-{21..4000}`. Ennél hosszabb csomag is
fogadható a fenti korlátokon belül.

[← Vissza a README-hez](../README.md)
