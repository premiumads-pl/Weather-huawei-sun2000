# ARCHITEKTURA — gdzie mieszka dana informacja

Mapa dla kogoś, kto wraca do projektu po pół roku. Odpowiada na jedno pytanie:
**„szukam informacji X — gdzie ona jest?”**

Sprzęt: ESP32-S3 + ST7789 320×240, 2 MB PSRAM. Urządzenie wisi w łazience,
**bez USB**. Jedyna droga do niego: panel WWW i OTA z GitHub Releases. Repo jest publiczne.

Stan na v93 (`Version.h`). ~9,8 tys. linii kodu + ~4,7 tys. wygenerowanych danych
(`WeatherIcons.h`, `PlFont*.h`, `MapData*.h` — dane, nie kod, nie czytaj ich).

---

## 1. Przepływ danych — kto pobiera, kto trzyma, kto rysuje

Zasada: **netTask pobiera i pisze do `g*`, loop() kopiuje `g*` → `ui*` pod `gLock`, rysuje z `ui*`.**
Kod rysujący **nigdy** nie dotyka `g*` i nigdy nie robi I/O.

```
ŹRÓDŁO (sieć/radio)          POBIERA (netTask, rdzeń 0)      TRZYMA (globalne)     RYSUJE (loop, rdzeń 1)
────────────────────────────────────────────────────────────────────────────────────────────────────────
api.open-meteo.com       →   WeatherClient::fetch()      →   gWeather  → uiWeather → drawViewNow/Hours/Days
  (WeatherClient.cpp:31)     co 15 min (WEATHER_REFRESH_MS)                          + drawHeader
tilecache.rainviewer.com →   RadarClient::fetch()        →   gWeather.radar*        → pasek opadu w TERAZ
  (punktowy poziom opadu)    co 5 min (RADAR_REFRESH_MS)      ↑ netTask NIE kasuje
                                                              tego świeżą prognozą
                                                              (.ino:240-242)
tilecache.rainviewer.com →   radarmap::fetch()           →   radarmap:: (własny     → drawViewRadar
  (7 kafelków PNG, 2 h)      co 10 min (RADAR_MAP_REFRESH)    mutex, RadarMap.cpp:31)
falownik Huawei          →   PvClient::fetch()           →   gPv       → uiPv      → drawViewPv + stopka
  (Modbus TCP, LAN)          co 30 s / 5 min w nocy           gHist     → uiHist    → sparkline profilu
opendata.adsb.fi         →   FlightClient::fetch()       →   gFlights  → uiFlights → drawViewFlights
  (ADS-B)                    co 15 s, TYLKO gdy ekran aktywny (gFlightsNeeded)
api.viessmann-...com     →   vi::fetch()                 →   gVi       → uiVi      → drawViewBoiler
  (chmura ViCare)            co 3 min                         gBurner   → uiBurner  → drawGasChart
                                                              gGas      → NIKT      → ⚠ patrz §7
czujniki Xiaomi (BLE)    →   ble::scan(4) co 20 s        →   ble:: (własny mutex)  → drawViewHome
  + Shelly jako bramka   →   blegw::poll() co 20 s           gRooms    → uiRooms   → wykres 24 h
                             (BleGateway.cpp)
GitHub Releases          →   Ota::checkAndUpdate()       →   otaStatus()           → drawOtaDirect
                             co 15 min (OTA_CHECK_MS)                                (bez bufora!)
```

**Wyjątki od zasady „`ui*` do rysowania”** — świadome, udokumentowane:
- `gRooms`/`uiRooms`, `gVi`/`uiVi`, `gBurner`/`uiBurner` idą do UI **wskaźnikiem**, nie kopią
  przez sygnaturę (`WeatherUi.h:81-83`, ustawiane raz w `setup()`, `.ino:707-709`). Powód:
  `RoomHistory` ma 1736 B, przewlekanie przez `render/paintFrame/drawView` zaśmiecałoby
  cztery sygnatury. `loop()` i tak odświeża `uiRooms` pod `gLock` (`.ino:841`).
- `diag()` (`Log.h:13`) czytają **wszyscy**, piszą **wszyscy**, **bez blokady**. To migawka
  diagnostyczna — pojedyncze pole rozjechane o klatkę nikogo nie boli. Nie stawiać na niej logiki.
- `settings()` czyta każdy wątek bez blokady; pisze webTask (panel) i netTask (token Viessmanna).
  Patrz §2 i `audyt/A-zrodla-prawdy.md` §3.

**Kierunek na zewnątrz:** `mqttha::publish*()` woła **netTask** zaraz po udanym `fetch`
(`.ino:251, 318, 418`) — nie ma osobnego cyklu MQTT dla danych. Telemetria samego urządzenia
(`publishDevice`) leci z `mqttha::loop()` co 60 s (`MqttClient.cpp:37`).

---

## 2. Wątki — kto na czym siedzi i co chroni `gLock`

| Wątek | Rdzeń | Prio | Stos | Robi |
|---|---|---|---|---|
| `loop()` (loopTask, Arduino) | **1** | 1 | 8 kB (core) | **rysuje**, dotyk, dioda, tryb nocny, alerty, taktowanie klatek |
| `netTask` (`.ino:750`) | **0** | 3 | 16 kB | **całe I/O**: pogoda, PV, radar, loty, piec, BLE, MQTT, OTA, roaming, zapisy NVS |
| `webTask` (`.ino:749`) | **0** | 2 | 16 kB | panel HTTP, zrzut ekranu, konsola szeregowa |
| `esp_timer` | — | wysoki | 8 kB (sdkconfig) | twardy limit okresu próbnego OTA (`OtaGuard.cpp:274-283`) |
| zdarzenia WiFi | — | — | — | `onRssiLow()` (`.ino:99`) — **tylko podnosi flagę**, nic więcej |
| stos Bluetooth | — | — | — | ~72 kB sterty w czasie `ble::scan()` |

**`gLock`** (`.ino:61`, mutex FreeRTOS) chroni **wyłącznie wymianę modeli między netTask a loop()**:

```
gWeather  gPv  gHist  gRooms  gVi  gBurner  gGas  gFlights
```

Wzorzec jest zawsze ten sam: **weź → skopiuj → oddaj**, nigdy „weź → rób I/O → oddaj”.
Jedyne miejsce, gdzie to widać wprost: `.ino:438-444` — `roomHistorySave()` dostaje **kopię
zdjętą pod mutexem**, a sam zapis do NVS leci **poza mutexem**, bo trwa. Tak samo `.ino:503-508`.

**NIE chroni `gLock`:**
- `diag()` — patrz §1.
- `settings()` — **dziura**, patrz `audyt/A-zrodla-prawdy.md` §3.
- `ble::` — ma **własny** mutex `gMx` (`BleSensors.cpp:22`), bo dane wpadają z callbacku BLE.
- `radarmap::` — ma **własny** mutex `gMx` (`RadarMap.cpp:31`).
- Log — ma **własny** `portMUX` (`Log.cpp:12`).
- `OtaGuard` — ma **własny** `portMUX` na werdykt (`OtaGuard.cpp:41`), bo werdykt mogą próbować
  wydać trzy różne konteksty i wolno go wydać **dokładnie raz** (`claimVerdict()`, `:91`).

**Flagi `volatile` zamiast blokad** (jednokierunkowe, jeden pisarz): `gWifiOk`, `gBooting`,
`gFlightsNeeded`, `gRoamWanted`, `gRadarWantMem`, `gRadarMemReady`, `gRssi`, `gWifiAttempt`,
`gNetInfoUntil` (`.ino:73-113`).

**Handshake o pamięć dla radaru** (`.ino:764-772` ↔ `.ino:322-336`) — jedyny protokół
między rdzeniami:
```
netTask: potrzebuję 47 kB w jednym kawałku → gRadarWantMem = true, czekam max 3 s
loop():  widzę flagę → ui.releaseBuffer(false) → gRadarMemReady = true, nie rysuję
netTask: dekoduję PNG → gRadarWantMem = false
loop():  render() sam odtworzy bufor przy najbliższej klatce (WeatherUi.cpp:1331)
```
`releaseBuffer(false)` = **bez czyszczenia ekranu**, więc panel zostaje z ostatnią klatką zamiast
mrugnąć na czarno. Ekran zamiera na ~sekundę raz na 5 minut. Od v50 (PSRAM) to rzadkie —
warunek `ESP.getMaxAllocHeap() < 48000` (`.ino:324`) prawie nie trafia.

**Zrzut ekranu nie zatrzymuje rysowania** (`.ino:722-731`, `WeatherUi.cpp:2667`): leci z webTask
(rdzeń 0) i **rysuje własną kopię klatki** do sprite'a 320×24, zamiast czytać bufor wyświetlacza.
Czeka tylko na `stableFrame()` (nie w trakcie przejścia/alertu) i na koniec `ble::scanning()`
(bo stos BT trzyma wtedy 72 kB).

---

## 3. Gdzie są stałe i dlaczego akurat tam

**`Config.h` (71 stałych) — wszystko, co ktoś mógłby chcieć pokręcić, nie znając kodu.**
Adresy OTA, piny, wymiary ekranu, siatka układu, czasy odświeżania, czasy trzymania widoków,
progi zdrowia (`CPU_T_*`, `HEAP_*`), indeksy widoków. **W `Config.h` nie ma i nie będzie sekretów**
— mówi o tym `Config.h:5-7` i to jest bariera, nie konwencja (§5).

**Poza `Config.h` — słusznie lokalne (nie przenosić):**

| Gdzie | Co | Dlaczego tam |
|---|---|---|
| `MqttClient.cpp:30-37` | `kBufSize=512`, `kKeepAliveS`, `kSockTimeoutS`, `kConnTimeoutMs`, `kBackoff*`, `kDevPublishMs` | to **budżet pamięci PubSubClienta**, sprzężony z długością prefiksu i payloadu discovery. Ma sens tylko razem z `publishConfig()` obok. Wyciągnięcie do `Config.h` zerwałoby związek z komentarzem `:22-29`, który jest tu ważniejszy niż stała. |
| `Viessmann.cpp:18-20` | `kIam`, `kApi`, `kRefreshTtlDays=180` | adresy jednego dostawcy + TTL jego tokena. Nikt tego nie stroi; zmienią się razem z całym plikiem. |
| `.ino:94-95` | `kRssiRoamBelow=-67`, `kRssiRoamGain=8` | próg roamingu — używany w jednym pliku, w dwóch miejscach obok siebie. |
| `.ino:109` | `NET_INFO_MS=10000` | czas ekranu „połączono”, jeden plik. |
| `WeatherUi.cpp:521` | `HDR_Y=46` | **wzorzec do naśladowania**: jedna stała, jeden nagłówek dla 9 widoków. Przed nią każdy ekran miał własną linię bazową (38/42/46) i wyglądały jak z trzech aplikacji. |
| `WeatherUi.cpp:539` | `kDescMaxW=116` | wynika z `icx=258` i szerokości ekranu; ma sens tylko przy `drawWeatherDesc`. |
| `WeatherUi.h:110-119` | `VIEW_H=206`, `BAND_H`, `BAND_N`, `SHOT_H=24` | **prywatna geometria bufora**. `VIEW_H` to jedyne żywe źródło liczby 206 (`cfg::FOOTER_Y=208` jest martwe i nieprawdziwe — patrz audyt §9). |
| `WeatherData.h:6-7,99` | `WX_HOURS=12`, `WX_DAYS=5`, `PV_SLEEP_MARGIN_MIN=30` | kształt modelu pogody — należy do modelu, nie do konfiguracji. |
| `RoomHistory.h:16,22` · `PvData.h:33` · `GasMeter.h:19,98` | `SLOTS`, `ROOMS`, `DAYS` | kształt struktur. **Ale:** „slot = 10 min” jest tu powtórzone 5 razy i rozjeżdża się z `WeatherUi.cpp` — patrz audyt §10. |

**Zasada, którą warto trzymać:** stała używana w jednym pliku **ma prawo tam zostać**.
Do `Config.h` idzie to, co (a) ktoś realnie stroi, albo (b) jest używane w ≥2 plikach.
`Config.h` nie jest workiem na stałe.

**Kolory** — `Colors.h`, jedno źródło, `col::*`. Zasada z `WeatherUi.cpp:518-519`:
**chrome zawsze tak samo, kolorem mówią wyłącznie DANE.** Kolory wykresu PV są **dokładnie** te,
którymi świeci dioda RGB (`WeatherUi.cpp:1029-1033`) — jeden kod barw w całym urządzeniu,
dlatego wykres czyta się bez legendy. Tak samo `roomCol[]` (`:2179`) = kolor paska kafelka =
kolor linii na wykresie = **cała legenda** ekranu „W DOMU”.

---

## 4. Układ NVS — klucze i wersjonowanie

Trzy przestrzenie nazw. **Wszystkie sekrety są tutaj i nigdzie indziej.**

### `"pogoda"` (`NS_CFG`, `Settings.cpp:14`) — konfiguracja
| Klucz | Typ | Co | Sekret? |
|---|---|---|---|
| `ssid`, `pass` | String | WiFi | **TAK** (`pass`) |
| `city`, `lat`, `lon` | String, Float, Float | lokalizacja pogody | nie |
| `mb`, `mbport`, `peak` | String, UShort, UShort | falownik (IP, port, moc szczytowa) | **TAK** (`mb` = IP w LAN) |
| `ota` | Bool | OTA włączone | nie |
| `mqhost`, `mqport`, `mqpre`, `mquser` | String/UShort | MQTT | nie |
| `mqpass` | String | hasło brokera | **TAK** |
| `vicid` | String | Viessmann Client ID | nie (publiczny — jest w każdej instalacji PyViCare) |
| `viref` | String (600 B) | **refresh token — 180 dni pełnego dostępu do ogrzewania** | **TAK, najwrażliwszy** |
| `viinst`, `vigw`, `viat`, `vien` | String/UInt/Bool | cache instalacji + epoch autoryzacji | nie |
| `blegw` | String | IP bramki Shelly | **TAK** (IP w LAN) |
| `b0mac`…`b7mac`, `b0nam`…`b7nam` | String | czujniki BLE | nie |
| `b0key`…`b7key` | Bytes (16 B) | **bindkey z chmury Xiaomi** | **TAK** |
| `mets` | Bytes (64 B) | odczyty licznika gazu | nie — **⚠ blob martwy, patrz §7** |

### `"pvday"` (`NS_PV`, `Settings.cpp:15`) — dane, nie konfiguracja
| Klucz | Rozmiar | Co | Kontrola przy odczycie |
|---|---|---|---|
| `day` | Int | `tm_yday` profilu | `day >= 0` |
| `w` | 288 B | produkcja PV, 144 sloty | `getBytesLength == sizeof(watts)` |
| `l` | 288 B | pobór domu, 144 sloty | `getBytesLength == sizeof(load)` — **⚠ ten sam rozmiar co `w`** |
| `rh2` | 1736 B | historia BLE 24 h | `getBytesLength == sizeof(RoomHistory)` **+ wersja w nazwie klucza** |

**⚠ Nazwa przestrzeni kłamie:** `rh2` (historia czujników BLE) mieszka w `"pvday"`.
`pvHistoryClear()` (`Settings.cpp:280`) robi `prefs.clear()` na **całej** przestrzeni — czyli
skasowałby też historię pokoi. Funkcja jest **dziś nieużywana**; nie dodawaj przycisku
„wyczyść profil PV”, dopóki to nie jest naprawione.

### `"otaguard"` (`OtaGuard.cpp:18`) — okres próbny i liczniki restartów
| Klucz | Typ | Co |
|---|---|---|
| `rst` | UChar | powód poprzedniego resetu (`esp_reset_reason`) |
| `panics` | UShort | licznik panic/WDT/brownout — od zawsze |
| `trialver` | Int | wersja, która **właśnie weszła** w okres próbny |
| `badver`, `badcnt` | Int, UChar | wersja, która nie przeżyła, i ile razy |

Ta przestrzeń jest jako jedyna obsługiwana **lokalnymi** obiektami `Preferences` (`:62`, `:75`, `:233`)
— i dlatego jako jedyna jest odporna na wyścig opisany niżej.

### WERSJONOWANIE — przeczytaj, zanim ruszysz strukturę zapisywaną do NVS

**Kontrola rozmiarem NIE WYSTARCZA.** Dowód, świeży (v93):

> `RoomHistory` v1 = 4 pokoje × (temperatura + wilgotność). v2 = 6 pokoi × sama temperatura.
> Obie struktury mają **przypadkiem identyczny rozmiar 1736 B**. `getBytesLength("rh") == sizeof(RoomHistory)`
> przepuściłoby stary blob i pokazało **wilgotność jako temperaturę**. Nie crash — cicha korupcja.
> Uratował to **bump klucza `rh` → `rh2`** (`Settings.cpp:291-294`).

**Zasada:** przy każdej zmianie *układu* (nie tylko rozmiaru) struktury zapisywanej blobem
— **podnieś numer w nazwie klucza**. To kosztuje jedną literę i kasuje starą historię;
brak bumpu kosztuje dane, które wyglądają na prawdziwe.
Kandydaci, gdzie rozmiar jest **jedynym** zabezpieczeniem: `w`, `l` (dodatkowo **identyczne**
288 B — patrz audyt §4a) i `mets`.

### ⚠ Jeden `Preferences` na trzy przestrzenie i dwa wątki
`Settings.cpp:12` — **globalny** `Preferences prefs`, używany przez netTask (`roomHistorySave`,
`pvHistorySave`, `viSave` przy odświeżeniu tokena) **i** webTask (`settings().save()` z panelu,
`bleSet`). `prefs.begin()` na wspólnym uchwycie z dwóch wątków = zapis do złej przestrzeni albo
cicho zgubiony. Objaw: **„zapisałem hasło w panelu i po restarcie go nie ma”**.
Szczegóły i poprawka: `audyt/A-zrodla-prawdy.md` §3.

---

## 5. Bariery — trzy rzeczy, których nie wolno złamać

### 5a. Statyczny RAM < 76 000 B (teraz 71 600 B, zapas 4,4 kB)
```
cd /Users/maciuso/Desktop/Maciej.5000/esp32/pogoda-gdynia && \
TMPDIR=/tmp arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs,PSRAM=enabled" .
```
`TMPDIR=/tmp` jest **konieczne** dla ctags. Katalog szkicu **musi** nazywać się jak plik `.ino` —
nie buduj w worktree o innej nazwie.

Każda propozycja podnosząca RAM musi to **jawnie policzyć**. Przykład rachunku (odrzucony wariant
z audytu §2B): `RoomHistory::ROOMS 6→8` = `sizeof` 1736→2312 (+576 B) × **trzy** instancje statyczne
(`gRooms` `.ino:65`, `uiRooms` `.ino:71`, `kEmpty` `WeatherUi.cpp:2165`) = **+1728 B**, zapas
spada do 2672 B. Szukając instancji — **szukaj też `static const X kEmpty{}` w funkcjach rysujących**,
łatwo je przeoczyć.

Co **nie** liczy się do tej bariery: bufor ekranu (66 kB, od v50 w **PSRAM**), stosy zadań
(2 × 16 kB, SRAM, ale poza `.bss`), sterta. Sterta ma ~113 kB; `HEAP_DANGER=25000`
(`Config.h:105`) to próg, poniżej którego radar nie zdekoduje PNG, a TLS się dławi.

### 5b. Żadnych sekretów w repo
Repo jest **publiczne**, binarka OTA leży na GitHubie. Hasła WiFi/MQTT, IP falownika, bindkeye BLE
i token Viessmanna żyją **wyłącznie w NVS**, wprowadzane przez panel WWW.
**`/api/state` nigdy nie zwraca sekretów — tylko flagi „ustawione”.**

Sprawdzone i trzyma się (v93): `Portal.cpp:383-390` (`mq_pass_set` jako bool), `:719-728`
(`/api/diag` pomija user/hasło — bo bywa wklejane do zgłoszeń błędów), `:900-901`
(`/api/vi` zwraca `cid` i `auth`, **nigdy** `viRefresh`), `:780` (`/api/ble` zwraca `hasKey`,
nigdy klucza), `:1127`, `:1131` (konsola szeregowa też pilnuje).

Wzorzec dla pól hasłowych: **puste = bez zmian, `"-"` = skasuj** (`Portal.cpp:589-595`,
`Settings.cpp:207-218`). Powód historyczny: wcześniej puste pole kasowało zapisany klucz,
czyli **sama zmiana nazwy czujnika wywalała bindkey**.

Bramka Shelly **nie odszyfrowuje niczego** — przekazuje szyfrogram, klucze zostają w NVS
(`BleSensors.h:62-65`).

**Dziura do załatania:** `/api/vi/set` jest zarejestrowane bez `HTTP_POST` (`Portal.cpp:966`),
czyli nastawę pieca da się przestawić GET-em z dowolnej strony w sieci domowej. Panel nie ma
uwierzytelnienia (świadomie — LAN only), więc metoda jest jedyną barierą.

### 5c. OTA — urządzenie jest nie do odzyskania ręcznie
Nie ma USB. **Zła wersja = wyjmowanie z ściany.** Stąd cały `OtaGuard`.

- `verifyRollbackLater()` (`.ino:659`) — **słaby symbol**, przejmuje kontrolę nad potwierdzaniem
  obrazu. Arduino core (`initArduino()`) domyślnie sam potwierdza świeży obraz **zanim ruszy
  `setup()`**. Bez tego przeciążenia `OtaGuard` nigdy nie zobaczyłby `PENDING_VERIFY`
  i cały mechanizm byłby **atrapą** — `/api/diag` pisałby „stabilna”, a ochrony by nie było
  (tak zachowywała się v29). **Nie kasuj tej funkcji.**
- Okres próbny: 3 min (`TRIAL_TIMEOUT_MS`). Warunek potwierdzenia: **maksimum** sterty z okresu
  próbnego > 40 kB (`TRIAL_MIN_HEAP`) **i** (WiFi + realnie pobrane dane po TLS). Maksimum,
  nie chwilowy odczyt — normalna praca ma głębokie dołki.
- Dowodem sieci jest **dowolne** źródło TLS (pogoda/radar/loty), nie tylko pogoda — awaria samego
  Open-Meteo nie może cofać sprawnej wersji. **Falownika tam nie ma celowo**: Modbus to gołe TCP,
  o TLS nic nie dowodzi (`OtaGuard.cpp:186-190`).
- `otaGuardTick()` stoi **przed** wszystkimi `continue` w netTask (`.ino:207`) — musi tykać także
  bez WiFi, bo brak sieci to jeden z powodów, dla których wersję trzeba cofnąć.
- Twardy limit w `esp_timer` (`OtaGuard.cpp:274-283`) na wypadek, gdyby netTask zawisł.
- `otaGuardFatal()` (`.ino:692`) — gdy nie ma RAM na bufor ekranu, cofamy **natychmiast**,
  bez czekania 3 minut. To regresja typu **v14**.
- Wersja, która padła 2× (`MAX_BAD_TRIES`), nie pobierze się automatycznie — tylko ręcznie
  z panelu (`.ino:532-538`).
- Gdy nie ma na co wrócić: **zostajemy na gorszej wersji**, świadomie (`OtaGuard.cpp:160-163`).
  Pętla restartów byłaby gorsza niż praca w gorszej wersji.

---

## 6. Pułapki historyczne — każda kosztowała już raz

### Font i polskie znaki — **cztery** razy ta sama pułapka
Wbudowany GLCD (font 1) **nie ma** `ą ę ł ó ń ś ż ź ć` ani `°`. Wracało w kółko:
`"CIEP A WODA"`, `"52.4[]C"`, `"m-|"`. Od v81 `gl()`/`glCenter()`/`glRight()` idą przez
**PlFont10** z pełnym zestawem (`WeatherUi.cpp:116-132`) i GLCD zniknął z projektu.

**Zasada:** cokolwiek ma `°`, `ą`, `ł` — sprawdź, czy font to zna. `PlFont10` **nie ma** `…`
(U+2026), dlatego znacznik urwania to `".."`, a nie `"…"` (`WeatherUi.cpp:2487-2489`) — brakujący
glif `glyphIndex` zwraca −1 i znak **znika po cichu**, więc nie byłoby widać, że tekst urwano.
Ślad tej lekcji: `WeatherUi.cpp:1128-1139` (jednostka `°C` idzie PlFontem, `kW`/`kWh` GLCD-em,
bo węższe) i `:2361-2365` („CZWARTY raz ta sama pulapka”).

### `TFT_eSPI::drawSmoothArc` rozwala stertę przy przesuniętym viewporcie
`drawWedgeLine` **miesza układy współrzędnych**: bounding box przycina razem z datumem
(współrzędne fizyczne), ale pętle skanujące jadą po współrzędnych użytkownika i piszą przez
`setWindow()`/`pushColor()`. W dolnym pasie (datum −103) `setWindow` przepuszczał y=113, bo
`height()` zwracało 206, a bufor miał 103 wiersze — zapis leciał **~7 kB za koniec bufora**.
(Sprawdzone w TFT_eSPI 2.5.43.) Stąd własny `smoothArc()` (`WeatherUi.cpp:156`), złożony
z prymitywów wirtualnych.
**Dokładając prymitywy TFT_eSPI: sprawdź najpierw, czy nie piszą przez `setWindow()`/`pushColor()`.**
Bezpieczne (wirtualne, honorują viewport): `drawPixel`, `drawChar`, `drawLine`, `drawFastH/VLine`,
`fillRect`, `readPixel` — i wszystko, co jest na nich zbudowane.

### `fillSprite()` przy aktywnym viewporcie
Szybka ścieżka robi `memset(_img, ..., _iwidth * _yHeight * 2)` — ignoruje viewport.
**Nigdy `fillSprite()`**, zawsze `fillRect(0, 0, W, VIEW_H)` (`WeatherUi.cpp:195-197`).

### Transfer chunked — `getStream()` oddaje surowe nagłówki porcji
Viessmann `/features` (~53 kB) leci jako `Transfer-Encoding: chunked`; małe błędy mają `Content-Length`.
`http.getStream()` oddaje **surowy** strumień razem z `"1f4a\r\n{..."` — ArduinoJson słusznie
odrzucał to jako `InvalidInput`. `writeToStream()` rozpakowuje porcje; zbieramy całość do PSRAM
(`PsramSink`, `Viessmann.cpp:42-71`) i dopiero parsujemy. ArduinoJson ma tam własny alokator
PSRAM (`PsramAlloc`, `:29-34`) — w SRAM nie ma na to miejsca.

Pokrewne: RainViewer oddaje host **ze schematem** (`"https://tilecache..."`). Bez obcięcia
budowaliśmy `"http://https://..."` i żadna klatka nie dochodziła (`RadarMap.cpp:217-221`).

### HTTP 200 ≠ sukces u Viessmanna
API odsyła **200 nawet gdy komenda została odrzucona** — prawdziwy status siedzi w ciele
(`"statusCode": 400/502`). Sprawdzanie samego kodu HTTP dawało **fałszywy sukces**: „zapisano”,
a piec nie drgnął. PyViCare broni się tak samo (`__handle_command_error`).
Obsłużone w `setCircuitTemp` (`Viessmann.cpp:208-227`).
**⚠ Ścieżka odczytu (`:246`) tego NIE sprawdza** — patrz audyt §13.

Pokrewne, ta sama rodzina: liczniki miesięczne/roczne pieca są **zepsute**
(sprawdzone 15.07.2026: `currentMonth=5.3` < `lastSevenDays=5.8`, `currentYear=5.3` po 4 latach
instalacji). Ufamy **tylko `currentDay`** (`GasMeter.h:5-16`).

### Cudze API kłamie zerami — flaguj „mam dane”, nie „dane == 0”
Czujnik Xiaomi nadaje temperaturę i wilgotność w **osobnych ramkach**. Bez `hasTemp`/`hasHum`
rysowaliśmy `0.0 °C`, dopóki nie doszła ramka z temperaturą — **czyli kłamaliśmy**
(`BleSensors.h:42-45`). Ta sama lekcja: `rssiOwn`/`rssiGw` osobno, bo jedno `rssi` powodowało,
że bramka i własne radio nadpisywały się nawzajem i narzędzie do pomiaru zasięgu
**porównywało Shelly samo ze sobą** (`BleSensors.h:32-37`).

### ArduinoJson zapamiętuje **wskaźnik**, nie kopię, dla `const char*`
`o["mac"] = s.mac` gdzie `s` to kopia na stosie ginąca po iteracji → wszystkie wpisy pokazywały
ten sam, martwy adres (dwa czujniki, jeden MAC). Stąd `String(s.mac)` (`Portal.cpp:775-778`).

### Taktowanie klatek, nie stała pauza
`delay(33)` **po** rysowaniu daje okres „rysowanie + 33 ms”. Cel liczymy od **poprzedniego celu**,
nie od „teraz” — inaczej po każdym renderowaniu jesteśmy spóźnieni względem samych siebie,
resetujemy bazę i czekamy pełny okres (zmierzone: 110 ms / 9 fps zamiast równych 70)
(`.ino:879-902`).

### Jedna klatka = jeden moment
`nowMs` i `heapNow` łapiemy **raz, u wołającego**, i wieziemy przez stos — nie przez pole,
bo `render()` i zrzut ekranu jadą na **różnych rdzeniach**. Gdyby każdy pas/pasek czytał
`millis()`/`getFreeHeap()` od nowa, napis przecięty granicą pokazałby w górnej połowie inną
wartość niż w dolnej i litery rwałyby się w pół. W zrzucie widać to najmocniej — między paskami
leci transmisja BMP, więc mijają setki ms (`WeatherUi.h:126-133`, `WeatherUi.cpp:2704-2709`).

### Skala PV była zapadką
`pvScaleW_` rosło, gdy produkcja przekroczyła moc deklarowaną, i **nigdy nie wracało** — nawet po
zmianie ustawienia. Użytkownik wpisywał 6,0 kWp, ekran uparcie pokazywał „z 7.0 kWp”
(6667 × 1,05 = 7000) i nie dało się tego zrozumieć. Teraz skala = **dokładnie** `settings().pvPeakW`
(`WeatherUi.cpp:1001-1006`). Procentu **nie** przycinamy do 100 — produkcja powyżej nominału
zdarza się realnie (zimne, słoneczne dni) i jest informacją, nie błędem.

### Milczenie ≠ awaria
Huawei **wyłącza Modbus TCP po zachodzie**. Nocne milczenie falownika to stan **neutralny**
(szary), nie czerwony (`PvData.h:22-26`, `WeatherData.h:90-117`). Okno „wolno mu milczeć” jest
przesunięte o 30 min w **obie** strony: zaczyna się 30 min po zachodzie (potrafi odpowiadać
jeszcze po zmroku), kończy 30 min po wschodzie (rozgrzewka Modbusa ~100 s).
Bez godzin wschodu/zachodu zwracamy `false` — wolimy uczciwy błąd niż zamiecenie awarii pod dywan.
`pv.asleep` **musi** być w porównaniu odświeżającym stopkę (`WeatherUi.cpp:418-419`), inaczej przy
zasypianiu wszystkie liczby zostają zerami i czerwone „nie odpowiada” wisi całą noc.

### PSRAM: „psram: 0” nie było dowodem braku pamięci
Do v49 kompilowaliśmy z `PSRAM=disabled`, więc `psramInit()` nigdy się nie wykonywał i diagnostyka
pokazywała `psram: 0`. To był dowód, że **o nią nie pytaliśmy** (`.ino:668-677`).
Konsekwencja: dwa pasy renderowania (66 kB zamiast 132 kB) istniały tylko dlatego, że nie
wiedzieliśmy o 2 MB PSRAM. Od v50 `BAND_N = 1` — rysujemy raz zamiast dwa (`WeatherUi.h:111-115`).
**Kod nadal jest napisany tak, jakby pasów było wiele** (`setBand`, `pushBands`, globalne
współrzędne, `checkViewport`) — to jest zapas, nie martwy kod, ale wiedz, że dziś pas jest jeden.

---

## 7. Rzeczy, o których musisz wiedzieć, zanim uwierzysz komentarzom

- **Weryfikacja licznikiem gazu NIE ISTNIEJE.** `GasMeter.h:5-16` opisuje ją szczegółowo, z datami
  pomiarów. Kodu nie ma: `gGas` (`.ino:70`) jest **zapisywany co 3 min i nigdy nie czytany**,
  nie trafia do NVS (ginie przy restarcie), a `meterAdd`/`meterDel`/`sumBetween`/`MeterRead`
  **nie mają ani jednego wywołującego** — nie ma trasy `/api/meter*` w `Portal.cpp:943-971`.
  Koszt: 312 B statycznego RAM przy zapasie 4,4 kB. Patrz audyt §8.
- **`.ino:5-10` mówi o „5 widokach”.** Jest ich **9** (`Config.h:85`). Komentarz nie był ruszany
  od czterech ekranów.
- **`cfg::FOOTER_Y = 208` i `cfg::FOOTER_H = 32` są nieużywane i nieprawdziwe.** Stopka realnie
  zaczyna się na **206** i ma **34** px (`WeatherUi.cpp:436,441`). Patrz audyt §9.
- **`Settings::BLE_SLOTS = 8` nic nie definiuje** — tablica ma zaszyte `ble[8]` (`Settings.h:41`),
  a stała stoi 27 linii niżej (`:68`). Zmiana stałej **nie zmieni tablicy**, za to odczyty
  wyjadą poza nią.
- **Panel mówi „maks. 4 czujniki”** (`Portal.cpp:822`) — nieprawda, slotów jest 8, a realnie
  widocznych 6. Patrz audyt §2.
- **MQTT publikuje tylko 4 czujniki** (`MqttClient.cpp:217,493,502`), mimo że ekran pokazuje 6.

---

## 8. Ściągawka — „szukam X”

| Szukam… | Idź do |
|---|---|
| adresu OTA, pinów, czasów, progów | `Config.h` |
| hasła / IP / tokena | **NVS** (`Settings.cpp`), nigdy w kodzie |
| co jest sekretem, a co nie | §4 wyżej + `Settings.h:32-52` |
| dlaczego ekran wygląda tak, a nie inaczej | komentarze w `WeatherUi.cpp` — są gęste i **prawdziwe** (poza §7) |
| dlaczego OTA nie potwierdza wersji | `OtaGuard.cpp` + `verifyRollbackLater()` w `.ino:659` |
| kto pisze do `gWeather` | `netTask`, `.ino:238-244`, pod `gLock` |
| skąd się bierze liczba 206 | `WeatherUi.h:110` (`VIEW_H`) — **jedyne żywe źródło** |
| ile jest czujników BLE | pytanie źle postawione — patrz audyt §2 |
| listy widoków | `Config.h:85-92` **i** `WeatherUi.cpp:1263` **i** `Portal.cpp:200` (trzy źródła — audyt §5) |
| znanych problemów | `audyt/A-zrodla-prawdy.md`, `BACKLOG.md` |
</content>
</invoke>
