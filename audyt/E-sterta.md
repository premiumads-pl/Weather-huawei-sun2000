# E — Sterta: skad 22044 i dlaczego nasz licznik tego nie widzi

Sledztwo w kodzie v93. Dane wejsciowe z zywego urzadzenia (uptime 7155 s, `was_crash: false`).
Wszystkie odwolania w formacie `plik:linia` wskazuja stan repo w chwili audytu.

---

## 0. Streszczenie

| Pytanie | Odpowiedz |
|---|---|
| Czym jest `heap_min_ever`? | `ESP.getMinFreeHeap()` — **prawdziwy** znacznik minimum, prowadzony przez alokator przy KAZDYM `malloc`/`free` od startu. Nie da sie go oszukac. |
| Czym jest `heap_min`? | `diag().minHeap` — probka pobierana **wylacznie wewnatrz `logPrintf()`**. Nie w `loop()`. |
| Skad rozjazd 63 kB? | `heap_min` probkuje sterte tylko w momentach, gdy cos logujemy — a logujemy **po** zakonczeniu ciezkiej operacji, gdy pamiec jest juz oddana. Licznik z definicji mierzy spoczynek. |
| Czy to grozne? | **Nie bezposrednio** (dolek minal, urzadzenie chodzi). Grozniejsza jest **fragmentacja**: `heap_largest_block = 38900` przy `heap_free = 94548`. |
| Czy `heap_min` klamie na ekranie? | **Tak.** Komentarz `WeatherUi.cpp:2624` obiecuje "najgorszy moment od startu" i tego nie dowozi. |

---

## 1. Odpowiedz na zagadke

### 1.1. `heap_min_ever` — znacznik prowadzony przez alokator

`Portal.cpp:700`:

```cpp
j["heap_min_ever"] = ESP.getMinFreeHeap();
```

`ESP.getMinFreeHeap()` to w rdzeniu Arduino cienka nakladka na `esp_get_minimum_free_heap_size()`.
Ta wartosc **nie jest probkowana przez nasz kod w ogole** — utrzymuje ja komponent `heap` w ESP-IDF,
aktualizujac znacznik `minimum_free_bytes` przy kazdej operacji na stercie. Liczy **od startu
ukladu** (od `app_main`, czyli obejmuje takze caly `setup()`) i jest monotonicznie nierosnaca.

Dwie konsekwencje, obie wazne:

1. **Nie da sie przed nia ukryc zadnego dolka.** Jesli jakis `malloc` na 1 mikrosekunde zjechal
   sterte do 22044 B, ta liczba to zapamieta. Nie ma znaczenia, czy ktokolwiek wtedy patrzyl.
2. **Dotyczy wylacznie wewnetrznego SRAM-u.** PSRAM jest w tym projekcie alokowany jawnie przez
   `ps_malloc` (osobne `caps`), wiec nie wchodzi do puli `MALLOC_CAP_DEFAULT`. Dowod nie wprost:
   gdyby PSRAM byl w domyslnej puli, `heap_free` pokazywaloby ~2 MB, a pokazuje 94548 B.
   Czyli **22044 to prawdziwe minimum wewnetrznego SRAM-u**, nie artefakt pomiaru.

### 1.2. `heap_min` — probka przy okazji logowania

`Portal.cpp:699`:

```cpp
j["heap_min"] = d.minHeap == 0xFFFFFFFF ? ESP.getFreeHeap() : d.minHeap;
```

`d.minHeap` to `Diag::minHeap` (`Log.h:36`, wartownik `0xFFFFFFFF`). **Jedyne miejsce w calym
repo, ktore go zapisuje**, to koncowka `logPrintf()` — `Log.cpp:52-55`:

```cpp
  const uint32_t h = ESP.getFreeHeap();
  if (h < gDiag.minHeap) {
    gDiag.minHeap = h;
  }
}
```

To jest sedno sprawy i tu trzeba **skorygowac zalozenie z brefu**.

> **`minHeap` NIE jest probkowany w `loop()`.**

`loop()` faktycznie czyta sterte raz na klatke — `WeatherUi.cpp:1422`:

```cpp
  const uint32_t heapNow = ESP.getFreeHeap();
```

...ale ta wartosc idzie **wylacznie do narysowania** (`paintFrame(..., heapNow)`, `WeatherUi.cpp:1431`).
Nigdy nie trafia do `diag().minHeap`. Czyli:

- `minHeap` nie jest probkowany co klatke (30 fps),
- `minHeap` jest probkowany **co linijke logu**, z dowolnego watku, ktory akurat loguje,
- `loop()` (rdzen 1, renderer) praktycznie **nie loguje wcale** — wiec `minHeap` to w 99 %
  probki z `netTask`.

### 1.3. Dlaczego probki systematycznie trafiaja w spoczynek (a nie w dolek)

To nie jest pech. To jest **konstrukcja**. Miejsca wywolan `LOG(...)` z heapem leza *za*
operacja, ktora pamiec brala i juz ja oddala.

Sztandarowy dowod — `pogoda-gdynia.ino:249-262`:

```cpp
      WeatherModel tmp{};
      if (weatherClient.fetch(tmp)) {          // <-- TU zyje TLS (~40 kB)
        ...
        LOG("Pogoda OK: %.1f C, kod %d, heap %lu\n", tmp.current.tempC,
            tmp.current.weatherCode, (unsigned long)ESP.getFreeHeap());   // <-- TU juz nie zyje
```

`WiFiClientSecure client;` jest lokalna w `WeatherClient::fetch()` (`WeatherClient.cpp:182`).
Zanim `fetch()` zwroci sterowanie, jej destruktor **juz zwolnil bufory mbedTLS**. `LOG` na
linii 261 mierzy sterte, z ktorej TLS wyszedl kilkanascie mikrosekund wczesniej.

Ten sam wzorzec wszedzie:

| Miejsce | Ciezka operacja | `LOG` z heapem |
|---|---|---|
| `pogoda-gdynia.ino:249-262` | `weatherClient.fetch()` (TLS) | **po** |
| `pogoda-gdynia.ino:150-170` | `WiFi.begin()` + skan wszystkich kanalow | **po** (`:169`) |
| `RadarClient.cpp:226,344` | pobranie + dekoder PNG | **po** (`:344`) |
| `BleSensors.cpp:331-342` | `BLEDevice::init()` | **po** (`:341`) |

**Wniosek: `heap_min = 85464` nie jest minimum sterty. To jest podloga stanu spoczynkowego —
najnizsza wartosc, jaka sterta miala w chwili, gdy akurat bylo spokojnie i cos pisalismy do logu.**
Rozjazd 63 kB to dokladnie **laczny rozmiar tego, co rodzi sie i umiera pomiedzy dwiema linijkami
logu**.

I to nie jest odkrycie — autor **juz to wiedzial**. `OtaGuard.cpp:177-180`:

```cpp
  // 1) Sterta. Bierzemy MAKSIMUM z okresu probnego, a nie chwilowy odczyt —
  //    normalna praca ma glebokie dolki (TLS, radar, zrzut ekranu), wiec jedna
  //    probka klamie. Zdrowa wersja w ciagu 3 minut na pewno kiedys ma > 40 kB;
```

Czyli: w `OtaGuard` swiadomie bierzemy **maksimum**, bo "jedna probka klamie" i "normalna praca
ma glebokie dolki". Ta sama wiedza nie zostala zastosowana do `diag().minHeap` i do ekranu
statystyk. **Dolki sa znane, oczekiwane i nazwane w komentarzu — brakuje tylko licznika, ktory
by je widzial.**

### 1.4. Dodatkowo: najwazniejsza diagnostyka heapu jest niewidoczna

Urzadzenie ma **slepy Serial** (wisi w lazience, bez USB). Tymczasem te miejsca pisza heap przez
`Serial.printf`, a nie przez `LOG`, wiec **nie trafiaja ani do `/api/log`, ani do `minHeap`**:

- `Ota.cpp:67-69` — heap przy nieudanym sprawdzeniu wersji
- `Ota.cpp:98-99` — **heap przed pobieraniem firmware'u** (najwazniejsza liczba w calym OTA)
- `Ota.cpp:133-134` — heap przy starcie zapisu
- `WeatherUi.cpp:1790-1791` — heap po zwolnieniu bufora ekranu
- `WeatherUi.cpp:1808-1809` — heap po odtworzeniu bufora

To sa dokladnie te punkty, ktore rozstrzygnelyby zagadke — i wszystkie leca w prozne.

### 1.5. Drobiazg: dwa rozne fallbacki dla tego samego pola

`Portal.cpp:699` przy wartowniku zwraca `ESP.getFreeHeap()`, a `Portal.cpp:747` dla tej samej
danej zwraca `0`:

```cpp
  j["heap_min"]   = d.minHeap == 0xFFFFFFFF ? ESP.getFreeHeap() : d.minHeap;   // :699
  mem["sram_min"] = d.minHeap == 0xFFFFFFFF ? 0 : d.minHeap;                   // :747
```

Nieszkodliwe (wartownik znika po pierwszym logu), ale to dwa zrodla prawdy o jednej liczbie.

---

## 2. KIEDY sterta schodzi do ~22 kB

### 2.1. Skala zjawiska

```
heap_free (spoczynek)   94548
heap_min_ever           22044
-----------------------------
transient               72504 B  (~72,5 kB PONAD stan spoczynkowy)
```

To jest duzo. Pojedynczy handshake TLS (mbedTLS: `in_buf` 16 kB + `out_buf` 4 kB + struktury
handshake'u + parsowanie lancucha certyfikatow) to realnie **~35-45 kB**. Sam TLS **nie tlumaczy
72,5 kB**. Potrzebne jest albo zdarzenie grubsze, albo nalozenie sie dwoch.

### 2.2. Co udalo sie WYKLUCZYC z kodu

**Radar i mapa opadow — NIE.** Cala sciezka PNG idzie do PSRAM:

- `RadarClient.cpp:123-125` — `psram_ ? ps_realloc/ps_malloc : realloc/malloc`, a `psram_` to
  `psramFound()` (`RadarClient.cpp:110`)
- `RadarClient.cpp:301` — `void* mem = psramFound() ? ps_malloc(sizeof(PNG)) : nullptr;`
- `RadarMap.cpp:144-145`, `RadarMap.cpp:268`, `RadarMap.cpp:358` — `ps_malloc` / `ps_realloc`
- Kafelki ciagniemy **czystym HTTP, bez TLS** (`RadarMap.cpp:167-175` uzywa `WiFiClient`,
  nie `WiFiClientSecure`; komentarz `RadarClient.cpp:259-260`: "Kafelek bierzemy po czystym
  HTTP: bez TLS starcza RAM-u na dekoder")

Do tego radar ma **wlasna bramke** (`RadarClient.cpp:25,225-231`):

```cpp
constexpr uint32_t kMinHeapForRadar = 64000;
...
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < kMinHeapForRadar) {
    ++diag().radarSkips;
```

**I TAK — `radarSkips` JEST widoczne w `/api/diag`.** `Portal.cpp:737`:

```cpp
  r["skips_low_ram"] = d.radarSkips;
```

To jest **pomiar, ktory juz masz na urzadzeniu** — patrz § 3.1.

**OTA (pobieranie firmware'u) — NIE w tym uptime.** `Ota::downloadAndFlash()` przy sukcesie
konczy sie `ESP.restart()` (`Ota.cpp:303`), a przy porazce tez (`Ota.cpp:290-291`). Skoro
uptime = 7155 s i `was_crash: false`, to zadne **pobieranie** nie zdarzylo sie w tym starcie.
W grze zostaje wylacznie `fetchRemoteVersion()` (`Ota.cpp:49-88`) — zwykly TLS, co 15 min
(`cfg::OTA_CHECK_MS`).

**MQTT — NIE.** `MqttClient.cpp:40,292` uzywa golego `WiFiClient` (bez TLS), a bufor
PubSubClient to 512 B (`MqttClient.cpp:302`).

**Viessmann — TLS tak, ale JSON nie.** `WiFiClientSecure` (`Viessmann.cpp:149,253,302`), ale
odpowiedz `/features` (~53 kB) idzie w calosci do PSRAM: `PsramSink` (`Viessmann.cpp:72-101`)
i `PsramAlloc` dla ArduinoJson (`Viessmann.cpp:59-64`). Czyli Viessmann kosztuje w SRAM tyle,
co zwykly handshake, nie wiecej.

### 2.3. Co ZOSTAJE — kandydaci, uszeregowani

> Ponizsze to **hipotezy z uzasadnieniem z kodu, nie ustalenia**. Rozstrzyga je pomiar z § 3.

**A. Okno `connectWifi()` — jedyne okno bez zadnego logu, o wlasciwej skali. (Najmocniejszy kandydat.)**

Sekwencja z konca `setup()` i poczatku `netTask`:

1. `pogoda-gdynia.ino:796-797` — `xTaskCreatePinnedToCore(webTask, "web", 16384, ...)` i
   `(netTask, "net", 16384, ...)`. To **32 kB ze sterty SRAM** (stosow FreeRTOS nie da sie
   trzymac w PSRAM — `pogoda-gdynia.ino:83-84`). **Za tym nie ma zadnego `LOG`.**
2. `netTask` wchodzi w `connectWifi()` (`pogoda-gdynia.ino:132`).
3. `pogoda-gdynia.ino:147-148`:
   ```cpp
   WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
   WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
   ```
   To wymusza **skan wszystkich kanalow** przy kazdym laczeniu. Wyniki skanu (rekordy wszystkich
   widzianych AP) plus bufory sterownika WiFi zyja na stercie do konca laczenia. W bloku
   mieszkalnym to kilkadziesiat AP.
4. `pogoda-gdynia.ino:169-170` — **pierwszy `LOG` z heapem pojawia sie dopiero PO udanym
   polaczeniu**, gdy bufory skanu juz oddane.

Okno miedzy logiem z `BleSensors.cpp:341` a logiem z `pogoda-gdynia.ino:169` zawiera:
**32 kB stosow + init sterownika WiFi + skan wszystkich kanalow + handshake polaczenia** —
i nie ma w nim ani jednej probki `minHeap`. Skala pasuje do 72,5 kB.

Zdarza sie: **raz na kazde polaczenie WiFi** (`wifiConnects` w `/api/diag` mowi, ile razy).

**B. Roaming — `WiFi.scanNetworks()` (to samo, cyklicznie).**

`pogoda-gdynia.ino:490-509`:

```cpp
        const bool scanned = portal::scanLock(20000);
        if (scanned) {
          const int found = WiFi.scanNetworks(false, false, false, 250);
          ...
          WiFi.scanDelete();
          portal::scanUnlock();
        }
```

Miedzy `scanNetworks()` a `scanDelete()` **nie ma zadnego `LOG`** — pierwszy pojawia sie na
`:512`/`:514`/`:523`, czyli juz po `scanDelete()`. Wyniki skanu wszystkich AP zyja w tym oknie.
Do tego przy przenosinach idzie `WiFi.disconnect()` + `WiFi.begin()` (`:515-516`) — czyli
ponownie sciezka A.

Zdarza sie: przeglad co 180 s (`:477`), ale tresc wykonuje sie **tylko gdy RSSI < -68**
(`:479`). `wifiRoams` w `/api/diag` mowi, ile razy doszlo do przenosin.

**C. Handshake TLS — najczestszy, ale za maly sam z siebie.**

Najczestszy TLS to **loty: co 15 s** (`cfg::FLIGHT_REFRESH_MS = 15000`, `Config.h:74`;
`WiFiClientSecure` w `FlightClient.cpp:68`), o ile `gFlightsNeeded`. Dalej pogoda (15 min),
Viessmann (3 min), OTA-check (15 min). Kazdy taki handshake to ~35-45 kB **na kilkaset ms** —
i ani jednej probki `minHeap` w srodku.

Sam nie da 72,5 kB. Ale **nalozony** na cokolwiek innego — owszem.

**D. Nalozenie dwoch TLS naraz (netTask + webTask).**

`webTask` ma wlasny TLS: geokoder w panelu (`Portal.cpp:538-539`). Jesli ktos szuka miasta
w panelu dokladnie wtedy, gdy `netTask` robi handshake — dwa razy ~40 kB. To rzadkie
(wymaga czlowieka przy panelu), ale mozliwe.

**E. Wyniki skanu BLE, akumulowane w trakcie nasluchu.**

`BleSensors.cpp:349-357`:

```cpp
void scan(int seconds) {
  ...
  BLEScan* sc = BLEDevice::getScan();
  sc->start(seconds, false);
  sc->clearResults();  // inaczej wyniki zostaja na stercie
  gScanning = false;
}
```

Przez cale `sc->start(4, ...)` wyniki (obiekty `BLEAdvertisedDevice`, kazdy ze `String`-ami)
narastaja na stercie i sa zwalniane dopiero przez `clearResults()` **po** skanie. W bloku
mieszkalnym to kilkadziesiat urzadzen. Zadnego `LOG` w srodku. Dzieje sie **co 20 s**
(`pogoda-gdynia.ino:466`).

Uwaga: `ble::scan()` blokuje `netTask`, wiec **nie moze** nalozyc sie na TLS z `netTask`.
Moze za to nalozyc sie na TLS z `webTask`.

**F. Zrzut ekranu — pod warunkiem, ze sprite ladi w SRAM (patrz § 3.2).**

`WeatherUi.cpp:2733-2734` tworzy dodatkowy sprite. Autor **juz raz to zlapal** —
`pogoda-gdynia.ino:770-771`:

```cpp
    // Nie w trakcie nasluchu BLE: zrzut alokuje sprite'y, a stos Bluetooth trzyma
    // wtedy 72 kB. Zbieg tych dwoch rzeczy zjechal sterta do 2 kB.
```

Zabezpieczenie istnieje (`pogoda-gdynia.ino:772-773` czeka na koniec skanu BLE), ale
liczba "72 kB" w tym komentarzu jest podejrzanie bliska naszym 72,5 kB. Patrz nizej.

### 2.4. Trop, ktorego nie domykam: komentarze o BLE sa wzajemnie sprzeczne

`BleSensors.cpp:323-326` (opis `begin()`, stan aktualny):

```cpp
// Od v51 stos BLE zostaje w pamieci NA STALE. Wczesniej podnosilismy go i oddawali
// przy kazdym nasluchu, bo 72 kB nie miescilo sie obok reszty — kosztowalo to
// fragmentacje sterty, 6 s niedostepnego panelu i sterte spadajaca do 2 kB.
```

`BleSensors.cpp:345-348` (opis `scan()`, **12 linijek nizej**):

```cpp
// Stos BLE kosztuje ~72 kB sterty. Trzymany na stale zostawial 37 kB wolnego —
// za malo na TLS (pogoda, samoloty, OTA) i OtaGuard slusznie cofnal taka wersje
// (v38). Skoro sluchamy 6 s na 45 s, stos podnosimy TYLKO na czas nasluchu
// i zaraz go oddajemy. Miedzy skanami sterta jest nietknieta.
```

**Te dwa komentarze opisuja dwa przeciwne projekty.** Kod realizuje pierwszy: `scan()`
(`:349-357`) **nie robi zadnego `BLEDevice::deinit()`** — tylko `start()` i `clearResults()`.
Drugi komentarz to zywcem opis architektury sprzed v51 i dzis **klamie**.

Przy okazji, w tym samym pliku:

- `BleSensors.cpp:26` — `constexpr uint32_t kMinHeapForBle = 100000;` z komentarzem
  "Nie podnosimy go, jesli sterta jest juz napieta". **Ta stala nie jest uzyta nigdzie
  w repo** (sprawdzone: jedyne wystapienie to sama definicja). Bramka opisana w komentarzu
  **nie istnieje**.
- Komentarz `:347` mowi "6 s na 45 s". Kod: `ble::scan(4)` (`pogoda-gdynia.ino:435`) co 20 s
  (`pogoda-gdynia.ino:466`). Komentarz `pogoda-gdynia.ino:432-433` tez mowi "6 s nasluchu co
  45 s". **Trzy miejsca, dwie rozne nieprawdy.**

To nie jest czepianie sie stylu. **Nie da sie ustalic z kodu, ile realnie kosztuje dzis stos
BLE**, bo jedyne zrodlo tej liczby (72 kB) to komentarz opisujacy nieistniejaca juz
architekture na starym rdzeniu (Bluedroid). Na rdzeniu 3.x BLE stoi na NimBLE
(`BleSensors.cpp:293`: "Core 3.x opiera BLE na NimBLE"), ktory jest istotnie chudszy.
**Pomiar w § 3.3 to rozstrzyga.**

---

## 3. Czy to grozne

### Werdykt: **nie bezposrednio — ale nie z powodu, ktory podaje `heap_min`, i nie jest to udowodnione.**

Uzasadnienie po kolei.

### 3.1. Dolek 22044 juz sie nie powtarza w takiej skali — i mamy na to dowod z urzadzenia

Radar chodzi **co 5 min** (`cfg::RADAR_REFRESH_MS`, `Config.h:73`), czyli w ciagu 7155 s
odpalil sie **~23 razy**. Przy kazdym uruchomieniu sprawdza sterte i **inkrementuje licznik**,
jesli jest ponizej 64 kB (`RadarClient.cpp:225-231`). Licznik jest wystawiony jako
`radar.skips_low_ram` (`Portal.cpp:737`).

> **To jest gotowy, dzialajacy detektor niskiej sterty z progiem 64 kB — prawie trzy razy
> wyzszym niz `HEAP_DANGER`. Odpytaj `/api/diag` i przeczytaj `radar.skips_low_ram`.**

- `skips_low_ram == 0` -> przy **wszystkich 23 podejsciach** sterta miala >= 64 kB.
  Czyli dolki nie sa stanem trwalym; sa krotkie i rzadkie. To mocno wspiera hipoteze **A**
  (jednorazowe okno przy starcie / przy laczeniu WiFi).
- `skips_low_ram > 0` -> sterta bywa ponizej 64 kB **regularnie**, w losowych momentach.
  Wtedy 22044 to nie ciekawostka z rozruchu, tylko **rozklad**, ktorego ogon kiedys trafi
  w handshake TLS. To zmienia werdykt na "grozne".

### 3.2. Prawdziwym ryzykiem jest FRAGMENTACJA, nie liczba wolnych bajtow

```
heap_free           94548
heap_largest_block  38900     <- 41 % wolnej sterty
```

94,5 kB wolnego, ale **najwiekszy spojny kawalek to 38,9 kB**. mbedTLS potrzebuje ciaglego
bufora `in_buf` (~16 kB) w jednym kawalku. Dzis wchodzi z zapasem, ale to **ta liczba jest
wiazaca**, nie `heap_free`.

Widac to juz w zachowaniu urzadzenia. `pogoda-gdynia.ino:336`:

```cpp
      const bool needMem = ESP.getMaxAllocHeap() < 48000;
```

**38900 < 48000 -> `needMem` jest dzis PRAWDA przy kazdym cyklu radaru** (co 5 min). Czyli
co 5 minut `netTask` podnosi `gRadarWantMem`, a `loop()` zamraza ekran na maks. 3 s
(`pogoda-gdynia.ino:338-341`, `:811-818`).

I tu jest pytanie warte sprawdzenia: **czy to cokolwiek daje?** Jesli sprite (66 kB) siedzi
w PSRAM — a tak twierdza komentarze od v50 (`ARCHITEKTURA.md:231`: "bufor ekranu (66 kB, od v50
w **PSRAM**)") — to `ui.releaseBuffer()` **zwalnia PSRAM i nie zmienia `getMaxAllocHeap()` dla
SRAM ani o bajt**. Wtedy zamrozenie ekranu na 3 s co 5 minut jest **czysta strata**: warunek
`needMem` bedzie prawdziwy zawsze, bo zwolnienie bufora nie moze go odwrocic.

Audyt B doszedl do tego samego (`audyt/B-wspolbieznosc-pamiec.md:669`: "Uwaga na marginesie:
`gRadarWantMem` zamraza ekran i (po wlaczeniu PSRAM) nic nie daje").

Zauwaz tez, ze komentarz `pogoda-gdynia.ino:77-80` opisuje swiat sprzed PSRAM:

```cpp
// Radar potrzebuje 47 kB w JEDNYM kawalku. Sterty jest 113 kB, ale bufor ekranu
// (66 kB) siedzi w srodku i dzieli ja tak, ze najwiekszy spojny blok ma 43 kB.
```

"najwiekszy spojny blok ma 43 kB" vs. dzisiejsze 38900 — podejrzanie blisko. **Jesli sprite
naprawde wyprowadzil sie do PSRAM, to co dzieli sterte dzisiaj?** To pytanie rozstrzyga jedno
pole, ktore **juz masz w `/api/diag`**: `mem.psram_free` (`Portal.cpp:750`).

> **Porownaj `mem.psram_free` z `psram` (2097152):**
> - roznica rzedu ~66 kB + kafelki radaru -> sprite **jest** w PSRAM, komentarze aktualne,
>   a `gRadarWantMem` to martwy rytual do usuniecia (i wtedy fragmentacje SRAM robi cos innego);
> - roznica rzedu tylko kafelkow radaru (PSRAM prawie nietkniete) -> **sprite siedzi w SRAM**,
>   komentarze od v50 klamia, `ARCHITEKTURA.md:231` klamie, a 66 kB w srodku sterty
>   tlumaczy i fragmentacje, i znaczna czesc dolka 22 kB.

Drugi wariant jest realny: `TFT_eSPI::callocSprite()` wybiera PSRAM pod warunkiem
`#if defined(ESP32) && defined(CONFIG_SPIRAM_SUPPORT)`. W rdzeniu ESP32 3.x makro nazywa sie
`CONFIG_SPIRAM`, nie `CONFIG_SPIRAM_SUPPORT`. Nie zweryfikowalem tego — biblioteki TFT_eSPI
nie ma w podpietych katalogach. **Nie zgaduje; `psram_free` odpowiada na to w 5 sekund.**

### 3.3. Czego dolek 22044 dowodzi, a czego nie

**Dowodzi:** urzadzenie **przezylo** zejscie do 22 kB. `was_crash: false`, uptime 7155 s.
Zaden `malloc` w tym momencie nie dostal `NULL` w miejscu, ktore by go nie sprawdzilo.

**Nie dowodzi:** ze przezyje nastepnym razem. 22044 to **znacznik**, nie **rozklad**. Wiemy,
ze raz tam bylo. Nie wiemy, czy raz, czy 400 razy — bo `getMinFreeHeap()` nie liczy zdarzen.

Prawdziwe zagrozenie nie jest "restart z braku RAM". Jest gorsze i ma w tym repo nazwe —
**scenariusz v14** (`Ota.cpp:226-228`):

```cpp
    // SIEC BEZPIECZENSTWA: jesli sterty jest tak malo, ze nie da sie zestawic TLS,
    // urzadzenie nie mogloby sie juz NIGDY zaktualizowac po sieci (dokladnie to
    // zabilo v14). Oddajemy wiec 150 kB bufora ekranu i probujemy jeszcze raz.
```

Czyli: sterta nie zabija urzadzenia — **zabija OTA**. A OTA to jedyny sposob, w jaki czlowiek
moze cokolwiek naprawic w skrzynce wiszacej w lazience. Siec bezpieczenstwa istnieje
(`Ota.cpp:229-244`), ale konczy sie `ESP.restart()` (`:242`) — czyli "klopotem czlowieka".
I opiera sie na zwolnieniu bufora ekranu, ktore — jesli sprite jest w PSRAM — **nie oddaje
ani bajtu SRAM-u**. Wtedy ta siec bezpieczenstwa jest atrapa i zostaje z niej sam restart.

To jest realne ryzyko do domkniecia, i ono zalezy od tego samego pytania co § 3.2.

### 3.4. Podsumowanie werdyktu

| | |
|---|---|
| Czy 22 kB grozi restartem **dzis**? | Nie. Dolek minal, `was_crash: false`, 2 h uptime. |
| Czy to byl jednorazowy dolek przy starcie? | **Prawdopodobnie tak** (hipoteza A), ale **nieudowodnione**. Rozstrzyga `radar.skips_low_ram` i pomiar z § 4. |
| Czy jest cos grozniejszego? | **Tak: fragmentacja.** `largest_block = 38900` przy `free = 94548`. To ta liczba dlawi TLS, nie `heap_free`. |
| Najgorszy realny scenariusz | TLS przestaje wchodzic -> OTA niemozliwe -> urzadzenie nie do naprawienia zdalnie (v14). |

---

## 4. Co zmierzyc na urzadzeniu

### 4.1. NATYCHMIAST, bez wgrywania czegokolwiek (`GET /api/diag`)

| Pole | Zrodlo | Co rozstrzyga |
|---|---|---|
| `radar.skips_low_ram` | `Portal.cpp:737` | **Najwazniejsze.** > 0 = sterta bywa < 64 kB regularnie (grozne). == 0 = dolki rzadkie/startowe. |
| `mem.psram_free` vs `psram` | `Portal.cpp:750,702` | Czy sprite 66 kB jest w PSRAM czy w SRAM (§ 3.2). Przesadza o sensie `gRadarWantMem` i o sieci bezpieczenstwa OTA. |
| `wifi.connects` | `Portal.cpp:711` | Ile razy przeszla sciezka A (`connectWifi`). Jesli == 1, a dolek istnieje -> dolek jest ze startu. |
| `wifi.roams` | `Portal.cpp:714` | Ile razy przeszla sciezka B (skan + reconnect). |
| `mem.sram_block` | `Portal.cpp:748` | Fragmentacja (= `heap_largest_block`). |

**Zestawienie `wifi.connects == 1` + `wifi.roams == 0` + `skips_low_ram == 0` = mocny dowod,
ze 22044 pochodzi z rozruchu i sie nie powtarza.** Wtedy temat zamkniety bez wgrywania.

### 4.2. INSTRUMENTACJA #1 — bracketowanie `getMinFreeHeap()` (do wdrozenia; tanie, zerowe ryzyko)

**Pomysl:** `ESP.getMinFreeHeap()` jest **monotonicznie nierosnaca**. Nie trzeba niczego
probkowac — wystarczy **drukowac ja na granicach faz**. Faza, w ktorej wartosc spadla, jest
zidentyfikowana **dokladnie** przez pierwsza granice, na ktorej jest juz niska. Zero nowych
watkow, zero nowych alokacji, zero ryzyka.

**Zmiana 1 — do istniejacych logow z heapem dopisz `minEver`.**

Wszedzie tam, gdzie dzis logujemy `ESP.getFreeHeap()`, dopisz drugi argument
`ESP.getMinFreeHeap()`. Miejsca: `pogoda-gdynia.ino:169-170`, `:261-262`, `:266`,
`RadarClient.cpp:344`, `BleSensors.cpp:341-342`.

Wzorzec (`pogoda-gdynia.ino:169-170`):

```cpp
  LOG("WiFi OK, IP: %s (%d dBm), heap %lu, minEver %lu\n", gIpStr, gRssi,
      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
```

**Zmiana 2 — dodaj granice tam, gdzie dzis jest luka (to sedno).**

```cpp
// Log.h — sonda fazowa. getMinFreeHeap() nie rosnie, wiec wystarczy ja drukowac
// na granicach faz: faza, w ktorej spadla, jest wskazana jednoznacznie.
#define HEAPMARK(tag) LOG("HEAP %-14s free %lu min %lu blok %lu\n", (tag), \
    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(), \
    (unsigned long)ESP.getMaxAllocHeap())
```

Punkty do wstawienia (kolejnosc odpowiada scenariuszowi z § 2.3):

| Gdzie | Znacznik | Po co |
|---|---|---|
| `pogoda-gdynia.ino:723` (po `gLock`) | `HEAPMARK("pre-ui")` | punkt odniesienia |
| `pogoda-gdynia.ino:734` (po `ui.begin()`) | `HEAPMARK("post-ui")` | **czy sprite zjadl SRAM** (rozstrzyga § 3.2 wprost) |
| `pogoda-gdynia.ino:788` (po `ble::begin()`) | `HEAPMARK("post-ble")` | realny koszt BLE na NimBLE (rozstrzyga § 2.4) |
| `pogoda-gdynia.ino:790` (po `radarmap::begin()`) | `HEAPMARK("post-radarmap")` | |
| `pogoda-gdynia.ino:798` (po obu `xTaskCreate`) | `HEAPMARK("post-tasks")` | **koszt 2 x 16 kB stosow** |
| `pogoda-gdynia.ino:150` (przed `WiFi.begin()`) | `HEAPMARK("pre-wifi")` | **otwiera okno A** |
| `pogoda-gdynia.ino:158` (zaraz po petli oczekiwania) | `HEAPMARK("post-wifi")` | **zamyka okno A** |
| `pogoda-gdynia.ino:508` (po `WiFi.scanDelete()`) | `HEAPMARK("post-scan")` | zamyka okno B |
| `pogoda-gdynia.ino:436` (po `ble::scan(4)`) | `HEAPMARK("post-blescan")` | zamyka okno E |

**Jak czytac wynik (`GET /api/log`):** znajdz **pierwszy** znacznik, przy ktorym `min` jest juz
~22 kB. Dolek zdarzyl sie miedzy nim a znacznikiem poprzednim. Koniec sledztwa.

Przyklad odczytu:
```
HEAP post-tasks     free 121340 min 121340 blok  60112
HEAP pre-wifi       free 121280 min 121280 blok  60112
HEAP post-wifi      free  95012 min  22044 blok  39100   <-- dolek TU: init WiFi + skan
```

**Koszt:** ~9 linii logu na starcie, po jednej na cykl radaru/BLE/roamingu. Bufor logu ma
3072 B i jest kolowy (`Log.cpp:7`) — przy 9 znacznikach startowych po ~55 B to ~500 B.
Statyczny RAM: **0 B** (makro, nie zmienna). Bezpieczne wobec bariery 76000 B.

> **Uwaga na bufor logu.** `logDump()` (`Log.cpp:58-72`) buduje `String` o `kSize + 64` B na
> stercie, w `webTask`. Jesli dolozysz duzo znacznikow cyklicznych, startowe zdazy wypasc
> z bufora kolowego, zanim je przeczytasz. Dlatego znaczniki cykliczne
> (`post-blescan`, `post-scan`) proponuje wlaczyc **dopiero w drugim kroku**, jesli krok
> pierwszy nie wskaze winowajcy w `setup()`/`connectWifi()`.

### 4.3. INSTRUMENTACJA #2 — sonda z etykieta faz (tylko jesli #1 nie wystarczy)

Jesli #1 pokaze, ze dolek **nie** jest w rozruchu, tylko gdzies w normalnej pracy, potrzebny
jest probnik ciagly z etykieta. Wtedy:

```cpp
// Log.cpp — dopisz obok gDiag
volatile const char* gPhase = "idle";   // 4 B, ustawiane przez netTask/webTask

// Zadanie probkujace. Prio 1 (nizej niz net=3 i web=2, wyzej niz idle), rdzen 0,
// stos 2048 B. Probkuje co 5 ms — handshake TLS trwa setki ms, wiec 5 ms wystarcza
// z zapasem. Nie bierze zadnego mutexa i nic nie alokuje.
static void heapProbeTask(void*) {
  for (;;) {
    const uint32_t h = ESP.getFreeHeap();
    if (h < diag().heapLow) {
      diag().heapLow = h;
      diag().heapLowAt = millis();
      strncpy(diag().heapLowTag, const_cast<const char*>(gPhase),
              sizeof(diag().heapLowTag) - 1);
      diag().heapLowBlock = ESP.getMaxAllocHeap();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
```

W `Diag` (`Log.h`): `uint32_t heapLow = 0xFFFFFFFF; uint32_t heapLowAt = 0;
uint32_t heapLowBlock = 0; char heapLowTag[16] = {};` — **+28 B statycznego RAM-u**
(zapas do bariery: 4376 B, wiec bez problemu). Stos zadania 2048 B idzie ze sterty, nie z `.bss`.

W `netTask` ustawiaj `gPhase` przed/po kazdej ciezkiej rzeczy: `"tls-weather"`, `"tls-flights"`,
`"tls-vi"`, `"tls-ota"`, `"wifi-connect"`, `"wifi-scan"`, `"ble-scan"`, `"radar"`,
`"nvs-rooms"`, `"nvs-pv"`. W `webTask`: `"tls-geo"`, `"screenshot"`.

W `/api/diag` (`Portal.cpp`, obok `:700`):

```cpp
  j["heap_low"] = d.heapLow;          // prawdziwe minimum, probkowane co 5 ms
  j["heap_low_tag"] = d.heapLowTag;   // W JAKIEJ FAZIE
  j["heap_low_at_s"] = d.heapLowAt / 1000;
  j["heap_low_block"] = d.heapLowBlock;
```

**To daje odpowiedz wprost: "najnizej bylo 22044 B, w fazie `tls-flights`, w 41. sekundzie
po starcie".** `heap_low` powinno byc bliskie `heap_min_ever` — jesli jest **znacznie
wyzsze**, to znaczy, ze dolek jest krotszy niz 5 ms (czyli to pojedyncza alokacja
pikowa, nie faza) i trzeba szukac inaczej.

**Nie polecam** `heap_caps_register_failed_alloc_callback()` — odpala sie dopiero **po**
nieudanej alokacji, czyli po szkodzie.

### 4.4. Poprawka do rozwazenia (nie wdrazam — zgodnie z brefem)

Niezaleznie od sledztwa: `diag().minHeap` w obecnej postaci nie ma sensu jako "minimum".
Najtansza uczciwa naprawa to **zastapienie go przez `ESP.getMinFreeHeap()`** w obu miejscach
(`Portal.cpp:699`, `WeatherUi.cpp:2550`) i **usuniecie** pola `Diag::minHeap` (`Log.h:36`) razem
z probkowaniem w `logPrintf()` (`Log.cpp:52-55`). Zysk: **-4 B** statycznego RAM-u, jeden
`ESP.getFreeHeap()` mniej **na kazda linijke logu**, i licznik, ktory mowi prawde.

---

## 5. Czy `heap_min` jest uzytecznym wskaznikiem — i czy klamie uzytkownikowi

### Werdykt: **nie jest uzyteczny, i tak — klamie. Wprost, w komentarzu i w kolorze.**

**Dowod 1 — komentarz obiecuje cos, czego kod nie robi.** `WeatherUi.cpp:2624`:

```cpp
      // Biala kreska = minimum, jakie sterta osiagnela od startu (najgorszy moment).
```

Biala kreska to `minHeap` (`WeatherUi.cpp:2629`):

```cpp
      zoneGauge(spr, gx, gy, gw, gh, static_cast<float>(heap), 0.f,
                static_cast<float>(cfg::HEAP_FULL), z, 3, static_cast<float>(minHeap), true);
```

A `minHeap` (`:2550`) to `d.minHeap`, czyli probka z `logPrintf()`. **Nie jest to ani "minimum
od startu", ani "najgorszy moment".** Prawdziwe minimum od startu to `ESP.getMinFreeHeap()`
= 22044 i **nie jest pokazywane nigdzie na ekranie**.

Skala wskaznika to `0 .. HEAP_FULL` (160000). Wiec:

- gdzie **stoi** biala kreska: 85464 / 160000 = **53 % skali — srodek strefy zielonej**
  (`HEAP_WARN = 45000`);
- gdzie **powinna stac**: 22044 / 160000 = **14 % skali — w czerwonej strefie**
  (`HEAP_DANGER = 25000`).

Kreska opisana jako "najgorszy moment" stoi **o 63 kB za wysoko** i po **niewlasciwej stronie
progu alarmowego**.

**Dowod 2 — falszywa liczba steruje kolorem karty.** `WeatherUi.cpp:2568-2571`:

```cpp
  // minimalna sterta jest wazniejsza niz biezaca — to ona ostrzega przed padem
  cards[1] = {"WOLNY SRAM", {0}, {0},
              minHeap < cfg::HEAP_DANGER
                  ? col::ERR
                  : (minHeap < cfg::HEAP_WARN ? col::WARN : col::OK)};
```

Komentarz jest **sluszny co do zasady** ("minimalna sterta jest wazniejsza niz biezaca —
to ona ostrzega przed padem") i **oparty na zlej danej**. Zamierzone dzialanie: karta
czerwienieje, gdy minimum spadlo ponizej `HEAP_DANGER`. Realne dzialanie:

- warunek liczony z 85464 -> `col::OK`, **karta zielona**;
- warunek liczony z prawdziwych 22044 -> `col::ERR`, **karta czerwona**.

**Alarm, ktory mial ostrzec przed dokladnie tym zdarzeniem, nie odpalil w chwili, gdy
zdarzenie zaszlo.** Progi z `Config.h:110-112` sa dobrane sensownie — tylko porownywane
z liczba, ktora nigdy nie zejdzie tak nisko.

**Dowod 3 — a liczba na karcie to i tak co innego.** `WeatherUi.cpp:2572-2573`:

```cpp
  snprintf(cards[1].value, sizeof(cards[1].value), "%lu kB",
           static_cast<unsigned long>(heap / 1024));
```

Wielka cyfra na karcie to `heap` (**biezaca**, ~92 kB), a nie `minHeap`. Czyli karta
"WOLNY SRAM" laczy: **biezaca** liczbe, **falszywe** minimum jako kolor i **falszywe**
minimum jako biala kreska.

**Dowod 4 — na tym urzadzeniu minimum nie jest nawet drukowane.** `WeatherUi.cpp:2575-2582`:

```cpp
  if (ESP.getPsramSize() > 0) {
    snprintf(cards[1].sub, sizeof(cards[1].sub), "PSRAM %.1f MB",
             ESP.getFreePsram() / 1048576.f);
  } else {
    snprintf(cards[1].sub, sizeof(cards[1].sub), "min %lu kB",
             static_cast<unsigned long>(minHeap / 1024));
  }
```

`psram = 2097152 > 0`, wiec podpis pokazuje PSRAM. Napis "min 83 kB" **nie pojawia sie**.
Uzytkownik dostaje wylacznie biala kreske — czyli **falsz bez liczby, ktora daloby sie
zakwestionowac**. Paradoksalnie to zmniejsza szkode: nie ma jawnie falszywego napisu, jest
tylko zle postawiona kreska.

### Podsumowanie § 5

`heap_min` **jako "minimum sterty" nie ma wartosci diagnostycznej zadnej** — przegapil
dolek o 63 kB, czyli o 74 % wlasnej wartosci, i przegapil go **systematycznie, nie przypadkiem**
(§ 1.3). Jedyne, co ta liczba uczciwie opisuje, to "najnizsza sterta w spoczynku, jaka
zaobserwowano przy okazji pisania logu" — wielkosc, ktorej nikt nie potrzebuje i ktorej nazwa
`heap_min` nie sugeruje.

Naprawa jest jednolinijkowa i **ujemna w statycznym RAM-ie** (§ 4.4): `ESP.getMinFreeHeap()`
robi dokladnie to, co komentarz w `WeatherUi.cpp:2624` juz obiecuje, i robi to za darmo,
bo alokator i tak to liczy.

---

## 6. Czy `stack_net_spare = 7364` to powod do niepokoju

### Werdykt: **nie. To najzdrowsza liczba w calym zestawie — i jedyna, ktorej mozna ufac.**

```
stos netTask       16384 B   (pogoda-gdynia.ino:797)
zapas w szczycie    7364 B   (45 % nietkniete)
zuzycie szczytowe   9020 B
```

### 6.1. Dlaczego tej liczbie mozna ufac (w odroznieniu od `heap_min`)

`pogoda-gdynia.ino:531-533`:

```cpp
    if (gNetTask != nullptr) {
      diag().stackNet = uxTaskGetStackHighWaterMark(gNetTask) * sizeof(StackType_t);
    }
```

`uxTaskGetStackHighWaterMark()` to **prawdziwy znacznik szczytu**, prowadzony przez jadro
FreeRTOS (wzorzec wypelnienia stosu). Zwraca **najgorszy przypadek od utworzenia zadania**,
a nie probke. Tu jest ladna asymetria warta odnotowania:

| Licznik | Mechanizm | Wiarygodnosc |
|---|---|---|
| `stack_net_spare` | znacznik szczytu (FreeRTOS) | **prawdziwy najgorszy przypadek** |
| `heap_min_ever` | znacznik minimum (alokator IDF) | **prawdziwy najgorszy przypadek** |
| `heap_min` | probka przy `logPrintf()` | **fikcja** |

Dwa z trzech licznikow sa uczciwe, bo liczy je za nas ktos inny. Ten jeden, ktory piszemy sami,
klamie. To jest morał tego audytu.

### 6.2. Co realnie stoi na tym stosie

Zidentyfikowane grube obiekty w `netTask`:

| Obiekt | Rozmiar | Miejsce |
|---|---|---|
| `RoomHistory snap = gRooms;` | **1736 B** | `pogoda-gdynia.ino:457` |
| `WeatherModel tmp{};` | **~1160 B** | `pogoda-gdynia.ino:248` |
| `PvHistory snap = gHist;` | ~584 B | `pogoda-gdynia.ino:541` |
| `FlightModel tmp{};` | zalezne od `FlightData.h` | `pogoda-gdynia.ino:551` |
| `RadarSnapshot rs{};` | maly | `pogoda-gdynia.ino:343` |
| `PvModel tmp{};` | maly | `pogoda-gdynia.ino:281` |

`RoomHistory` = `int16_t t10[6][144]` (1728 B) + `lastSlot` (4 B) + `head` (2 B) -> 1734,
wyrownane do **1736 B**. Zgadza sie z liczba z brefu.

**Ale to nie one robia szczyt 9 kB.** Wszystkie leza w rozlacznych zakresach `if` wewnatrz
`for(;;)` i GCC ponownie wykorzystuje sloty stosu. Szczyt robi **glebokosc wywolan**, nie
lokalne migawki:

1. **mbedTLS handshake** — najglebszy lancuch w calym firmwarze (weryfikacja certyfikatu,
   ECDHE, `mbedtls_ssl_handshake_step`). Realnie 3-5 kB samego stosu.
2. **ArduinoJson `deserializeJson()`** — parser **rekurencyjny**. Zagniezdzenie JSON-a przeklada
   sie wprost na glebokosc stosu. Uwaga: `PsramAlloc` (`Viessmann.cpp:59-64`) przenosi do PSRAM
   **dane dokumentu, nie stos parsera**. Odpowiedz Viessmanna `/features` (~53 kB, mocno
   zagniezdzona) parsuje sie **na tym samym stosie 16 kB**.
3. **`HTTPClient` + `String`** — konkatenacje i bufory posrednie.

Najglebsza sciezka to prawie na pewno `vi::fetch()` (`Viessmann.cpp`): TLS **plus** rekurencyjny
parser na duzym, zagniezdzonym JSON-ie. Chodzi co 3 min, wiec w 7155 s odpalila sie ~39 razy —
czyli **9020 B to najprawdopodobniej juz zmierzony szczyt tej wlasnie sciezki**, a nie przypadek.

### 6.3. Wniosek praktyczny

- **7364 B zapasu (45 %) to zdrowy margines.** Dla stosu z TLS i rekurencyjnym parserem
  reguła kciuka to >= 25 % zapasu; mamy prawie dwa razy tyle.
- **Nie skracac `netTask` ponizej ~12 kB.** Komentarz `pogoda-gdynia.ino:83-84` sugeruje, ze
  "jesli realnie biora 6-8 kB, mamy do odzyskania kilkanascie kB". **Pomiar mowi 9020 B, wiec
  ten pomysl jest juz zweryfikowany negatywnie dla `netTask`** — zejscie do 12 kB da 4 kB, a
  zjada caly margines bezpieczenstwa przed nieznana glebia parsera na nieznanym JSON-ie.
  Przepelnienie stosu = natychmiastowy panic = restart = klopot czlowieka. **Za 4 kB nie warto.**
- **`webTask` to inna sprawa:** `stack_web_spare = 12696` z 16384, czyli szczyt to **3688 B**.
  Tu zapas jest 77 %. Zejscie `webTask` do 8 kB odzyskuje **8 kB sterty** przy zachowanym
  zapasie ~54 %. To jest realne 8 kB do wziecia — i, co ciekawe, 8 kB **spojnego** SRAM-u
  wroconego do puli, co pomaga fragmentacji z § 3.2. **Uwaga:** `webTask` obsluguje zrzut
  ekranu, ktory "rysuje teraz cala klatke" (`pogoda-gdynia.ino:793-795`), wiec przed cieciem
  trzeba sie upewnic, ze 3688 B to szczyt **z wykonanym zrzutem** (odpal `/api/screen` i
  odczytaj `stack_web_spare` ponownie).
- **Poprawka do rozwazenia:** `RoomHistory snap` (1736 B) mozna zdjac ze stosu, robiac
  `static` (`netTask` jest jeden, wiec to bezpieczne) — ale to **+1736 B do bariery 76000 B**
  (zapas 4376 B). Audyt B proponowal `ps_malloc` (`audyt/B-wspolbieznosc-pamiec.md:348`).
  **Przy 45 % zapasu na stosie to rozwiazanie problemu, ktorego nie ma.** Zostawic.

---

## 7. Ustalenia poboczne (nie wchodza w zakres, ale wyszly po drodze)

1. **`BleSensors.cpp:26`** — `kMinHeapForBle = 100000` jest **martwa stala**, nieuzywana nigdzie
   w repo. Opisana w komentarzu bramka "nie podnosimy BLE, jesli sterta jest napieta"
   **nie istnieje**.
2. **`BleSensors.cpp:345-348`** — komentarz nad `scan()` opisuje architekture sprzed v51
   ("stos podnosimy TYLKO na czas nasluchu i zaraz go oddajemy") i jest **sprzeczny** z
   komentarzem nad `begin()` (`:323-326`) oraz z kodem (`scan()` nie robi `deinit()`).
3. **"6 s nasluchu co 45 s"** (`BleSensors.cpp:347`, `pogoda-gdynia.ino:432-433`) vs. kod:
   `ble::scan(4)` co 20 s (`pogoda-gdynia.ino:435,466`). Trzy miejsca, dwie nieprawdy.
4. **`Ota.cpp:91`** — "Poczekaj, az UI odda bufor ekranu (**150 kB**)". Od zmiany na dwa pasy
   bufor ma **66 kB** (`ARCHITEKTURA.md:231`). Ta sama nieaktualna liczba w `Ota.cpp:228`
   ("Oddajemy wiec 150 kB bufora ekranu").
5. **`pogoda-gdynia.ino:77-80`** — "Sterty jest 113 kB, ale bufor ekranu (66 kB) siedzi
   w srodku" opisuje swiat sprzed PSRAM. Arytmetyka tego komentarza nie domyka sie
   z `heap_free = 94548` przy rezydentnym BLE, WiFi i 32 kB stosow — pula sterty jest
   **istotnie wieksza** niz 113 kB. `ARCHITEKTURA.md:232` powtarza te liczbe.
6. **Kluczowa diagnostyka OTA idzie na slepy Serial** (`Ota.cpp:67-69,98-99,133-134`) —
   w tym "heap przed pobieraniem", ktory jest najwazniejsza liczba calego OTA. Zamiana
   `Serial.printf` -> `LOG` w tych trzech miejscach kosztuje 0 B statycznego RAM-u i czyni
   je widocznymi w `/api/log`. To samo `WeatherUi.cpp:1790-1791,1808-1809`.

---

## 8. Podsumowanie

**Odpowiedz na zagadke (jedno zdanie):**
`heap_min_ever` (22044) to prawdziwy znacznik minimum prowadzony przez alokator ESP-IDF przy
kazdym `malloc`/`free`, a `heap_min` (85464) to probka pobierana **wylacznie wewnatrz
`logPrintf()`** (`Log.cpp:52-55`, **nie w `loop()`**) — a poniewaz kazdy `LOG` z heapem stoi
*za* ciezka operacja, ktora pamiec juz oddala (dowod: `pogoda-gdynia.ino:261` loguje po powrocie
z `weatherClient.fetch()`, gdy destruktor `WiFiClientSecure` z `WeatherClient.cpp:182` juz
zwolnil ~40 kB TLS), nasz licznik **z konstrukcji mierzy spoczynek** i tych 63 kB nie mial jak
zobaczyc.

**Werdykt — czy grozne:**
**Nie bezposrednio.** Dolek minal, `was_crash: false`, 7155 s uptime, a `heap_min_ever` nie mowi
nic o czestosci — tylko o tym, ze raz tam bylo. Prawdziwym ryzykiem nie jest liczba wolnych
bajtow, tylko **fragmentacja**: `heap_largest_block = 38900` przy `heap_free = 94548`, co juz
dzis wyzwala `needMem` (`pogoda-gdynia.ino:336`: `38900 < 48000`) i zamraza ekran na 3 s co
5 minut — prawdopodobnie **bez zadnego zysku**, jesli sprite siedzi w PSRAM. Najgorszy realny
scenariusz to nie restart, tylko **v14**: TLS przestaje wchodzic -> OTA niemozliwe -> skrzynka
w lazience nie do naprawienia zdalnie (`Ota.cpp:226-228`).

**Jedna najwazniejsza rzecz do zmierzenia:**
**`radar.skips_low_ram` z `GET /api/diag`** — nic nie trzeba wgrywac. Radar chodzi co 5 min
(~23 razy w tym uptime) i przy kazdym uruchomieniu sam sprawdza sterte z progiem 64 kB,
inkrementujac ten licznik (`RadarClient.cpp:225-231`, wystawiony w `Portal.cpp:737`). **`0` =
przy wszystkich 23 podejsciach bylo >= 64 kB, czyli 22044 to jednorazowe okno przy rozruchu
i temat zamkniety. `> 0` = sterta bywa nisko regularnie, dolki sa rozkladem a nie ciekawostka,
i wtedy werdykt zmienia sie na "grozne".** Zaraz za tym: porownaj `mem.psram_free` z `psram`
(2097152) — to rozstrzyga, czy bufor 66 kB naprawde jest w PSRAM, a wiec czy `gRadarWantMem`
i siec bezpieczenstwa OTA to realne mechanizmy, czy martwe rytualy.

Jesli po tych dwoch odczytach cos zostanie niejasne — **instrumentacja #1 z § 4.2**
(bracketowanie `getMinFreeHeap()` na granicach faz). Zero nowych watkow, 0 B statycznego RAM-u,
i wskazuje faze-winowajce jednoznacznie, bo ta wartosc nigdy nie rosnie.
