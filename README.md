# VET-woonhuis

Interactief demonstratiemodel waarmee vermogensstromen en lokale overbelasting in een woonhuisinstallatie zichtbaar worden gemaakt met RFID-apparaatmodellen en adresseerbare RGB-leds.

## Eerste ESP-IDF-proef

De huidige firmware test één Mini RC522-lezer en één WS2812B/NeoPixel-achtige strip met 24 RGB-leds.

### Aansluitingen

| RC522 | ESP32 |
|---|---:|
| SDA / CS | GPIO13 |
| SCK | GPIO18 |
| MOSI | GPIO23 |
| MISO | GPIO19 |
| RST | 3,3 V |
| 3V3 | 3,3 V |
| GND | GND |
| IRQ | niet aangesloten |

| Ledstrip | Aansluiting |
|---|---|
| DIN | GPIO27 via 330–470 ohm en bij voorkeur een 74AHCT125-levelshifter |
| +5 V | aparte 5 V-voeding |
| GND | gezamenlijke GND met ESP32 en RC522 |

Voed de ledstrip niet vanuit de 3,3 V-pin van de ESP32. De proefcode begrenst de helderheid softwarematig, maar een aparte 5 V-voeding blijft nodig.

### Gedrag

- geen RFID-kaart: een groen stand-bypunt loopt over de strip;
- kaart gedetecteerd: UID verschijnt in de seriële monitor en het spoor wordt blauw;
- kaart verwijderd: de strip knippert kort rood en keert terug naar groen.

### Bouwen en uploaden

Open een ESP-IDF-terminal in deze projectmap en voer uit:

```sh
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

Met een expliciete seriële poort, bijvoorbeeld op macOS:

```sh
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

Stop de monitor met `Ctrl+]`.

De proef gebruikt een kleine lokale MFRC522-driver, omdat de beschikbare externe RC522-component nog uitsluitend ESP-IDF 5.x accepteert. Deze eerste driver leest vierbyte-UID's, zoals die van veel MIFARE Classic-kaarten. De officiële `espressif/led_strip`-component is vastgezet op versie 3.0.3.

Het project is via `sdkconfig.defaults` ingesteld op de 4 MB SPI-flash van het gebruikte ESP32-board.

## Ontwerp

Het volledige technische concept staat in [ONTWERP.md](ONTWERP.md).
