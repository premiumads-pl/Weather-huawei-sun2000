# Pogoda + Fotowoltaika Huawei SUN2000

Wyświetlacz ścienny na ESP32-S3 + ST7789 (320×240, landscape). Pokazuje prognozę
pogody z Open-Meteo, dane z falownika Huawei SUN2000 przez Modbus TCP oraz mapę
samolotów nad Zatoką Gdańską (ADS-B).

Płytka nie ma przycisków — wszystko przełącza się automatycznie.

## Ekrany

Rotacja co 9 s (mapa lotów: 15 s), przejścia slide, animowane wykresy.
Stała belka górna (miasto, data, zegar, WiFi) i stały pasek PV na dole.

| # | Ekran | Zawartość |
|---|-------|-----------|
| 1 | **TERAZ** | wielka temperatura, ikona, odczuwalna, wiatr + kierunek, wilgotność, ciśnienie, zachmurzenie, wschód/zachód, UV |
| 2 | **GODZINY** | krzywa temperatury na 12 h (gradient), słupki opadów, ikony co 2 h |
| 3 | **5 DNI** | pionowe słupki zakresu min–max, ikony, paski opadów |
| 4 | **PV** | moc chwilowa, wskaźnik obciążenia instalacji, profil produkcji dnia, dziś / dom / sieć / temperatura falownika |
| 5 | **SAMOLOTY** | mapa Zatoki (Hel–Trójmiasto) + lista lotów; zielony = ląduje w Gdańsku, pomarańczowy = startuje z Gdańska |

**Alerty** (burza, ulewa, silny wiatr, mróz, upał, awaria falownika) przerywają
rotację i wyświetlają ostrzeżenie.

**Tryb nocny** — PWM na podświetleniu, przygaszenie 22:00–06:00.

## Konfiguracja — bez sekretów w kodzie

W repozytorium **nie ma** żadnych haseł, adresów IP ani kluczy. Wszystko siedzi
w pamięci NVS urządzenia i ustawia się przez panel WWW.

1. Po pierwszym uruchomieniu urządzenie tworzy sieć **`Pogoda-Setup`**
   (hasło `pogoda123`). Nazwa sieci i adres są pokazane na ekranie.
2. Połącz się telefonem i otwórz `http://192.168.4.1`.
3. W panelu:
   - **Wyszukaj sieci bezprzewodowe** → wybierz swoją → wpisz hasło
   - **Lokalizacja** → wpisz miasto → wybierz z listy (geokoder Open-Meteo)
   - **Falownik** → adres IP (Modbus TCP) i moc szczytowa instalacji w kWp
4. Po połączeniu panel zostaje dostępny w sieci domowej pod adresem IP
   urządzenia (widocznym na ekranie startowym).

Konfiguracja przeżywa aktualizacje OTA i zanik zasilania.

## Aktualizacje OTA

Urządzenie co 15 minut sprawdza
`releases/latest/download/version.json`. Jeśli numer wersji jest wyższy niż
wkompilowany, pobiera `firmware.bin`, zapisuje go na partycji OTA i restartuje
się. Na ekranie widać pasek postępu.

Publikacja nowej wersji:

```bash
./tools/release.sh "co się zmieniło"
```

Skrypt podnosi `FW_VERSION`, kompiluje, commituje, taguje i tworzy Release
z `firmware.bin` + `version.json`.

## Sprzęt

| Sygnał | GPIO |
|--------|------|
| CS  | 10 |
| DC  | 8  |
| RST | 9  |
| MOSI| 11 |
| SCLK| 12 |
| BL  | 14 (PWM) |
| MISO| — |

ESP32-S3 (4 MB flash, 2 MB PSRAM), ST7789 240×320 używany w landscape 320×240,
SPI 27 MHz (HSPI), TFT_eSPI.

## Budowanie

Wymagane: `arduino-cli`, rdzeń `esp32:esp32` ≥ 3.3, biblioteki `TFT_eSPI`,
`ArduinoJson`, `PNGdec` i `PubSubClient` (MQTT).

```bash
arduino-cli lib install TFT_eSPI ArduinoJson PNGdec PubSubClient
```

Skopiuj `User_Setup.h` z tego repo do katalogu biblioteki TFT_eSPI
(nadpisuje domyślną konfigurację — sterownik, piny, `TFT_BGR`, `TFT_INVERSION_OFF`).

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs" .

arduino-cli upload -p /dev/cu.usbmodem101 \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs" .
```

Partycje `min_spiffs` są konieczne — dają dwie partycje aplikacji po 1,9 MB,
bez tego OTA się nie zmieści.

## Źródła danych

- **Pogoda** — [Open-Meteo](https://open-meteo.com) (bez klucza)
- **Geokoder** — Open-Meteo Geocoding API
- **Fotowoltaika** — Huawei SUN2000, Modbus TCP :502
  (rejestry 32064/32080/32106/32114/32016/32086/32087/32089/37100/37113)
- **Samoloty** — [adsb.fi](https://adsb.fi) (ADS-B + MLAT) i trasy z
  vrs-standing-data

## MQTT / Home Assistant

Optional, **off by default**. Turn it on in the web panel under
**MQTT / Home Assistant**: broker address, port (default `1883`), optional
username/password, and a topic prefix (default `pogoda-gdynia`). The broker
password is stored in NVS and is never returned by `/api/state` — the panel only
sees a "password is set" flag.

Once the device connects, it publishes Home Assistant MQTT Discovery configs
(retained, on `homeassistant/sensor/<device-id>/<entity>/config`) **once per
connection**, and they all share one `device` block — so Home Assistant shows a
single device with 22 entities rather than 22 loose ones:

| Group | Entities |
|-------|----------|
| **PV** | AC power, DC power, energy today (`total_increasing`), lifetime energy, grid balance (positive = exporting, negative = importing), house load, inverter temperature, inverter status |
| **Weather** | temperature, apparent temperature, humidity, pressure, wind, cloud cover, UV index, precipitation (mm/h), condition |
| **Device** (diagnostic) | ESP32 temperature, free heap, uptime, Wi-Fi RSSI, firmware version |

States are grouped into one retained JSON per topic (`<prefix>/pv/state`,
`<prefix>/wx/state`, `<prefix>/dev/state`) and picked apart with
`value_template`, which keeps the packet count low: PV every 30 s, weather every
15 min, device telemetry every 60 s.

Availability uses `<prefix>/status` (`online` / `offline`, retained) with an MQTT
Last Will, so Home Assistant marks the entities unavailable if the display drops
off the network.

If the broker is unreachable the device keeps working normally — connection
attempts use short timeouts and back off from 5 s to 5 min, and the failure is
reported on the on-device stats screen and in `GET /api/diag`.

Same settings from the serial console:

```
mqtt <host> [port]        # sets the broker and enables publishing
mqtt off
mqttauth <user> <pass>    # pass "-" clears the password
mqttprefix <prefix>
```

## Konsola serwisowa

Przez USB (115200):

```
wifi <ssid> <haslo>
loc <nazwa> <lat> <lon>
modbus <ip>
peak <W>
mqtt <host> [port]   # mqtt off — wyłącza
mqttauth <user> <haslo>
mqttprefix <prefiks>
show
reset          # kasuje zapisane WiFi
```

## Licencja

MIT
