# Audyt B — współbieżność i pamięć

Zakres: wyścigi między `netTask` / `webTask` / `loop()` / task BLE, przepełnienia buforów,
alokacje, arytmetyka `millis()`, stosy zadań. Bez stylu, bez refaktoru — tylko rzeczy,
które wywalają urządzenie, psują dane albo dają fałszywy odczyt.

Metoda: czytanie kodu + weryfikacja semantyki API rdzenia (źródła `Preferences.cpp`
z arduino-esp32 3.3.0 i dokumentacja ESP-IDF v5.5 dla ESP32-S3). Tam, gdzie czegoś
nie dało się rozstrzygnąć bez urządzenia, napisane jest to wprost.

---

## Mapa par `g*` / `ui*` — wynik przeglądu

Wszystkie globalne `g*`/`ui*` żyją **wyłącznie** w `pogoda-gdynia.ino` (zweryfikowane
grepem po całym repo — żaden inny plik ich nie widzi, nie ma nawet jednego `extern`).
To dobra wiadomość: `Portal.cpp` **nie czyta stanu współdzielonego bezpośrednio**.

| stan sieciowy | kopia UI | kopiowane w `loop()` pod `gLock`? | uwagi |
|---|---|---|---|
| `gWeather` | `uiWeather` | tak (`.ino:838`) | OK |
| `gPv` | `uiPv` | tak (`.ino:839`) | OK |
| `gHist` | `uiHist` | tak (`.ino:840`) | OK |
| `gRooms` | `uiRooms` | tak (`.ino:841`) | OK — poprawka po błędzie z `setup()` weszła |
| `gVi` | `uiVi` | tak (`.ino:842`) | OK — j.w. |
| `gBurner` | `uiBurner` | tak (`.ino:843`) | OK |
| `gFlights` | `uiFlights` | tak (`.ino:844`) | OK |
| **`gGas`** | **BRAK** | **nie istnieje** | **patrz W-4 — dane zapisywane w próżnię** |

Komplet kopii w `loop()` jest **poprawny i zamknięty pod jednym `xSemaphoreTake`**
(`.ino:837-845`). Zapomniana jest dokładnie jedna rzecz: `gGas`. Nie ma pary `uiGas`,
bo nikt tych danych nigdy nie czyta (W-4).

**Blokady vs. operacje długie** — przegląd wszystkich `xSemaphoreTake(gLock)`:

* `.ino:439-443` `roomHistorySave(snap)` — mutex **zwolniony** przed zapisem NVS i wzięty
  z powrotem. Autor pamiętał. Poprawnie.
* `.ino:503-505` `pvHistorySave(snap)` — snapshot pod mutexem, zapis **poza** nim. Poprawnie.
* `.ino:263-266` (`sunrise`/`sunset`), `271-287` (PV), `339-343` (radar), `374-400` (piec),
  `422-446` (BLE), `516-518` (loty) — wszystkie operacje sieciowe (`fetch`) są **poza**
  mutexem, pod mutexem lecą tylko przypisania w RAM. Poprawnie.
* Kolejność blokad: `gLock` → `ble::gMx` (`.ino:424`, `ble::count()`/`ble::get()` w sekcji
  krytycznej). Ścieżki odwrotnej (`ble::gMx` → `gLock`) nie ma nigdzie — **brak ryzyka
  zakleszczenia**.

**Nie znalazłem ani jednego miejsca, gdzie `gLock` byłby trzymany podczas operacji
sieciowej albo zapisu NVS.** Ta część jest zrobiona dobrze.

Problemy są gdzie indziej: w stanie, którego `gLock` **w ogóle nie chroni**, bo nikt nie
uznał go za stan współdzielony.

---

# KRYTYCZNE

## K-1. Jeden obiekt `Preferences prefs` dzielony przez `netTask` i `webTask` → zapis do złej przestrzeni NVS i ciche gubienie ustawień

**Plik:** `Settings.cpp:12` (`Preferences prefs;` w anonimowym namespace)

Ten jeden obiekt obsługuje **wszystkie** operacje NVS w projekcie, wołane z **dwóch zadań
naraz**:

* z `netTask`: `roomHistorySave()` (`Settings.cpp:308`, co 10 min), `pvHistorySave()`
  (`Settings.cpp:272`, co 5 min), `Settings::viSave()` (`Settings.cpp:110`, z
  `Viessmann.cpp:160` przy rotacji tokena i z `Viessmann.cpp:291` przy `ensureIds`)
* z `webTask`: `Settings::save()` (`Portal.cpp:442, 534, 550, 598, 847` + konsola szeregowa
  `1050-1120`), `Settings::viSave()` (`Portal.cpp:865`), `Settings::bleSet()`
  (`Settings.cpp:181`, z `Portal.cpp:818`), `Settings::clearWifi()` (`Portal.cpp:937`),
  `vi::forget()` → `viSave()` (`Portal.cpp:932`)

`Preferences` **nie jest re-entrantne i nie broni się przed tym**. Zweryfikowałem w źródle
arduino-esp32 3.3.0 (`libraries/Preferences/src/Preferences.cpp`):

```cpp
bool Preferences::begin(const char *name, bool readOnly, const char *partition_label) {
  if (_started) { return false; }          // <-- zajęte? wychodzi, ale _handle ZOSTAJE cudzy
  ...
  err = nvs_open(name, readOnly ? NVS_READONLY : NVS_READWRITE, &_handle);
  ...
  _started = true;
  return true;
}
void Preferences::end() {
  if (!_started) { return; }
  nvs_close(_handle);                      // <-- zamyka CUDZY uchwyt
  _started = false;
}
size_t Preferences::putString(const char *key, const char *value) {
  if (!_started || !key || !value || _readOnly) { return 0; }   // <-- _started == true!
  esp_err_t err = nvs_set_str(_handle, key, value);             // <-- pisze pod CUDZY _handle
  ...
}
```

Kod projektu **nigdzie nie sprawdza wartości zwracanej przez `begin()`** (`Settings.cpp:23,
88, 111, 122, 221, 239, 250, 273, 296, 309`).

**Kiedy dokładnie wybucha (przeplot krok po kroku):**

1. `netTask` (`.ino:441`) → `roomHistorySave()` → `prefs.begin("pvday", false)` →
   `_started = true`, `_handle` = uchwyt do `pvday`.
2. Użytkownik w tej samej chwili klika „Zapisz" w panelu. `webTask` → `apiWifi()` →
   `settings().save()` → `prefs.begin("pogoda", false)` → **zwraca `false`, ale nikt tego
   nie czyta**. `_handle` dalej wskazuje `pvday`.
3. `webTask` → `prefs.putString("ssid", ...)` → `_started` jest `true`, więc funkcja
   wykonuje `nvs_set_str(_handle = pvday, "ssid", ...)` → **SSID i hasło lądują
   w przestrzeni `pvday`**, nie w `pogoda`.
4. `webTask` → `prefs.end()` → `nvs_close(pvday)`, `_started = false`.
5. `netTask` wraca do `prefs.putBytes("rh2", ...)` → `_started` jest już `false` →
   funkcja **cicho zwraca 0**. Historia 24 h nie została zapisana.
6. `netTask` → `prefs.end()` → no-op.

**Skutek dla użytkownika:** użytkownik zmienia coś w panelu, panel melduje „Zapisano",
a po restarcie ustawienie jest stare — bo `Settings::load()` czyta z przestrzeni `pogoda`,
a zapis poszedł do `pvday`. Przy **pierwszej** konfiguracji WiFi (gdy w `pogoda` nie ma
jeszcze `ssid`) urządzenie po restarcie wstaje w trybie AP „Pogoda-Setup" i nie łączy
się z siecią. Wariant odwrotny (webTask trzyma `pogoda`, netTask wchodzi z `roomHistorySave`)
wsadza blob 1736 B pod kluczem `rh2` do przestrzeni **konfiguracyjnej** — a partycja NVS
przy `min_spiffs` jest mała, więc powtarzanie tego może ją zapchać i wtedy zaczną padać
także zapisy `settings().save()` (`nvs_set_*` → `ESP_ERR_NVS_NOT_ENOUGH_SPACE`, też cicho).

**Prawdopodobieństwo — uczciwie:** okno to czas trwania jednego zapisu NVS (rząd 10-50 ms)
w cyklu 5 min (`pvHistorySave`) i 10 min (`roomHistorySave`). To ~0,01 % czasu. Na jedno
kliknięcie „Zapisz" szansa trafienia jest rzędu ułamka promila. **Ale**: (a) nie jest
zerowa, (b) urządzenie ma działać miesiącami, (c) skutek jest cichy — nie ma crashu, nie
ma wpisu w logu, jest tylko „przecież zapisywałem". To jest dokładnie klasa błędu, której
nie da się zdiagnozować po fakcie.

**Proponowana poprawka:** usunąć globalny `prefs` i w każdej funkcji użyć **lokalnego**
obiektu — dokładnie tak, jak już robi `OtaGuard.cpp:61, 74, 232` (`Preferences p;`). Samo
NVS (`nvs_open`/`nvs_set_*`/`nvs_close`) jest w ESP-IDF bezpieczne wątkowo; niebezpieczny
jest wyłącznie **współdzielony obiekt C++** ze stanem `_handle`/`_started`. Dodatkowo
sprawdzać wartość `begin()`.

**Koszt RAM: −12 B statycznie** (znika globalny obiekt `Preferences`, ~12 B; instancje
lądują na stosie wołającego, po ~12 B). Poprawka **zwalnia** pamięć, nie zajmuje.

**Ryzyko poprawki: niskie.** Zmiana mechaniczna, ograniczona do `Settings.cpp`. Jedyny
efekt uboczny: `nvs_open`/`nvs_close` przy każdym wywołaniu zamiast raz — koszt mikrosekund
przy operacji, która i tak dotyka flasha. `OtaGuard.cpp` robi tak od dawna i działa.

---

## K-2. `WiFi.begin()` wołane z dwóch zadań → panel melduje sukces, którego nigdy nie sprawdził, i zapisuje niezweryfikowane hasło

**Pliki:** `Portal.cpp:427-442` (`apiWifi`, webTask) vs `pogoda-gdynia.ino:215-221` +
`130-158` (`connectWifi`, netTask)

`apiWifi()` ma świadomy zamysł — komentarz w `Portal.cpp:426` mówi *„sprobuj polaczyc zanim
zapiszemy"*:

```cpp
WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
WiFi.begin(s, p);                                   // NOWE dane z panelu
const uint32_t t0 = millis();
while (WiFi.status() != WL_CONNECTED && millis() - t0 < 14000) { delay(200); }
if (WiFi.status() != WL_CONNECTED) { /* "sprawdź hasło" */ return; }
strncpy(settings().ssid, s, ...);                   // zapis DOPIERO po udanym teście
settings().pass ...; settings().save();
```

Problem: przez te 14 sekund `netTask` kręci swoją pętlą co 250 ms i widzi:

```cpp
if (WiFi.status() != WL_CONNECTED) {                // .ino:215 — a właśnie NIE jest,
  gWifiOk = false;                                  //   bo apiWifi zerwał asocjację
  if (!connectWifi()) { ... }                       // .ino:217
}
```

a `connectWifi()` (`.ino:148`) robi:

```cpp
WiFi.begin(settings().ssid, settings().pass);       // STARE dane z NVS!
```

**Kiedy dokładnie wybucha:** za **każdym razem**, gdy użytkownik zmienia sieć WiFi na
urządzeniu, które ma już zapisaną jakąś sieć (`settings().hasWifi() == true`). `WiFi.begin(s,p)`
w `apiWifi` natychmiast zrywa bieżącą asocjację → w ciągu ≤250 ms `netTask` to zauważa →
woła `WiFi.begin()` ze **starymi** danymi → nadpisuje próbę z panelu i sam blokuje się na
12 s w `connectWifi`.

Możliwe zakończenia:
* Urządzenie wraca na **starą** sieć. `apiWifi` widzi `WL_CONNECTED`, uznaje test za zdany,
  **zapisuje nowe (nigdy niesprawdzone) dane** i odpowiada „Połączono. Panel: http://…"
  — z adresem IP w starej sieci. Jeśli użytkownik zrobił literówkę w haśle, dowie się o tym
  dopiero po restarcie: urządzenie nie wstanie w sieci i wpadnie w tryb AP.
* Albo obie próby się zabijają nawzajem, `apiWifi` mija się z 14 s i melduje
  **„Nie udało się połączyć — sprawdź hasło"** przy **poprawnym** haśle.

Test „sprobuj polaczyc zanim zapiszemy" jest więc dziś **atrapą** — sprawdza nie to, co
myśli, że sprawdza.

Uwaga (zawęża zakres): przy **świeżym** urządzeniu bez zapisanej sieci wyścigu **nie ma** —
`netTask` wychodzi wcześniej na `if (!settings().hasWifi())` (`.ino:209`) i nigdy nie woła
`connectWifi()`. Błąd dotyczy wyłącznie **zmiany** istniejącej konfiguracji.

**Skutek dla użytkownika:** fałszywy komunikat w obie strony + zapisanie hasła, którego
nikt nie zweryfikował. Urządzenie po restarcie znika z sieci i wraca w tryb AP. Do
odzyskania bez zdejmowania ze ściany (przez AP „Pogoda-Setup"), ale użytkownik nie ma jak
zrozumieć, co się stało — panel przecież powiedział „Połączono".

**Proponowana poprawka:** flaga `volatile bool gWifiCfgBusy` ustawiana przez `apiWifi` na
czas próby; `netTask` na jej widok pomija blok `connectWifi()` (`.ino:215-221`) i tylko
czeka. Flaga musi być kasowana także na ścieżkach błędu (inaczej netTask przestanie
odtwarzać połączenie na stałe — najlepiej strażnik RAII albo dodatkowo twardy limit czasu
na flagę).

**Koszt RAM: +1 B statycznie** (71 600 → 71 601, zapas 4,4 kB → 4,4 kB).

**Ryzyko poprawki: średnie.** Główne ryzyko to zapomniane kasowanie flagi na ścieżce
błędu → `netTask` nigdy nie odzyska WiFi → urządzenie ślepe do restartu. Dlatego twardy
limit czasu na flagę jest tu obowiązkowy, nie opcjonalny.

---

## K-3. `WiFi.scanNetworks()` / `WiFi.scanDelete()` z dwóch zadań → dostęp do zwolnionej pamięci → panic i restart

**Pliki:** `Portal.cpp:397-411` (`apiScan`, webTask) vs `pogoda-gdynia.ino:462-486`
(roaming, netTask)

Wyniki skanu WiFi to **jeden globalny bufor w `WiFiScanClass`** (`_scanResult`, tablica
`wifi_ap_record_t` na stercie). Obie strony na nim operują bez żadnej synchronizacji:

`webTask` (`Portal.cpp:397-411`):
```cpp
const int n = WiFi.scanNetworks(false, true);
for (int i = 0; i < n && i < 24; ++i) {
  o["s"] = WiFi.SSID(i);            // czyta _scanResult[i]
  o["r"] = WiFi.RSSI(i);
  o["e"] = WiFi.encryptionType(i);
}
WiFi.scanDelete();                  // free(_scanResult)
```

`netTask` (`.ino:463-476`):
```cpp
const int found = WiFi.scanNetworks(false, false, false, 250);   // w środku woła scanDelete()
for (int i = 0; i < found; ++i) {
  if (WiFi.SSID(i) != settings().ssid) continue;                 // czyta _scanResult[i]
  ...
}
memcpy(bssid, WiFi.BSSID(best), 6);
WiFi.scanDelete();
```

**Kiedy dokładnie wybucha:** użytkownik otwiera panel i klika „Wyszukaj sieci
bezprzewodowe" w chwili, gdy `netTask` robi przegląd roamingowy. `WiFi.scanNetworks()`
w `apiScan` zaczyna od zwolnienia poprzednich wyników — a `netTask` właśnie po nich
iteruje (`WiFi.SSID(i)`, `WiFi.BSSID(best)`). Odczyt ze zwolnionego bloku → `LoadProhibited`
→ **panic i restart urządzenia**. Symetrycznie: `netTask` wchodzi w roaming, gdy `webTask`
składa JSON-a z wyników (to okno jest **większe** — budowa JSON-a dla 24 sieci to
alokacje i milisekundy, nie mikrosekundy).

Roaming odpala się co 3 min (`.ino:459`) **oraz** natychmiast po sprzętowym zdarzeniu
RSSI < −67 dBm (`.ino:99-101, 455`). Urządzenie wisi w łazience, czyli daleko od punktu
dostępowego — próg −67 dBm nie jest tam egzotyczny, to może być stan normalny.

**Prawdopodobieństwo — uczciwie:** wymaga zbiegu „użytkownik klika skan" + „netTask
akurat skanuje". Okno jest małe (ms). Ale skutek to twardy crash, a użytkownik klika skan
dokładnie wtedy, gdy grzebie w panelu przy słabym sygnale — czyli wtedy, gdy roaming też
jest najaktywniejszy. Te dwa zdarzenia są **skorelowane**, a nie niezależne.

**Skutek dla użytkownika:** restart urządzenia (widoczny potem w `/api/diag` jako
`reset.reason = "wyjątek (panic)"` i `crashes_total` +1). Jeśli trafi w okres próbny po
OTA — `OtaGuard` uzna świeżą wersję za niesprawną i ją cofnie, choć wersja jest w porządku.

**Proponowana poprawka:** jeden mutex `gScanLock` obejmujący **cały** blok
„skan + odczyt wyników + `scanDelete()`" po obu stronach. Alternatywa bez mutexa:
`apiScan` nie skanuje sam, tylko zgłasza prośbę do `netTask` (wzorzec już użyty dla OTA:
`requestOtaCheck()` / `takeOtaRequest()`, `Ota.cpp:33-43`) i zwraca wynik z jego skanu.

**Koszt RAM: +4 B statycznie** (uchwyt `SemaphoreHandle_t`; sam obiekt mutexa ~80 B idzie
na stertę, nie do statycznego RAM-u). 71 600 → 71 604, zapas 4,4 kB → praktycznie bez zmian.

**Ryzyko poprawki: niskie-średnie.** Uwaga: `netTask` trzyma taki mutex przez cały skan
(ok. 1-2 s przy `WIFI_ALL_CHANNEL_SCAN`), więc `apiScan` w `webTask` będzie tyle czekał —
panel na moment przymuli. Nie wolno brać tego mutexa pod `gLock` (odwrotna kolejność
blokad); w obu miejscach `gLock` i tak nie jest trzymany, więc problemu nie ma, ale trzeba
tego pilnować przy zmianach.

---

# WAŻNE (fałszywy odczyt / ciche psucie danych)

## W-1. Zrzut ekranu w `webTask` czyta kopie `ui*` bez `gLock` — panel pokazuje klatkę, której urządzenie nigdy nie wyświetliło

**Pliki:** `pogoda-gdynia.ino:722-731` (handler), `WeatherUi.cpp:2667-2732`
(`streamScreenshot`), `WeatherUi.cpp:1302-1323` (`paintFrame`)

Mutex `gLock` chroni parę `g*` → `ui*`. Ale kopie `ui*` mają **drugiego czytelnika**,
o którym wzorzec nie wie: handler zrzutu ekranu, wołany z `webTask` (rdzeń 0):

```cpp
// .ino:730 — webTask
ui.streamScreenshot(c, uiWeather, uiPv, uiHist, uiFlights, gWifiOk);
```

a `loop()` na rdzeniu 1 w tym samym czasie te struktury **nadpisuje**:

```cpp
// .ino:837-845 — rdzeń 1
xSemaphoreTake(gLock, portMAX_DELAY);
uiWeather = gWeather;   // ~1,2 kB memcpy
uiPv = gPv; uiHist = gHist; uiRooms = gRooms;   // uiRooms: 1736 B
uiVi = gVi; uiBurner = gBurner; uiFlights = gFlights;
xSemaphoreGive(gLock);
```

`gLock` **nie chroni tu niczego** — `webTask` go nie bierze. To samo dotyczy wskaźników,
które `WeatherUi` trzyma na stałe (`WeatherUi.h:81-83`): `rooms_ = &uiRooms`,
`boiler_ = &uiVi`, `burner_ = &uiBurner` — `paintFrame` → `drawView` → `drawViewHome` /
`drawViewBoiler` / `drawGasChart` (`WeatherUi.cpp:2132`) czytają je wprost z `webTask`,
podczas gdy `loop()` je przepisuje.

Dodatkowo `paintFrame` czyta stan animacji obiektu `ui` (`alertActive_`, `transitioning_`,
`view_`, `prevView_`, `enterStart_`, `transStart_` — `WeatherUi.cpp:1305-1322`), który
`render()` na rdzeniu 1 modyfikuje (`WeatherUi.cpp:1359-1394`). Zrzut trwa **setki
milisekund** (10 pasków po 24 px, między nimi transmisja BMP przez `client.write`), więc
w trakcie jednego zrzutu `loop()` zdąży przepisać `ui*` **kilkanaście razy**.

**Kiedy dokładnie:** przy każdym otwartym panelu — strona sama ciągnie `/api/screen` co
700 ms (`Portal.cpp:214-220`, funkcja `nextShot()`). Czyli **praktycznie zawsze, gdy panel
jest otwarty**.

**Skutek dla użytkownika:** rozdarty odczyt — zrzut skleja dane z dwóch różnych chwil
(np. temperatura z nowej prognozy, wykres godzinowy ze starej; wykres pokoi w połowie
przepisany). Podgląd „na żywo" w panelu pokazuje klatkę, której na TFT nigdy nie było.
**Crashu tu nie ma**: wszystkie tablice w tych strukturach są stałej długości, a liczniki
(`FlightModel::count` ≤ `FLIGHT_MAX`) nie mogą wyjść poza zakres nawet przy rozdarciu.
To jest fałszywy odczyt, nie wywalenie.

**Uwaga — to, co autor zrobił dobrze:** `streamScreenshot` faktycznie rysuje do
**własnego** sprite'a (`TFT_eSprite shot`, `WeatherUi.cpp:2692`) i nie dotyka ani bufora
wyświetlacza `spr_`, ani magistrali SPI. Komentarz z `.ino:718-721` jest prawdziwy —
sprawdziłem całą ścieżkę. Ryzyka „dwa rdzenie na jednym SPI" **nie ma**.

**Proponowana poprawka — trzy warianty, każdy z kosztem:**

1. *(najtańszy, zalecany)* Zaakceptować rozdarcie i **udokumentować** je w komentarzu przy
   handlerze. Zrzut jest podglądem, nie źródłem prawdy. Koszt RAM: 0 B. Ryzyko: 0.
2. Wziąć `gLock` w `streamScreenshot` na czas całego zrzutu. **Odradzam**: `webTask`
   trzymałby mutex przez setki ms, blokując `netTask` (a ten pod `gLock` robi `gRooms.advance`
   i publikacje) i `loop()`. To zamieniłoby fałszywy odczyt na zacinanie się urządzenia.
   Koszt RAM: 0 B. Ryzyko: **wysokie**.
3. Zrobić w `streamScreenshot` własną migawkę pod `gLock` na wejściu. Poprawne, ale to
   ~4 kB (`WeatherModel` ~1,2 kB + `RoomHistory` 1736 B + reszta) — na stosie `webTask`
   (16 kB) ciasno, więc migawka musiałaby iść do PSRAM (`ps_malloc`, ze sprawdzeniem NULL).
   Koszt RAM statycznego: 0 B (PSRAM). Ryzyko: średnie (nowa alokacja na ścieżce, która
   dziś nic nie alokuje poza sprite'em).

Rekomendacja: wariant 1. To nie jest błąd, który boli użytkownika — boli tylko wtedy, gdy
ktoś uzna zrzut za dowód w diagnostyce.

---

## W-2. `radarmap::levelAt()` czytane z `loop()` bez `gMx` — mapa radaru może pokazać dwie różne chwile naraz

**Pliki:** `RadarMap.cpp:326-330` (`levelAt`, bez blokady) vs `RadarMap.cpp:279-285`
(`resample` pod `gMx`) i `RadarMap.cpp:363-395` (`setDemo` pod `gMx`)

`RadarMap` ma własny mutex `gMx` i używa go przy **zapisie** klatek (`fetch()` w `netTask`,
`setDemo()` w `webTask`). Ale **odczyt** — `levelAt()`, wołany z `loop()` przy rysowaniu
ekranu radaru — mutexa nie bierze wcale. Tak samo `count()`, `frame()`, `hasRain()`.

**Kiedy:** co 10 min (`cfg::RADAR_MAP_REFRESH_MS`), gdy `netTask` przepisuje `gFrames[i]`
przez `resample()`, a akurat wyświetlany jest ekran radaru (`cfg::VIEW_RADAR`). Oraz
natychmiast, gdy użytkownik kliknie „Włącz symulację" w panelu — `setDemo()` przepisuje
wtedy wszystkie 7 klatek (`RadarMap.cpp:370-394`).

**Skutek dla użytkownika:** jedna klatka animacji narysowana w połowie ze starych,
w połowie z nowych danych — front opadu na moment „pęka" w poprzek. Kosmetyka, bez crashu:
bufory `gFrames[]` są alokowane raz w `begin()` i **nigdy nie zwalniane**, więc nie ma
ryzyka dostępu do zwolnionej pamięci.

**Proponowana poprawka:** albo wziąć `gMx` w `levelAt()` (uwaga: to wołanie **per piksel**
w pętli rysującej — mutex na piksel zabiłby wydajność; trzeba by objąć nim całe
`drawViewRadar`), albo zostawić i udokumentować.

**Koszt RAM: 0 B.** **Ryzyko poprawki: średnie** — objęcie mutexem całego `drawViewRadar`
oznacza, że `loop()` (rdzeń 1) trzyma blokadę, na którą czeka `netTask` w `resample()`.
Rekomendacja: **zostawić**, to kosmetyka na ekranie odświeżanym co 650 ms.

---

## W-3. `/api/diag` gubi `wifi.ssid`, `wifi.ip` i `wifi.connects` — jedyne okno diagnostyczne kłamie przez pominięcie

**Plik:** `Portal.cpp:647-651` i `Portal.cpp:691-695`

```cpp
JsonObject w = j["wifi"].to<JsonObject>();      // :647
w["ssid"] = WiFi.SSID();
w["ip"] = WiFi.localIP().toString();
w["rssi"] = WiFi.RSSI();
w["connects"] = d.wifiConnects;
...
JsonObject wf = j["wifi"].to<JsonObject>();     // :691 — TEN SAM klucz, drugi raz
wf["rssi"] = WiFi.RSSI();
wf["bssid"] = WiFi.BSSIDstr();
wf["channel"] = WiFi.channel();
wf["roams"] = d.wifiRoams;
```

`to<JsonObject>()` na istniejącym kluczu w ArduinoJson **zamienia go na nowy, pusty
obiekt**. Drugie wywołanie kasuje cały dorobek pierwszego.

**Kiedy dokładnie:** przy **każdym** `GET /api/diag`, zawsze, deterministycznie.

**Skutek dla użytkownika:** `/api/diag` (jedyne okno na urządzenie wiszące bez USB) nie
pokazuje `ssid`, `ip` ani `connects` — pola po prostu nie istnieją w odpowiedzi. Osoba
diagnozująca zdalnie nie widzi, do której sieci urządzenie jest podłączone ani ile razy
się przelogowało. Nie „zła wartość" — **brak wartości**, co przy diagnostyce jest gorsze,
bo wygląda jak brak funkcji.

**Proponowana poprawka:** usunąć drugie `to<JsonObject>()` (`:691`) i dopisać `bssid`,
`channel`, `roams` do obiektu `w` z linii 647.

**Koszt RAM: 0 B statycznie.** **Ryzyko poprawki: znikome** — zmiana lokalna, jeden blok.
(Uwaga: to na granicy zakresu agenta C — kontrakt API. Zgłaszam, bo to fałszywy odczyt
w narzędziu, którym diagnozuje się wszystko inne.)

---

## W-4. `gGas` — 120 dni historii gazu zbierane do pamięci, nigdy nieczytane i nigdy niezapisywane

**Pliki:** `pogoda-gdynia.ino:70` (`GasHistory gGas{};`), `pogoda-gdynia.ino:391-393`
(jedyne użycie), `GasMeter.h:18-80`

`gGas` jest **wyłącznie zapisywany**. Grep po całym repo: poza deklaracją (`.ino:70`)
i wywołaniami `advance()`/`push()` w `netTask` (`.ino:391-392`) **nie ma ani jednego
odczytu**. Konkretnie:

* `GasHistory::sumBetween()` (`GasMeter.h:65`) — nigdy nie wołane.
* Nie ma pary `uiGas` ani `ui.setGasHistory()`.
* `WeatherUi::drawGasChart()` (`WeatherUi.cpp:2130-2156`) **nie rysuje z `gGas`** — rysuje
  z `burner_` (`BurnerHistory`, modulacja palnika). Liczba „Gaz dziś" (`WeatherUi.cpp:2091`)
  bierze się z `b.gasDhwM3 + b.gasHeatM3`, czyli z bieżącego odczytu `vi::Model`, nie z historii.
* Nie ma `gasHistorySave()` / `gasHistoryLoad()` — **każdy restart kasuje wszystko**.
* Powiązane: `Settings::meterAdd()` / `meterDel()` (`Settings.cpp:128, 160`) też nie są
  wołane znikąd — w `Portal.cpp:943-968` (`routes()`) **nie ma trasy** dla odczytów licznika.

**Skutek dla użytkownika:** cel opisany w komentarzu `GasMeter.h:5-16` („zbieramy je sami,
dzień po dniu, i sumujemy między odczytami licznika", z konkretnym uzasadnieniem, że
liczniki pieca są zepsute) **nie jest realizowany w ogóle**. Funkcja weryfikacji pieca
licznikiem gazu nie istnieje — jest tylko struktura danych, która się wypełnia i ginie
przy restarcie. Użytkownik nie widzi błędu; widzi brak funkcji, o której komentarz mówi,
że jest.

**Koszt obecny:** `GasHistory` = 120 × 2 B + 4 + 2 = **246 B statycznego RAM-u**
+ `Settings::meters` = 8 × 8 B = **64 B**. Razem **~310 B** zajęte na dane, których nikt
nie ogląda.

**Proponowana poprawka:** decyzja produktowa, nie techniczna — albo dokończyć (para `uiGas`
+ `gasHistorySave/Load` + trasa `/api/meter` + wykres), albo usunąć. Usunięcie **zwalnia
~310 B statycznego RAM-u** (71 600 → ~71 290, zapas 4,4 kB → 4,7 kB).

**Ryzyko poprawki: niskie** w obie strony. **To ustalenie zahacza o zakres agentów C i D** —
zgłaszam je tutaj, bo szukałem zapomnianych par `g*`/`ui*` i to jest właśnie ta jedna
zapomniana.

---

## W-5. Stan Viessmanna (`gAccess`, `gVerifier`, `gCircuitTarget`) dzielony przez `webTask` i `netTask` bez żadnej blokady

**Plik:** `Viessmann.cpp:22-25` (`gVerifier[65]`, `gAccess[1400]`, `gErr[56]`),
`Viessmann.cpp:296` (`gCircuitTarget`)

Z `netTask`: `vi::fetch()` (`.ino:373`, co 3 min) → `ensureAccess()` → `storeTokens()` →
`snprintf(gAccess, 1400, ...)` (`Viessmann.cpp:154`).
Z `webTask`: `vi::authUrl()` (`Portal.cpp:867`) → `makeVerifier()` pisze `gVerifier`;
`vi::exchangeCode()` (`Portal.cpp:887`) → `storeTokens()` **też pisze `gAccess`**;
`vi::setCircuitTemp()` (`Portal.cpp:920`) i `vi::forget()` (`Portal.cpp:932`) → czyta/czyści
`gAccess`; `vi::daysLeft()` (`Portal.cpp:902`, panel odpytuje co 30 s) → czyta
`settings().viAuthAt`.

**Kiedy dokładnie:** użytkownik przechodzi ścieżkę autoryzacji pieca (klika „Zapisz
i wygeneruj link", loguje się, przeglądarka wraca na `/vicare`) dokładnie wtedy, gdy
`netTask` odświeża token (co ~55 min) lub robi zwykły odczyt co 3 min. Wtedy oba zadania
piszą/czytają ten sam bufor 1400 B.

**Skutek dla użytkownika:** rozdarty token → `apiGet` wysyła `Authorization: Bearer <pół
starego, pół nowego>` → HTTP 401 → ekran pieca mówi „nie odpowiada", a log pokazuje błąd
autoryzacji, choć token jest poprawny. Gorszy wariant: `exchangeCode` (webTask) i
`storeTokens` (netTask) piszą `settings().viRefresh` (600 B) jednocześnie → w NVS ląduje
sklejka dwóch tokenów → **refresh token nie do odzyskania, użytkownik musi autoryzować
piec od nowa**. Crashu nie ma (bufory statyczne, zawsze zakończone `\0`).

Dodatkowo: obie ścieżki wołają `settings().viSave()` → **to samo pudło co K-1**
(współdzielony `prefs`).

**Prawdopodobieństwo — uczciwie:** niskie. Wymaga, żeby użytkownik był w trakcie
autoryzacji pieca (rzadka, jednorazowa czynność) w tej samej sekundzie, w której netTask
odświeża token. Zgłaszam, bo skutek (utrata refresh tokena) jest trwały i nieoczywisty.

**Proponowana poprawka:** mutex `vi::gMx` obejmujący `ensureAccess`/`storeTokens`/`forget`/
`setCircuitTemp`. Uwaga: **nie wolno** go trzymać podczas `postToken`/`apiGet` (to operacje
sieciowe po TLS, do 25 s) — blokada tylko wokół dotknięć `gAccess`/`viRefresh`, sieć poza nią.

**Koszt RAM: +4 B statycznie** (uchwyt; obiekt mutexa ~80 B na stercie).

**Ryzyko poprawki: średnie** — łatwo przez pomyłkę objąć mutexem także `http.GET()`
i zablokować `netTask` na 25 s. Granica blokady musi być tu wyznaczona precyzyjnie.

---

# DROBNE

## D-1. `radarmap::setDemo()` sprawdza `gFrames[0]`, a pisze do `gFrames[i]` — potencjalny zapis pod NULL

**Plik:** `RadarMap.cpp:360` vs `RadarMap.cpp:370-394`

```cpp
if (gFrames[0] == nullptr) return;        // :360 — sprawdza TYLKO klatkę 0
...
for (int i = 0; i < FRAMES; ++i) { ... gFrames[i][y * W + x] = lv; }   // :388 — pisze do WSZYSTKICH
```

`begin()` (`RadarMap.cpp:173-179`) alokuje 7 klatek w pętli i przy pierwszej porażce
**wychodzi, zostawiając część wskaźników nie-NULL, a resztę NULL**. Jeśli `ps_calloc`
padnie np. przy `i == 3`, to `gFrames[0]` jest ważne, a `gFrames[3..6]` to NULL — i wtedy
kliknięcie „Włącz symulację" w panelu wpisuje pod adres 0 → **panic**. To samo dotyczy
`fetch()` (`RadarMap.cpp:280`, `resample(gFrames[i])`), które sprawdza tylko `gTile`.

**Prawdopodobieństwo — uczciwie: bardzo niskie.** 7 × 38 528 B = ~270 kB przy 2 MB PSRAM.
Żeby to trafić, PSRAM musiałby być obecny (inaczej `begin()` wychodzi wcześniej na
`!psramFound()`, `RadarMap.cpp:168`) i jednocześnie prawie pełny w chwili `setup()`.
Dziś to się nie zdarzy. Zgłaszam jako niespójność strażnika, nie jako realne zagrożenie.

**Poprawka:** w `begin()` przy porażce wyzerować wszystkie `gFrames[]` (albo ustawić flagę
`gReady`), a w `setDemo()`/`fetch()` sprawdzać ją zamiast `gFrames[0]`.
**Koszt RAM: +1 B.** **Ryzyko: znikome.**

## D-2. `pngLine()` nie sprawdza `d->y` — zaufanie do rozmiaru obrazka z sieci

**Plik:** `RadarMap.cpp:83-101`

```cpp
uint8_t* row = gTile + d->y * kTilePx;              // :90 — brak kontroli d->y < kTilePx
for (int x = 0; x < d->iWidth && x < kTilePx; ++x)  // :91 — szerokość JEST pilnowana
```

Szerokość jest ograniczona (`x < kTilePx`), wysokość **nie**. Gdyby RainViewer zwrócił
kafelek wyższy niż 256 px, dekoder pisałby poza `gTile` (65 536 B w PSRAM) → cicha
korupcja sterty PSRAM. Adres zawiera `/256/` (`RadarMap.cpp:244`), więc dziś przychodzi
256×256 i problemu nie ma — ale to jest zaufanie do zdalnego serwera, nie kontrakt lokalny.
(`RadarClient.cpp:57-59` robi to **dobrze**: sprawdza `draw->y` przed użyciem.)

**Poprawka:** `if (d->y < 0 || d->y >= kTilePx) return 1;` na wejściu `pngLine`.
**Koszt RAM: 0 B.** **Ryzyko: znikome.**

## D-3. `logDump()` buduje `String` w sekcji krytycznej

**Plik:** `Log.cpp:58-72`

`portENTER_CRITICAL(&gMux)` wyłącza przerwania na rdzeniu, a wewnątrz leci do 3072 iteracji
`out += gBuf[i]`. Realokacji **nie ma** (`out.reserve(kSize + 64)` w linii 60 jest zrobione
przed sekcją i pojemność wystarcza), więc **to nie jest błąd** — ale przerwania na rdzeniu 0
są wyłączone na ~50-100 µs przy każdym `GET /api/log`. Do zaakceptowania. Zgłaszam tylko
dlatego, że gdyby ktoś kiedyś usunął `reserve()`, zamieni się to w `malloc()` w sekcji
krytycznej, czyli w twarde zawieszenie.

**Poprawka:** komentarz ostrzegawczy przy `reserve()`. **Koszt RAM: 0 B.**

---

# Sprawdzone i CZYSTE (żeby nie sprawdzać drugi raz)

Rzeczy, o które pytało zlecenie i które **nie są błędami**:

**Arytmetyka `millis()`** — przejrzałem wszystkie porównania w repo. Wzorzec
`(int32_t)(millis() - deadline) >= 0` jest stosowany **konsekwentnie i poprawnie**:
`.ino:232, 260, 322, 359, 371, 407, 416, 438, 456, 502, 513, 534, 822`,
`PvClient.cpp:20`, `Viessmann.cpp:166`. Odejmowania `millis() - x` na `uint32_t` też są
poprawne (`.ino:153, 328, 726, 729, 866`, `WeatherUi.cpp:1359, 1368, 1374`).

Są **dwa** wystąpienia wzorca `millis() > stała`, który zlecenie słusznie kazało sprawdzić:

* `pogoda-gdynia.ino:544` — `if (gBooting && millis() > 35000)`. **Nie jest błędem.**
  Warunek odpala się raz, w 35. sekundzie po starcie, i ustawia `gBooting = false`
  na stałe. Po przekręceniu licznika (49 dni) `gBooting` jest już dawno `false`, więc
  gałąź jest martwa. Ryzyka po 49 dniach **nie ma**.
* `OtaGuard.cpp:206` — `if (force || millis() > TRIAL_TIMEOUT_MS)`. **Nie jest błędem.**
  `evaluate()` wychodzi na wejściu, gdy `gState != TRIAL || gDone` (`OtaGuard.cpp:168`),
  a okres próbny kończy się w ciągu pierwszych 3 minut po starcie i `gDone` zostaje `true`
  do restartu. Po 49 dniach ta linia jest nieosiągalna.

Oba przetrwają przekręcenie licznika **przypadkiem, nie z projektu** — działają dlatego,
że mierzą „czas od startu" w oknie, w którym `millis()` na pewno się jeszcze nie przekręcił.
Zostawiłbym je, ale warto dopisać komentarz „liczone od startu, przed pierwszym
przekręceniem" — inaczej ktoś kiedyś skopiuje ten wzorzec tam, gdzie zaboli.

**`diag().stackNet` / `diag().stackWeb`** (`pogoda-gdynia.ino:494-499`) — **raportują
poprawnie, w bajtach**:
```cpp
diag().stackNet = uxTaskGetStackHighWaterMark(gNetTask) * sizeof(StackType_t);
```
Zweryfikowałem w dokumentacji ESP-IDF v5.5 dla ESP32-S3: `uxTaskGetStackHighWaterMark()`
zwraca **słowa** („in words, so on a 32 bit machine a value of 1 means 4 bytes"), więc
mnożenie przez `sizeof(StackType_t)` = 4 daje bajty. Poprawnie. (Dla porządku: ta sama
dokumentacja **jawnie** odnotowuje odstępstwo IDF przy `xTaskCreate` — „usStackDepth: the
size of the task stack specified as the NUMBER OF BYTES. Note that this differs from
vanilla FreeRTOS" — więc `16384` w `.ino:749-750` to faktycznie 16 kB, nie 64 kB. Też
poprawnie. Skoro dokumentacja odnotowuje odstępstwa tam, gdzie są, to milczenie przy
`HighWaterMark` znaczy „bez odstępstwa, słowa".)

**Kto to obserwuje: NIKT.** `stackNet`/`stackWeb` trafiają wyłącznie do `/api/diag` jako
`mem.stack_net_spare` / `mem.stack_web_spare` (`Portal.cpp:688-689`). Nie ma progu, nie ma
ostrzeżenia w logu, nie ma nic na ekranie statystyk. Pole istnieje po to, żeby przyciąć
2 × 16 kB (komentarz `.ino:82-84`), ale **nikt nigdy nie zobaczy, że zapas spadł do zera** —
zobaczy dopiero `Stack canary watchpoint triggered` i restart. **Sugestia:** dorzucić
w `netTask` warunek „jeśli `stackNet < 2048`, wpisz to do logu raz" — koszt **0 B
statycznie** (zmienna `static bool` w funkcji, 1 B w `.bss`), ryzyko znikome, a zamienia
niediagnozowalny restart w linijkę w `/api/log`.

**Czy 16 kB starcza dla `netTask`?** Nie umiem tego rozstrzygnąć z samego kodu — i mówię
to wprost. Największe lokalne w `netTask`: `RoomHistory snap` = **1736 B** (`.ino:439`),
`WeatherModel tmp` ≈ **1,2 kB** (`.ino:236`), `PvHistory snap` = **724 B** (`.ino:504`),
`FlightModel tmp` ≈ **412 B** (`.ino:514`), `vi::Model tmp` (`.ino:372`). Siedzą
w rozłącznych zakresach, więc GCC z `-O2` zwykle nakłada je na te same sloty — ale
**gwarancji nie ma**. Do tego dochodzi handshake TLS (mbedtls bierze głównie ze sterty,
ale ~1-2 kB stosu) i ArduinoJson. **To jest dokładnie pytanie, na które odpowiada
`stack_net_spare` w `/api/diag`** — wystarczy je odczytać po dobie pracy z włączonym
piecem i radarem. Do tego czasu każda liczba, którą bym tu podał, byłaby zgadywaniem.

**Alokacje PSRAM — sprawdzone, wszystkie z kontrolą NULL:**
`RadarMap.cpp:128-132` (`ps_malloc` + check), `RadarMap.cpp:174-178` (`ps_calloc` + check),
`RadarMap.cpp:180-184` (+ check), `RadarMap.cpp:255-261` (+ check),
`RadarClient.cpp:112-116` (+ check), `RadarClient.cpp:232-242` (+ check, z eleganckim
fallbackiem na SRAM), `Viessmann.cpp:55-58` (`ps_realloc`/`ps_malloc` + check),
`WeatherUi.cpp:223, 1784, 2694` (`createSprite() == nullptr` + check). **Ani jednego
`ps_malloc` bez sprawdzenia NULL.** `PsramAlloc` (`Viessmann.cpp:29-33`) zwraca NULL
z `allocate()` — ArduinoJson v7 to obsługuje (deserializacja zwróci `NoMemory`). Zwalnianie
pamięci z `ps_malloc` przez `free()` (`RadarMap.cpp:137, 270, 272`, `Viessmann.cpp:31, 44`)
jest **poprawne** na ESP32 — `heap_caps_free`/`free` działają na obu rodzajach pamięci.

**Przepełnienia buforów — sprawdzone, nie znalazłem ani jednego:**
* `strcpy` / `sprintf` / `strcat` / `gets`: **zero wystąpień w całym repo** (grep).
  Wszędzie `strncpy` z `sizeof(...) - 1` albo `snprintf`.
* `BleGateway.cpp:70-78` — `uint8_t raw[32]`, `hl` przycięte do `hl <= 64` → `hl/2 <= 32`.
  Mieści się **co do bajtu**. Poprawne, ale bez zapasu — warto dopisać `static_assert`
  albo komentarz, bo podniesienie limitu `hl` o 2 wywala stos.
* `BleSensors.cpp:144-176` — `plain[16]`, `ctLen` przycięte do `<= 16`, `3u + vlen > ctLen`
  sprawdzone. Poprawne.
* `BleSensors.cpp:41-47` (`hexDump`) — warunek `p + 3 < n` + `out[p] = '\0'`: poprawne.
* `PvClient.cpp:77-95` — `body[256]`, `bodyLen > sizeof(body)` sprawdzone przed odczytem,
  `bytes < count*2` i `bodyLen < 1 + bytes` sprawdzone przed indeksowaniem. Poprawne.
* `Log.cpp:47` — `line[strlen(line) - 1]`: `pre` z linii 31 zawsze > 0 (znacznik czasu),
  więc `line` nigdy nie jest puste. Poprawne.
* `Diag` (`Log.h:20-24, 44`) — `strncpy(diag().weatherErr, tmp.errorMsg, sizeof(...) - 1)`
  (`.ino:253, 312, 350, 526`): `strncpy` dopełnia zerami, ostatni bajt nigdy nie jest
  zapisywany i zostaje `\0` z inicjalizacji. Poprawne.

**Wycieki HTTPClient/WiFiClient — sprawdzone, nie znalazłem:** wszystkie ścieżki wyjścia
w `RadarClient.cpp:84-127`, `RadarMap.cpp:103-143`, `BleGateway.cpp:29-97`,
`Viessmann.cpp:118-269`, `Portal.cpp:456-523`, `WeatherClient.cpp:188-205` mają `http.end()`
(albo wychodzą przed `http.begin()`). `WiFiClient`/`WiFiClientSecure` są obiektami
lokalnymi — destruktor zamyka gniazdo. `MqttClient.cpp:268-279` zwalnia `gCli`/`gSock`
poprawnie i zeruje wskaźniki.

**Kolejność blokad / zakleszczenia:** nie znalazłem. Jedyne zagnieżdżenie to
`gLock` → `ble::gMx` (`.ino:422-446`), ścieżki odwrotnej nie ma.

**Zrzut ekranu vs SPI:** sprawdzone — `streamScreenshot` używa własnego sprite'a
i **nie dotyka magistrali SPI**, więc nie koliduje z `pushSprite()` z rdzenia 1.
Komentarz `.ino:718-721` mówi prawdę.

---

# Uwaga na marginesie: `gRadarWantMem` zamraża ekran i (po włączeniu PSRAM) nic nie daje

**Pliki:** `pogoda-gdynia.ino:322-336` vs `pogoda-gdynia.ino:764-772`

Sam protokół uzgodnienia jest **poprawny** (sprawdziłem wszystkie przeploty; `loop()`
sprawdza `gRadarWantMem` jako pierwszą rzecz w pętli, więc nie da się go ominąć). Ale:

```cpp
const bool needMem = ESP.getMaxAllocHeap() < 48000;   // .ino:324 — largest block w SRAM
if (needMem) { gRadarWantMem = true; ... }            // loop() oddaje bufor ekranu
```

Bufor ekranu po włączeniu PSRAM (v50) **mieszka w PSRAM** — `TFT_eSprite::createSprite()`
przy `psramFound()` alokuje przez `ps_calloc`. Czyli `ui.releaseBuffer(false)` zwalnia
**PSRAM**, a `ESP.getMaxAllocHeap()` mierzy **SRAM**. Oddanie bufora nie podnosi tej
liczby ani o bajt. Jednocześnie `RadarClient::fetch()` (`RadarClient.cpp:232`) i tak bierze
dekoder z PSRAM, więc bufora nie potrzebuje.

**Skutek dla użytkownika:** gdy największy spójny blok SRAM spadnie poniżej 48 kB, ekran
**zamiera na czas całego pobrania radaru** (`loop()` wraca na `return` w `.ino:764-771`) —
raz na 5 minut, na sekundy — **żeby zwolnić pamięć, która i tak nie jest tą, której brakuje**.

Nie zgłaszam tego jako błędu w moim zakresie (to raczej materiał dla agenta D —
over-engineering), ale skoro cały ten mechanizm dotyczy pamięci: **dziś jest to czysty
koszt bez korzyści**. Usunięcie zwalnia 3 zmienne `volatile bool` (**−3 B statycznie**)
i likwiduje zamieranie ekranu. **Ryzyko usunięcia: niskie** — pod warunkiem, że
potwierdzimy w `/api/diag` (`mem.psram_free`), że sprite faktycznie siedzi w PSRAM.
**Tego nie zweryfikowałem na urządzeniu** — wnioskuję z kodu TFT_eSPI; przed usunięciem
warto to potwierdzić jednym odczytem `/api/diag`.

---

# Bilans RAM proponowanych poprawek

Budżet: **< 76 000 B**, teraz **71 600 B**, zapas **4 400 B**.

| Poprawka | Δ RAM statycznego |
|---|---|
| K-1 lokalne `Preferences` zamiast globalnego | **−12 B** |
| K-2 flaga `gWifiCfgBusy` | +1 B |
| K-3 mutex skanu WiFi (uchwyt; obiekt ~80 B na stercie) | +4 B |
| W-1 wariant 1 (dokumentacja) | 0 B |
| W-3 naprawa `/api/diag` | 0 B |
| W-5 mutex Viessmanna (uchwyt) | +4 B |
| D-1 flaga `gReady` w RadarMap | +1 B |
| D-2 kontrola `d->y` | 0 B |
| ostrzeżenie o zapasie stosu w logu | +1 B |
| **Razem KRYTYCZNE + WAŻNE + DROBNE** | **−1 B** |
| *opcjonalnie:* usunięcie `gGas` + `meters` (W-4) | **−310 B** |
| *opcjonalnie:* usunięcie `gRadarWantMem` | **−3 B** |

Komplet poprawek mieści się w budżecie **z zapasem**, a przy okazji sprzątania (W-4)
zapas rośnie z 4 400 B do ~4 700 B. Sterta nie jest obciążana w sposób istotny: dwa nowe
mutexy to ~160 B na stercie, przy progach `HEAP_DANGER = 25 000` / `HEAP_WARN = 45 000`
to szum.

---

# Kolejność wdrożenia (sugestia dla triażu)

1. **K-1** — największy stosunek szkody do kosztu, poprawka zwalnia RAM, ryzyko niskie.
2. **W-3** — 5 minut roboty, a naprawia narzędzie, którym diagnozuje się resztę. Zrobić
   **przed** wszystkim innym, żeby mieć czym mierzyć.
3. **K-2** — realny, powtarzalny, myli użytkownika. Wymaga uwagi przy kasowaniu flagi.
4. **K-3** — crash, ale rzadki. Wariant „prośba do netTask" jest bezpieczniejszy niż mutex.
5. **W-5**, **D-1**, **D-2** — hardening.
6. **W-1**, **W-2** — udokumentować, nie naprawiać.
7. **W-4** — decyzja produktowa (dokończyć albo usunąć), nie techniczna.
