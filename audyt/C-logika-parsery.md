# Audyt C — błędy logiczne, parsery, kontrakty API

Zakres: co daje **złą liczbę na ekranie** albo **ciche milczenie**. Crashe, mutexy, stosy
i millis() overflow — poza zakresem (robi to audyt B).

Metoda: lektura kodu + dwie weryfikacje empiryczne (odpytanie Open-Meteo, odtworzenie
palety RainViewera w liczbach). Tam, gdzie nie dało się rozstrzygnąć bez urządzenia,
jest to napisane wprost.

---

## KŁAMIE — zła liczba pokazana jako prawda

### K1. Radar: poziomy 3, 4 i 5 są NIEOSIĄGALNE. „ULEWA" nie pokaże się nigdy
**`RadarClient.cpp:63`** oraz **`RadarMap.cpp:87`** — `PNG_RGB565_BIG_ENDIAN`

To jest najpoważniejsze ustalenie tego audytu i jedyne, które udało się **udowodnić
liczbowo, bez urządzenia**.

PNGdec przy fladze `PNG_RGB565_BIG_ENDIAN` robi dosłownie
`usPixel = __builtin_bswap16(usPixel)` przed zapisem do tablicy `uint16_t`
(źródło: `png.inl`, linie 279-280 i dziewięć innych miejsc; nagłówek: `PNG_RGB565_LITTLE_ENDIAN = 0`,
`PNG_RGB565_BIG_ENDIAN = 1`). ESP32 jest little-endian, więc odczyt `line[x]` jako `uint16_t`
oddaje wartość z **zamienionymi bajtami**. A kod zaraz potem rozbiera ją tak, jakby bajty
były na swoim miejscu:

```cpp
const uint16_t c = line[x];
const uint8_t r = ((c >> 11) & 0x1F) << 3;   // to NIE jest czerwony
const uint8_t g = ((c >> 5) & 0x3F) << 2;
const uint8_t b = (c & 0x1F) << 3;
```

Po zamianie bajtów w bitach 11-15 nie siedzi czerwony, tylko trzy młodsze bity zielonego
sklejone z dwoma starszymi niebieskiego. Kanały są przemieszane, a `levelFromRgba()` liczy
natężenie opadu ze śmieci.

**Konkretne wejście, które to łamie — cała paleta RainViewera.** Przepuściłem ją przez
dokładnie ten kod (`bswap16` → rozbiór jak wyżej → `levelFromRgba`):

| kolor kafelka | oczekiwany poziom | faktyczny | co kod „widzi" jako (r,g,b) |
|---|---|---|---|
| przezroczysty | 0 | 0 | (0,0,0) |
| beż (ślady) | 1 | **2** | (80,216,176) |
| jasnoniebieski | 1 | 1 | (248,204,48) |
| niebieski | 1 | 1 | (216,132,152) |
| granatowy | 2 | **1** | (80,64,72) |
| żółty | 3 | **1** | (224,28,248) |
| pomarańczowy | 4 | **1** | (0,28,232) |
| **czerwony (ULEWA)** | **5** | **2** | (160,92,192) |

Sześć z ośmiu przypadków źle. **Poziomy 3, 4 i 5 nie wychodzą z tej funkcji nigdy** —
niezależnie od tego, co pada. Że dwa niebieskie trafiają, jest przypadkiem.

**Co użytkownik widzi na ekranie:**
- ekran TERAZ: podczas ulewy pasek radaru mówi „deszcz" albo „mżawka";
- etykieta „ULEWA" i „bardzo silny" nie zapaliły się ani razu w historii urządzenia;
- ekran RADAR (animowana mapa): front burzowy w jednym kolorze zamiast gradientu,
  jądro ulewy nie do odróżnienia od krawędzi;
- `gRain = wet > 60` (RadarMap.cpp:306) liczy piksele > 0, więc sam próg „czy pada"
  działa — dlatego ekran radaru w ogóle się pokazuje i wszystko wygląda zdrowo.

**Dlaczego to przeżyło:** „w Gdyni po prostu nigdy nie leje aż tak" jest w pełni wiarygodnym
wyjaśnieniem braku ULEWY. Do tego **tryb symulacji (`setDemo`, RadarMap.cpp:351) generuje
poziomy 1-5 wprost, z pominięciem PNG** — więc demo pokazuje śliczny gradient i dowodzi,
że „paleta działa". To jest pułapka, która maskuje błąd, a nie go ujawnia.

**Poprawka:** `PNG_RGB565_LITTLE_ENDIAN` w obu miejscach. Kontrola tą samą metodą: wszystkie
8/8 przypadków wychodzi poprawnie. Jedno słowo w dwóch linijkach.

**Ryzyko poprawki:** znikome. Ale uwaga na skutek uboczny: po naprawie zaczną wreszcie
działać alerty o silnym opadzie i ekran radaru zmieni wygląd. Jeśli progi w `levelFromRgba`
były kiedykolwiek „dostrajane na oko" pod zepsute wartości — to dostrajanie trzeba cofnąć.

---

### K2. Piec: pusta odpowiedź z API pokazuje się jako komplet zer, oznaczonych jako świeże
**`Viessmann.cpp:416-459`**

```cpp
Model m{};
for (JsonObjectConst f : doc["data"].as<JsonArrayConst>()) { ... }
m.online = true;
m.valid = true;
m.okAt = millis();
out = m;
```

Pętla po `data` jest **jedynym miejscem, gdzie cokolwiek się wypełnia**, ale po niej nie ma
żadnej kontroli, czy trafiła choć raz. Gdy `data` jest puste, brakujące albo zmieni się
nazwa pola — pętla obraca się zero razy, `m` zostaje wyzerowany, a kod i tak melduje
`online = true, valid = true` ze świeżym `okAt`.

**Konkretne wejścia, które to łamią:**
1. **Rodzeństwo pułapki #1.** `apiGet()` (linia 246) sprawdza tylko `code != 200`.
   `apiPost()` sprawdza `inner` — `apiGet()` **nie**. Jeśli Viessmann odeśle na GET `/features`
   HTTP 200 z ciałem `{"statusCode":502,"message":"gateway offline"}` (a przy nieosiągalnej
   bramce to jest realny scenariusz — PyViCare broni się przed tym również na odczycie),
   to `deserializeJson` przejdzie, `doc["data"]` będzie null, `.as<JsonArrayConst>()` da
   pustą tablicę, pętla zero obrotów → komplet zer jako prawda.
2. Zmiana nazwy feature'a po stronie Viessmanna (np. `heating.gas.consumption.summary.dhw`
   → nowy układ `heating.gas.consumption.dhw` z tablicą `day[]`) — to samo, tylko częściowo.
3. `devices/0` przestaje być właściwym urządzeniem.

**Co użytkownik widzi:** ekran PIEC pokazuje **CWU 0,0°C, cel 0°C, zasilanie 0,0°C, palnik
off, modulacja 0%, gaz dziś 0,0 m3** — bez żadnego komunikatu błędu, z zieloną kropką
i świeżym czasem odczytu na ekranie STATYSTYKI. Wygląda dokładnie jak wychłodzony,
wyłączony piec. Zimą to jest komunikat „kotłownia stoi" postawiony na niczym.

Gorzej: `GasHistory::push(tmp.gasDhwM3 + tmp.gasHeatM3)` (`.ino:392`) też to łyka. Bierze
maksimum doby, więc jedno zero nie zepsuje slotu — ale `BurnerHistory::push` z
`modulationPct = 0, burnerActive = false` zapisze do profilu doby „palnik nie pracował"
w slocie, w którym pracował.

**Poprawka:**
1. W `apiGet()` — ta sama obrona co w `apiPost()`: po sparsowaniu sprawdzić
   `doc["statusCode"] | 0` i przy `>= 400` zwrócić błąd z `message`.
2. Liczyć trafienia w pętli i przy zerze zwrócić `false` z błędem „piec: brak danych
   w odpowiedzi" zamiast `m.online = true`. Minimalnie: wymagać, żeby przyszedł
   przynajmniej `heating.dhw.sensors.temperature.dhwCylinder`.

**Ryzyko:** niskie. **Czego nie umiem rozstrzygnąć bez urządzenia:** czy Viessmann realnie
odsyła 200 + `statusCode` w ciele także na GET (nie mam tokena, żeby to wywołać). Ale punkt 2
broni przed całą rodziną tych sytuacji niezależnie od odpowiedzi na to pytanie i kosztuje
cztery linijki — zrobiłbym go tak czy siak.

---

### K3. Falownik: połowa rejestrów nie jest liczona do progu awarii. „Pobór domu" = cała produkcja
**`PvClient.cpp:218-239`**

```cpp
int fails = 0;
if (readS32(32064, s32)) snap.powerDcW = s32; else ++fails;
if (readS32(32080, s32)) snap.powerAcW = s32; else ++fails;
if (readS32(37113, s32)) snap.gridPowerW = s32; else ++fails;
if (readU32(32106, u32)) snap.energyTotalKwh = ...; else ++fails;
if (readU32(32114, u32)) snap.energyTodayKwh = ...; else ++fails;
if (readS16(32016, 10, f)) snap.pvVoltageV = f;          // brak ++fails
if (readS16(32087, 10, f)) snap.inverterTempC = f;       // brak ++fails
if (readU16(32086, u16)) snap.efficiencyPct = ...;       // brak ++fails
if (readU16(32089, u16)) snap.statusCode = u16;          // brak ++fails
if (readU16(37100, u16)) snap.meterOk = (u16 == 1);      // brak ++fails
```

Dwa osobne problemy w jednym bloku:

**(a) Pięć ostatnich odczytów nie ma `else ++fails`.** Mogą paść wszystkie i próg `fails >= 3`
nawet nie drgnie. `snap` jest wyzerowany (`PvSnapshot snap{}`), więc porażka = wartość 0
podana jako pomiar. **Konkretnie: `statusCode` nie doszedł → zostaje 0 → `pvStatusLabel(0)`
→ „Czuwanie". `meterOk` nie doszedł → `false` → ekran melduje problem z licznikiem, którego
nie ma. `inverterTempC` → „0,0°C" zimą wygląda wiarygodnie.**

**(b) Nawet w liczonej piątce próg to `fails >= 3`, czyli DWIE porażki przechodzą jako
`online = true`.** Najgorszy przypadek jest w linii 242:

```cpp
snap.houseLoadW = snap.powerAcW - snap.gridPowerW;
```

**Konkretne wejście:** odczyt rejestru 37113 (licznik / bilans sieci) pada — timeout, chwilowa
desynchronizacja, licznik na innym Modbusie niż falownik. `gridPowerW` zostaje 0.
Wtedy `houseLoadW = powerAcW - 0 = powerAcW`.

**Co użytkownik widzi:** ekran PV, słoneczne południe, produkcja 5 kW → **„Pobór domu: 5000 W"**,
„Sieć: 0 W". Dom rzekomo zjada wszystko, co panele produkują. Ta sama liczba trafia do
`PvHistory::push(..., tmp.data.houseLoadW)` (`.ino:279`), czyli **wykres profilu dnia zapisuje
czerwoną krzywą poboru dokładnie pod żółtą krzywą produkcji, na stałe, do NVS**, i to samo
leci do Home Assistanta jako encja „Pobór domu". Rachunek się nie zgodzi, a wykres będzie
wyglądał na przekonujący.

**Poprawka:** dopisać `else ++fails` do pięciu brakujących; obniżyć próg do `fails >= 1`
albo — lepiej — nie liczyć `houseLoadW` i nie ustawiać `valid`, gdy którykolwiek z trzech
rejestrów mocy (32080, 37113, 32064) nie doszedł. Rozdzielić „nie mam odczytu" od „odczyt
wynosi zero" flagą per pole albo wartownikiem.

**Ryzyko:** średnie — zaostrzenie progu zwiększy liczbę cykli oznaczonych jako błąd.
To jest pożądane (dziś część z nich cicho kłamie), ale przy okazji może częściej zapalać
czerwoną kropkę o zmierzchu. Warto to wpiąć w istniejący mechanizm `asleep`.

---

### K4. Nieznany kod statusu falownika = „Praca"
**`PvData.h:114-115`** — `default: return "Praca";`

Każdy kod spoza listy — w tym każdy przyszły kod awarii, każdy kod z nowszego firmware'u
SUN2000, i **0, gdy odczyt padł** (patrz K3a) — jest przedstawiany jako normalna praca.
Do tego `pvStatusIsFault()` (linia 120) uznaje za awarię **wyłącznie** `0x0300`, więc
`buildAlert()` (`.ino:567`) nie zaalarmuje o żadnym innym stanie awaryjnym.

**Co użytkownik widzi:** „Status: Praca" przy falowniku, który zgłasza kod błędu.
Alert „Awaria falownika" nie zapali się.

**Poprawka:** `default` → `"Kod 0x%04X"` (uczciwe „nie wiem"). Rozważyć uznanie całej
rodziny `0x03xx` za stan nienormalny.
**Ryzyko:** niskie.

---

### K5. Samoloty: obcięcie listy PRZED sortowaniem — pokazane nie są najbliższe
**`FlightClient.cpp:171` i `:214`**

```cpp
for (JsonObjectConst a : arr) {
  if (n >= kMaxCandidates) break;      // kMaxCandidates = 18
  ...
}
out.total = n;
// ...sortowanie po priorytecie i odległości...
const int take = (n < FLIGHT_MAX) ? n : FLIGHT_MAX;   // FLIGHT_MAX = 6
```

adsb.fi zwraca samoloty w kolejności **swojej**, nie po odległości. Kod bierze **pierwszych
18 z odpowiedzi**, dopiero potem sortuje po odległości i pokazuje 6.

**Konkretne wejście:** ruchliwe popołudnie, w promieniu 40 Mm jest 45 samolotów. Kod widzi
18 przypadkowych (te, które akurat były wcześniej w JSON-ie), sortuje je i pokazuje 6
„najbliższych" — z osiemnastu, nie z czterdziestu pięciu. Samolot przelatujący nad domem
może być w odpowiedzi na pozycji 25 i **nie pojawi się wcale**.

Drugi błąd w tej samej linii: `out.total = n` jest **liczbą kandydatów po obcięciu i po
filtrach**, a nie liczbą samolotów w zasięgu.

**Co użytkownik widzi:** ekran SAMOLOTY z nagłówkiem „18 w zasięgu" przy czterdziestu pięciu
(liczba zatrzymuje się na 18 i nigdy nie idzie wyżej — to jest sygnatura tego błędu, widać
ją też w `diag().flightsTotal` i w logu „Loty: 18 w zasiegu"). Lista „najbliższych" bywa
losowa.

**Poprawka:** liczyć `total` w osobnym liczniku po przejściu **całej** tablicy, bez `break`.
Zamiast obcinać — trzymać tablicę 18 najlepszych metodą wstawiania (dla nowego kandydata
bliższego niż najgorszy z bieżących: podmienić), co i tak jest O(n·18) przy n≈50, czyli darmo.

**Ryzyko:** niskie.

---

### K6. MQTT: nocą „Pobór domu" = 0 W w Home Assistancie
**`MqttClient.cpp:537-546`**

Komentarz przy `gPvCache` (linia 52) rozwiązał problem liczników energii — słusznie, bo
zjazd `total_increasing` do zera HA zinterpretowałby jako reset. Ale **moce chwilowe lecą
jako twarde zera**:

```cpp
ok ? static_cast<long>(d.houseLoadW) : 0L
```

**Konkretne wejście:** noc, falownik śpi (`asleep = true`, stan całkowicie normalny —
`PvClient.cpp:203-209`). `publishPv(tmp, false)` publikuje `house: 0`.

**Co użytkownik widzi:** w Home Assistancie encja „Pobór domu" pokazuje **0 W przez całą noc**,
a wykres dobowy ma płaskie dno od zmierzchu do świtu. Dom w nocy pobiera prąd — to jest
wprost nieprawda, i to na wykresie, na który patrzy się w kontekście rachunku. To samo
dotyczy encji „Bilans sieci" (nocą realnie ujemny, publikowany jako 0).

Dodatkowo `status: "Offline"` nie odróżnia **śpi** od **zepsuty** — mimo że `tmp.asleep`
istnieje i jest tuż obok. Reszta firmware'u tę różnicę robi starannie (szara kropka zamiast
czerwonej, `diag().pvAsleep`) — MQTT ją gubi.

**Poprawka:** przy `!ok` **nie publikować** mocy chwilowych (HA zostawi ostatnią znaną wartość
z retained) albo publikować je jako `null`/`unavailable`. `status` przy `asleep` → „Uśpiony".
Sensowniej: `avty_t` już istnieje — użyć dostępności zamiast wysyłać fałszywe zera.

**Ryzyko:** niskie. Uwaga: encje mocy mają `state_class: measurement`, więc `unavailable`
jest dla nich bezpieczne (inaczej niż dla `total_increasing`).

---

### K7. Mapa opadów: warunek na liczbę klatek pilnuje 7, a kod potrzebuje 13
**`RadarMap.cpp:225` vs `:235`**

```cpp
const int n = past.size();
if (n < FRAMES) {                                  // FRAMES = 7
  snprintf(gErr, sizeof(gErr), "tylko %d klatek", n);
  return false;
}
...
for (int i = 0; i < FRAMES; ++i) {
  const int idx = n - 1 - (FRAMES - 1 - i) * 2;    // dla i=0 → idx = n - 13
  if (idx < 0) continue;
```

Bierzemy co drugą klatkę z siedmiu, licząc wstecz — więc potrzeba **13** klatek w `past`,
nie 7. Strażnik przepuszcza `n` w przedziale 7-12.

**Konkretne wejście:** RainViewer po awarii/restarcie odbudowuje historię i przez kilkanaście
minut oddaje np. 9 klatek zamiast 13. Wtedy dla `i = 0` i `i = 1` wychodzi `idx < 0` → `continue`
→ **`gFrames[0]` i `gFrames[1]` zachowują obrazki z POPRZEDNIEGO cyklu (sprzed 10 minut),
razem ze starym `gMeta[i].epoch`**, a `gCount = FRAMES` i `return true` mówią „komplet gotowy".

Ten sam mechanizm odpala się przy zwykłej porażce pobrania pojedynczej klatki (linie 249-252:
`continue`, klatka zostaje stara), a `ok` liczy tylko sukcesy i służy wyłącznie do sprawdzenia
`ok == 0`.

**Co użytkownik widzi:** ekran RADAR animuje 7 klatek, z których pierwsze dwie są sprzed
2,5 godziny zamiast sprzed 2 godzin — **front „przeskakuje" albo cofa się** na początku pętli.
Podpis czasu pod klatką jest wzięty ze starego `gMeta`, więc jest formalnie prawdziwy, ale
animacja przestaje być ciągła. To dokładnie efekt „koła wozu" opisany w komentarzu przy
`setDemo` (linia 365) — tam rozpoznany i naprawiony, tutaj wraca tylnymi drzwiami.

**Poprawka:** `if (n < 2 * FRAMES - 1)` (czyli 13). Przy porażce klatki — `gMeta[i].valid = false`
zamiast zostawiać starą, i pomijać nieważne przy rysowaniu.
**Ryzyko:** niskie. Skutek: przy słabym RainViewerze ekran radaru pokaże uczciwy błąd
zamiast mylącej animacji.

---

## MILCZY — cicha porażka

### M1. Pętla `k < 4` przy ośmiu slotach — naprawiona w DWÓCH miejscach z PIĘCIU
**`MqttClient.cpp:217`, `:493`, `:502`**

Ten sam błąd, który znalazłeś. Poprawka trafiła do `.ino:432` (`k < RoomHistory::ROOMS`,
z komentarzem) i do `WeatherUi.cpp:2194-2204`. **W `MqttClient.cpp` została nietknięta,
w trzech miejscach:**

```cpp
int sendBleDiscovery() {
  for (int i = 0; i < 4; ++i) {                    // :217 — Settings ma ble[8]
    const Settings::BleCfg& c = settings().ble[i];
...
void publishBle() {
  for (int i = 0; i < ble::count() && i < 4; ++i) {  // :493
...
    for (int k = 0; k < 4; ++k) {                    // :502
      if (&settings().ble[k] == cfg) slot = k;
```

**Konkretne wejście:** użytkownik konfiguruje piąty czujnik w panelu. Panel odpowiada
„Zapisano. Dane pojawią się po najbliższym nasłuchu (do 45 s)."

**Co użytkownik widzi:** na ekranie W DOMU czujnik jest (bo `WeatherUi` naprawione),
na wykresie 24 h jest (bo `.ino` naprawione), **a w Home Assistancie go nie ma i nigdy nie
będzie** — `sendBleDiscovery` nie utworzy dla niego encji, `publishBle` nie wyśle wartości,
`slot` zostanie `-1` i `continue` wyrzuci go z payloadu. Zero śladu w logu. Częściowa naprawa
jest tu gorsza od żadnej: czujnik widać na ekranie, więc nikt nie podejrzewa, że w HA go brakuje.

Linia 493 ma **dodatkowy, osobny błąd**: `i` indeksuje `ble::` (kolejność **usłyszenia**),
a nie Settings. Warunek `i < 4` obcina **pierwsze cztery usłyszane urządzenia** — a wśród
nich mogą być obce nadajniki z bloku (patrz M2). Jeśli sąsiad ma dwa czujniki ATC i wpadną
na pozycje 0-1, do HA trafią co najwyżej dwa Twoje, mimo że skonfigurowane są cztery.

**Poprawka:** `Settings::BLE_SLOTS` w :217 i :502; w :493 pętla do `ble::count()` bez limitu
(filtr `bleFind() == nullptr` i tak odsiewa obcych), z kontrolą miejsca w buforze — ale
uwaga, `p[224]` nie pomieści ośmiu czujników po ~40 B. Trzeba albo powiększyć bufor, albo
wysyłać w kilku pakietach. **To jest realne ograniczenie, nie sama pętla** — dlatego
podmiana samej czwórki na `BLE_SLOTS` bez ruszenia bufora zamieni cichy brak encji na cichy
brak części payloadu. Zrobić razem.

**Ryzyko:** średnie — dotyka bufora PubSubClient (512 B) i rozmiaru payloadu.

---

### M2. Osiem slotów BLE dzielonych z obcymi nadajnikami, bez eksmisji
**`BleSensors.cpp:31-39`**

```cpp
Sensor* slotFor(const char* mac) {
  for (int i = 0; i < gCount; ++i)
    if (strcmp(gSensors[i].mac, mac) == 0) return &gSensors[i];
  if (gCount >= MAX_SENSORS) return nullptr;      // <-- i cisza
  ...
}
```

`MAX_SENSORS = 8`, a `parse181A` **z założenia nie filtruje po adresie** (komentarz
w linii 28-30: firmware pvvx potrafi losować MAC, więc filtrujemy po formacie ramki).
Komentarz w `BleSensors.h:21` sam przyznaje: „obce nadajniki w bloku też potrafią zająć slot".
Tylko że **nie ma żadnej polityki eksmisji** — kto pierwszy, ten lepszy, na zawsze.

**Konkretne wejście:** blok mieszkalny, ośmiu sąsiadów z czujnikami Xiaomi/ATC/Qingping
w zasięgu (albo jeden sąsiad z kilkoma + firmware losujący MAC co restart — wtedy **jeden
czujnik zjada wiele slotów w czasie**). Tablica zapełnia się obcymi, zanim odezwie się Twój.
`slotFor` zwraca `nullptr`, ramka jest po cichu wyrzucana. Bramka Shelly (`feedRaw`) idzie
tą samą drogą, więc też nie pomoże.

**Co użytkownik widzi:** ekran W DOMU — **„Brak czujników / dodaj je w panelu urządzenia"**,
mimo że czujniki wiszą na ścianie i nadają. Albo, jeśli `ble::count() > 0`, ekran W DOMU
w ogóle znika z rotacji (`.ino` → `WeatherUi.cpp:385`, `:1379`, `:2631` pomijają widok przy
`ble::count() == 0`) — tu akurat nie zniknie, bo count to 8, tylko samych obcych. W panelu
`/api/ble` pokaże listę ośmiu nieznanych MAC-ów. Zero komunikatu o przepełnieniu.

**Poprawka:** przy pełnej tablicy eksmitować wpis, który (a) nie ma odpowiednika
w `settings().bleFind()` i (b) ma najstarszy `seenAt`. Skonfigurowane czujniki nie mogą
przegrywać z obcymi — nigdy. Do tego licznik odrzuconych do `diag()` i jedna linia w logu.

**Ryzyko:** niskie. **Czego nie umiem rozstrzygnąć bez urządzenia:** ilu obcych nadajników
0x181A/0xFE95/0xFDCD faktycznie słychać w tej lokalizacji. `/api/ble` odpowie na to od ręki —
jeśli lista bywa dłuższa niż 4-5 pozycji, ten błąd jest kwestią czasu, nie teorii.

---

### M3. Sloty 7-8 konfigurowalne, niewidoczne wszędzie. Komunikat mówi „maks. 4"
**`Settings.h:41` (`ble[8]`) vs `RoomHistory.h:22` (`ROOMS = 6`) vs `Portal.cpp:822`**

`RoomHistory` ma 6 pokoi, `WeatherUi.cpp:2179` ma 6 kolorów, `Settings` ma 8 slotów.
`WeatherUi.cpp:2200` uczciwie przyznaje: „Sloty 6-7 z Settings nie mają ani koloru, ani
miejsca w historii — pomijamy je świadomie zamiast wyjechać poza tablicę". Świadomie —
ale **milcząco**.

**Konkretne wejście:** użytkownik konfiguruje siódmy czujnik. `bleSet` znajduje wolny slot 6,
zapisuje do NVS, panel mówi „Zapisano".

**Co użytkownik widzi:** nic. Czujnik nie pojawia się na ekranie W DOMU, nie ma go na wykresie,
nie ma go w HA. Panel twierdzi, że jest skonfigurowany, `/api/ble` pokazuje jego odczyty.
To jest **dokładnie ten sam scenariusz co znaleziony przez Ciebie piąty czujnik**, tylko
przesunięty o dwa sloty.

Do tego `Portal.cpp:822` przy braku miejsca odpowiada **„Brak wolnego miejsca (maks. 4 czujniki)"** —
liczba nieaktualna od czasu podniesienia `BLE_SLOTS` do 8.

**Poprawka:** albo `BLE_SLOTS = RoomHistory::ROOMS = 6` (i `static_assert`, żeby nigdy więcej
się nie rozjechały), albo dołożyć dwa kolory i dwa pokoje w historii. Osobiście: zrównać do 6
i poprawić komunikat — 8 slotów, z których 2 są ślepe, to gorzej niż 6 uczciwych.
`static_assert(Settings::BLE_SLOTS == RoomHistory::ROOMS)` to jedna linijka, która zamyka
całą tę rodzinę błędów na zawsze.

**Ryzyko:** niskie. Uwaga: zmiana `ROOMS` zmienia `sizeof(RoomHistory)` → historia w NVS
przestanie pasować. Klucz `"rh2"` trzeba wtedy podbić na `"rh3"` — pułapka jest już opisana
w `Settings.cpp:291-294`, warto ją uszanować.

---

### M4. Nazwa czujnika trafia do JSON-a discovery bez ucieczki
**`MqttClient.cpp:182-185`** + brak walidacji w **`Portal.cpp:801-823`**

```cpp
int n = addf(p, cap, 0, "{\"~\":\"%s\",\"name\":\"%s\",...", s.mqttPrefix, e.name, ...);
```

`e.name` powstaje w `sendBleDiscovery:241` z `c.name` — czyli wprost z pola w panelu WWW.
`apiBleSet` sprawdza tylko długość klucza; nazwy nie sprawdza wcale.

**Konkretne wejście:** użytkownik nazywa czujnik `Łazienka "góra"` albo wkleja nazwę
z ukośnikiem odwrotnym.

**Co użytkownik widzi:** payload discovery przestaje być poprawnym JSON-em, **Home Assistant
po cichu odrzuca encję** (nie loguje tego głośno), a urządzenie melduje w logu
`MQTT: discovery 26/22 encji` — czyli sukces. Czujnik jest na ekranie, nie ma go w HA,
przyczyna jest w cudzysłowie w nazwie. To potrafi zjeść wieczór.

To samo dotyczy `s.mqttPrefix` (linia 185) i wszystkich pól ze znakiem `"` lub `\`.

**Poprawka:** albo funkcja ucieczki przy budowaniu payloadu, albo — prościej i pewniej —
odrzucać w `apiBleSet` nazwy zawierające `"` i `\` z czytelnym komunikatem.
**Ryzyko:** niskie.

---

### M5. Nieudane zapytanie o trasę zapamiętuje się jako „trasa nieznana"
**`FlightClient.cpp:99-101`**

```cpp
if (!httpGetJson(url, doc, &filter)) {
  return true;  // 404 / brak danych — cache'ujemy jako nieznane
}
```

Komentarz mówi „404 / brak danych", ale `httpGetJson` zwraca `false` **także** przy timeoucie,
błędzie TLS, DNS i HTTP 5xx. Wszystkie te przypadki lądują w cache jako `known = false`.

**Konkretne wejście:** chwilowy problem z `vrs-standing-data.adsb.lol` (albo zwykły timeout
przy zajętym WiFi) podczas jednego cyklu.

**Co użytkownik widzi:** rejsy, które akurat trafiły na tę chwilę, **na stałe tracą trasę** —
lista pokazuje sam znak wywoławczy zamiast „WAW-GDN", i to aż do wykręcenia wpisu z pierścienia
cache. Gorzej: `cand[i].prio` spada z 0/1 na 2, więc samolot **lecący do Gdańska przestaje mieć
priorytet** i wypada z sześciu pokazywanych na rzecz przypadkowego GA. Cisza w logu.

**Poprawka:** rozróżnić „404 = na pewno nie ma trasy" (cache'uj) od „nie udało się zapytać"
(nie cache'uj, spróbuj w następnym cyklu). `httpGetJson` musi oddawać kod HTTP, a nie `bool`.
**Ryzyko:** niskie. Efekt uboczny: przy padniętym API zużyjemy budżet 4 zapytań/cykl w kółko —
warto dołożyć krótką karencję.

---

### M6. Bramka BLE liczy jako „przyjęte" ramki, których nie dało się odszyfrować
**`BleGateway.cpp:83-86`** + **`BleSensors.cpp:117-132`**

`parseMiBeacon` zwraca `true` również wtedy, gdy **nie ma bindkeya** (ścieżka „zapisujemy sam
fakt istnienia") i gdy **klucz jest zły** (linia 158-169, `rc != 0`). `feedRaw` przekazuje to
dalej, a bramka liczy: `if (fed) ++n;`.

**Co użytkownik widzi:** ekran STATYSTYKI, pozycja „Bramka" — zielona kropka i
„5 ramek przyjętych", podczas gdy **żaden odczyt się nie zaktualizował**. Log „Bramka BLE:
5 ramek przyjetych" mówi to samo. Liczba jest prawdziwa (tyle ramek przyszło) i myląca
(sugeruje, że coś z nich wynikło).

Druga rzecz: bramka rozdziela tylko `fdcd` → Qingping, **wszystko inne → MiBeacon**
(`BleGateway.cpp:83-85`). Jeśli Shelly prześle ramkę `181a` (czujnik pvvx poza zasięgiem
naszego radia — czyli **dokładnie ten, dla którego bramkę postawiono**), poleci ona do
`parseMiBeacon`, które odrzuci ją na `!encrypted` i zwróci `false`. Cicho, bez logu.

**Poprawka:** `feedRaw` niech zwraca „czy odczyt się zaktualizował", a nie „czy ramkę
rozpoznano"; rozdzielić licznik ramek od licznika odczytów. Dodać gałąź `181a` →
`parse181A` w `BleGateway`.
**Ryzyko:** niskie.

---

### M7. Licznik dni do wygaśnięcia autoryzacji pieca może odliczać od złej daty
**`Viessmann.cpp:157-162`** i **`:337-344`**

```cpp
if (ref[0] != '\0' && strcmp(ref, settings().viRefresh) != 0) {
  ...
  settings().viAuthAt = static_cast<uint32_t>(time(nullptr));
```

`viAuthAt` odświeża się **tylko wtedy, gdy Viessmann przyśle INNY refresh token**. Jeśli
przy odświeżaniu odeśle ten sam (a rotacja tokena to polityka serwera, nie gwarancja
protokołu) — `viAuthAt` zostaje na dacie pierwszej autoryzacji i `daysLeft()` odlicza
180 dni od niej.

**Co użytkownik widzi:** ekran PIEC / panel pokazuje „0 dni" i sugeruje ponowną autoryzację,
podczas gdy token działa bez zarzutu. Albo odwrotnie — przy rotacji za każdym razem licznik
resetuje się do 180 i nigdy nie ostrzeże. `kRefreshTtlDays = 180` jest zresztą **założeniem**,
nie danymi z API.

**Poprawka:** aktualizować `viAuthAt` przy **każdym udanym** `postToken` (token, choćby ten sam,
właśnie potwierdził ważność). Traktować licznik jako orientacyjny — prawdziwym sygnałem jest
błąd odświeżenia, i to on powinien wołać o autoryzację.
**Ryzyko:** niskie. Uwaga na zapis do NVS: `storeTokens` woła `viSave()` tylko przy zmianie
tokena — bezwarunkowa aktualizacja `viAuthAt` przy każdym odświeżeniu (co ~55 min) oznaczałaby
zapis do NVS co godzinę. Zapisywać, gdy różnica przekroczy np. dobę.

---

## KRUCHE — zadziała do pierwszej zmiany po drugiej stronie

### R1. Radar: `getSize()` + `getStreamPtr()` — rodzeństwo pułapki #2, w dwóch plikach
**`RadarClient.cpp:107-118`** i **`RadarMap.cpp:123-134`**

```cpp
const int size = http.getSize();
if (size <= 0 || static_cast<size_t>(size) > kMaxPng) { http.end(); return false; }
...
Stream* s = http.getStreamPtr();
const int got = s->readBytes(*buf, size);
```

To jest **dokładnie ten wzorzec, który kosztował Cię miesiące** przy Viessmannie, tylko
schowany o krok dalej. Dwie warstwy tej samej miny:

1. `http.getSize()` zwraca `_size`, a przy `Transfer-Encoding: chunked` `_size` **wynosi -1**.
   Wtedy `size <= 0` → cichy `return false`, zanim ktokolwiek dotknie strumienia.
2. Gdyby serwer podał i `Content-Length`, i chunked (albo gdyby ktoś „naprawił" punkt 1
   przez podstawienie `kMaxPng`), `getStreamPtr()` oddaje **surowy strumień z nagłówkami
   porcji** — `readBytes` wciągnie `"1f4a\r\n\x89PNG..."` i `openRAM` odrzuci to jako zły PNG.

Dziś działa, bo RainViewer serwuje kafelki PNG z `Content-Length`. To jest **jedyny powód**.
Zmiana CDN-a po ich stronie, wejście proxy, HTTP/1.1 z kompresją — i radar milknie.

**Co użytkownik widzi:** ekran TERAZ — pasek radaru znika / „Radar: brak kafelka".
Ekran RADAR — „żadna klatka nie doszła". Bez żadnej wskazówki, że chodzi o kodowanie
transferu; komunikat będzie wskazywał na sieć.

`Viessmann.cpp:42-71` ma już gotowe rozwiązanie (`PsramSink` + `writeToStream`), z komentarzem
opisującym dokładnie tę pułapkę. **Ta wiedza nie została przeniesiona do dwóch pozostałych
klientów pobierających binaria.**

**Poprawka:** `writeToStream()` do bufora rosnącego (PSRAM — `RadarMap` już tam alokuje),
z twardym limitem `kMaxPng`. `PsramSink` jest gotowy do wyciągnięcia do wspólnego nagłówka.

**Ryzyko:** niskie, ale dotyka najbardziej pamięciożernej ścieżki w firmware — zrobić po K1,
osobno, i zmierzyć stertę.

**Przegląd pozostałych klientów HTTP (pod kątem pułapki #2):**

| plik | metoda | chunked-bezpieczne? |
|---|---|---|
| `WeatherClient.cpp:200` | `http.getString()` | **tak** — `getString()` woła wewnątrz `writeToStream()` |
| `FlightClient.cpp:57` | `http.getString()` | **tak** |
| `Viessmann.cpp:132, 205` | `http.getString()` | **tak** |
| `Viessmann.cpp:253` | `http.writeToStream()` | **tak** — naprawione świadomie |
| `BleGateway.cpp:54` | `http.getString()` | **tak** |
| `RadarClient.cpp:118` | `getStreamPtr()->readBytes()` | **NIE** |
| `RadarMap.cpp:134` | `getStreamPtr()->readBytes()` | **NIE** |

Czyli: pułapka #2 siedzi **wyłącznie** w dwóch klientach radaru. `getString()` jest bezpieczne
i nie trzeba go ruszać. (`Ota.cpp` nie był przedmiotem tego audytu.)

---

### R2. `statusCode` jako string obchodzi obronę przed pułapką #1
**`Viessmann.cpp:214`**

```cpp
const int inner = parsed ? (e["statusCode"] | 0) : 0;
```

Jeśli Viessmann kiedykolwiek odeśle `"statusCode": "400"` (string zamiast liczby),
ArduinoJson zwróci wartość domyślną `0` → `inner >= 400` nie zadziała → **komenda odrzucona
przez piec zostaje zameldowana jako sukces**. Czyli ten sam ból, przed którym broni ta linia.

**Co użytkownik widzi:** panel mówi „zapisano nastawę 45°C", piec nie drgnął.

**Poprawka:** `e["statusCode"].as<int>()` (konwertuje string na liczbę) albo jawne sprawdzenie
obu typów. Rozważyć też traktowanie **obecności** pola `statusCode` jako sygnału błędu —
przy sukcesie Viessmann go nie odsyła.
**Ryzyko:** znikome. **Czego nie umiem rozstrzygnąć bez urządzenia:** czy Viessmann to robi.
Poprawka kosztuje jedno słowo i domyka znany, bolesny wektor — zrobiłbym ją bez czekania na dowód.

---

### R3. Brakujące pole pieca = 0,0°C podane jako pomiar
**`Viessmann.cpp:298-300`**

```cpp
float propF(JsonObjectConst f, const char* prop) {
  return f["properties"][prop]["value"] | 0.0f;
}
```

Klasyczne „ArduinoJson po cichu daje 0". Viessmann oznacza niedostępne feature'y polami
`isEnabled` / `isReady` (a czujniki — statusem `notConnected`); kod nie patrzy na żadne z nich.

**Konkretne wejście:** czujnik CWU odłączony albo feature wyłączony w instalacji →
`properties.value` nie przychodzi lub przychodzi jako `null`.

**Co użytkownik widzi:** „CWU 0,0°C" — nieodróżnialne od zamarzniętego bojlera.
Ta sama pułapka dotyczy `gasDhwM3`, `heatDhwKwh`, `modulationPct` i `wifiRssi`.

**Poprawka:** `propF` niech zwraca `bool` + wartość przez referencję (albo NaN jako wartownik),
a wołający ustawia flagę `has*`. Wzorzec jest w projekcie sprawdzony — `ble::Sensor::hasTemp/hasHum`
powstało dokładnie po to i komentarz w `BleSensors.h:42-43` mówi wprost: „Bez tych flag
rysowaliśmy 0.0 C, dopóki nie doszła ramka z temperaturą — czyli kłamaliśmy". **Ta sama lekcja,
nieprzeniesiona na piec.**
**Ryzyko:** średnie — dotyka wszystkich pól ekranu PIEC.

---

### R4. Radar: `host` bez schematu albo z `http://`
**`RadarClient.cpp:191-194`** vs **`RadarMap.cpp:220-222`**

`RadarMap` obsługuje oba warianty:
```cpp
if (strncmp(host, "https://", 8) == 0) host += 8;
else if (strncmp(host, "http://", 7) == 0) host += 7;
```
`RadarClient` — **tylko `https://`**. Jeśli RainViewer wróci do `host` z `http://`
(albo bez schematu — tak było kiedyś, stąd pułapka #4), `RadarClient` zbuduje
`http://http://tilecache...` i punktowy radar umrze, podczas gdy mapa opadów będzie działać.
Rozjazd dwóch kopii tej samej logiki.

**Poprawka:** wyrównać do wersji z `RadarMap` — najlepiej jedna wspólna funkcja.
**Ryzyko:** znikome.

---

### R5. Odległość samolotu policzona na zaszytej szerokości Gdyni
**`FlightClient.cpp:34-38`**

```cpp
const float dLon = (lon - settings().lon) * 64.6f;
```

64,6 = 111,2 · cos(54,5°) — poprawne dla Gdyni i **tylko** dla Gdyni. `lat`/`lon` są tymczasem
konfigurowalne w panelu.

**Konkretne wejście:** ktoś (albo Ty, po przeprowadzce) ustawia lokalizację w Krakowie (50,06°N).
Poprawny współczynnik to 71,4 — błąd 10%.

**Co użytkownik widzi:** kolejność „najbliższych" samolotów lekko przekłamana. Mało dotkliwe,
ale to zaszyta stała udająca uniwersalną. `distKm` służy tylko do sortowania, więc skutek jest
kosmetyczny — zgłaszam dla porządku, nie do pilnej naprawy.

**Poprawka:** `111.2f * cosf(settings().lat * DEG2RAD)`, policzone raz.
**Ryzyko:** znikome.

---

### R6. `lastAlertAt[8]` indeksowane wartością enuma
**`pogoda-gdynia.ino:121`** i **`:863`**

```cpp
uint32_t lastAlertAt[8] = {};
...
const int idx = static_cast<int>(a.kind);
lastAlertAt[idx] = now;
```

Dziś `AlertKind` ma 7 wartości i mieści się. Dołożenie dwóch nowych rodzajów alertu = zapis
poza tablicę, bez ostrzeżenia kompilatora. To jest **ta sama klasa błędu co `k < 4`**: liczba
zaszyta obok rosnącego zbioru.

**Poprawka:** `AlertKind::COUNT` na końcu enuma i `lastAlertAt[static_cast<int>(AlertKind::COUNT)]`.
**Ryzyko:** znikome. Warto zrobić przy okazji, jako profilaktykę tej rodziny.

---

### R7. Maska alpha z progiem 1 — dwie gałęzie w `levelFromRgba` są martwe
**`RadarClient.cpp:64, 70`**, **`RadarMap.cpp:88, 97`**

```cpp
png->getAlphaMask(draw, alpha, 1);          // próg 1
const uint8_t a = (alpha[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
```

`getAlphaMask` z progiem 1 daje **bit**, nie wartość — więc `a` to zawsze 0 albo 255.
W `levelFromRgba` warunek `if (a < 20) return 0;` łapie tylko pełną przezroczystość,
a `return (a >= 60) ? 1 : 0;` **nie może już zwrócić 0** — końcowe rozróżnienie
„beż półprzezroczysty = mżawka vs. ślad" nie istnieje.

**Konkretne wejście:** piksel o alpha = 3 (ledwo widoczny artefakt na krawędzi echa radarowego).
Traktowany jak alpha = 255 → poziom 1 → „mżawka".

**Co użytkownik widzi:** „mżawka" przy suchym niebie z pojedynczymi artefaktami; w `RadarMap`
te piksele wpadają do `wet` i mogą przeważyć próg `gRain > 60`, **włączając ekran radaru,
gdy nie pada** — czyli dokładnie to, przed czym broni komentarz w linii 296-298.

Uwaga: **ten błąd jest zamaskowany przez K1** i ujawni się dopiero po jego naprawie.
Rozpatrywać razem.

**Poprawka:** próg ~20 w `getAlphaMask` (wtedy `a < 20` robi się nadmiarowe, ale nieszkodliwe)
albo pobierać rzeczywistą alfę. Prościej: podnieść próg i uprościć `levelFromRgba`.
**Ryzyko:** niskie. Kalibracja `gRain > 60` może wymagać korekty po zmianie — zmierzyć na urządzeniu.

---

### R8. Bramka Shelly: format MAC-a musi się zgadzać co do znaku
**`BleGateway.cpp:65`** → **`BleSensors.cpp:115`** → **`Settings.cpp:174`**

Klucz z JSON-a bramki idzie prosto do `bleFind()`, które robi `strcasecmp` z MAC-iem
wpisanym w panelu. Wielkość liter jest obsłużona (`strcasecmp`), ale **separatory nie**:
`"a4:c1:38:54:f9:a9"` ≠ `"a4c13854f9a9"`.

Własne radio podaje MAC z `BLEAdvertisedDevice::getAddress().toString()` — z dwukropkami.
Skrypt w Shellym może podawać jak chce.

**Czego nie umiem rozstrzygnąć bez urządzenia:** jaki format ma klucz w JSON-ie z Twojego
skryptu `/script/1/ble`. Jeśli bez dwukropków, to **bramka nie odszyfrowuje niczego** —
każda ramka idzie ścieżką „brak klucza" (`BleSensors.cpp:117-132`), która zwraca `true`,
więc bramka melduje „N ramek przyjętych" (patrz M6), a czujnik pokazuje się w panelu
z adnotacją „brak klucza" mimo poprawnie wpisanego bindkeya. Objaw myli: wygląda na problem
z kluczem, jest problemem z formatem adresu.

**Jak sprawdzić w 30 sekund:** `curl http://<shelly>/script/1/ble` i porównać klucze z tym,
co pokazuje `/api/ble` urządzenia.

**Poprawka (niezależnie od odpowiedzi):** normalizować MAC do jednego formatu przy wejściu
w `bleFind` i `bleSet` — usunąć separatory i porównywać 12 znaków hex. Odporne na wszystkie
warianty.
**Ryzyko:** niskie.

---

### R9. Bindkey nie jest walidowany jako hex
**`Portal.cpp:812`** + **`Settings.cpp:209-214`**

Panel sprawdza `strlen(key) != 32`, ale nie sprawdza, czy to hex. `bleSet` robi
`strtoul(b, nullptr, 16)` na parach — para spoza hex-a daje po cichu `0`.

**Konkretne wejście:** wklejenie 32 znaków z literówką albo ze znakiem spoza `[0-9a-f]`.
**Co użytkownik widzi:** „Zapisano", a potem czujnik z adnotacją „brak klucza" i w logu
`BLE: <mac> zly klucz albo uszkodzona ramka (mbedtls -XXXX)`. Ślad **jest** — to jedyny powód,
dla którego to nie jest cichą porażką. Ale komunikat w panelu kłamie.
**Poprawka:** walidacja `isxdigit` w `apiBleSet`.
**Ryzyko:** znikome.

---

## Sprawdzone i CZYSTE — nie szukaj tu

Żeby nikt nie tracił na to czasu w triażu:

- **Open-Meteo, `timeformat=unixtime` + `timezone=Europe/Warsaw` — POPRAWNE. Zweryfikowane
  odpytaniem API 15.07.2026.** To była moja główna hipoteza (dokumentacja Open-Meteo ostrzega
  „apply utc_offset_seconds again" i wygląda to na klasyczną minę). Sprawdziłem: `daily.time[0]`
  = 1784066400 = 14.07 22:00 UTC = **15.07 00:00 czasu lokalnego**, `sunrise[0]` = 1784082643 =
  lokalne 04:30 (ISO potwierdza: `"2026-07-15T04:30"`). Znaczniki są **prawdziwymi epokami UTC**,
  więc `localtime_r()` przy `TZ=CET-1CEST,M3.5.0,M10.5.0/3` daje poprawny czas lokalny.
  Kolejność w `connectWifi()` (`.ino:176-178`: `configTime(0,0,...)` → `setenv TZ` → `tzset()`)
  też jest poprawna. **Godziny, nazwy dni i wschód/zachód są dobrze. DST działa.**
- `pvMayBeAsleep()` (`WeatherData.h:101`) — przeliczyłem lato (zachód 21:13 → okno 21:43-05:00)
  i zimę (15:30 → 16:00-08:30), łącznie z gałęzią `start < end`. Logika przez północ poprawna.
- `RoomHistory::advance()` / `idx()` — arytmetyka slotów (`epoch/600`), przewijanie, gałąź
  cofniętego zegara i `gap >= SLOTS` — poprawne.
- `GasHistory` — mimo że doba to `epoch/86400` (UTC), a `currentDay` pieca zeruje się o lokalnej
  północy, branie **maksimum** w slocie sprawia, że pełna suma doby lokalnej i tak zostaje
  złapana. Przeanalizowałem przesunięcie 2 h w obie strony — nie gubi gazu.
  `sumBetween()` z warunkami brzegowymi — poprawne.
- `PvClient::readRegs` — weryfikacja Transaction ID i obrona przed desynchronizacją Modbusa:
  zrobione dobrze. Skalowanie rejestrów (32064/32080/37113 gain 1; 32106/32114 gain 100;
  32016/32087 gain 10; 32086 gain 100) zgodne z mapą SUN2000 — **żadnej pomyłki o 10x nie ma**.
  Znak `int16_t`/`int32_t` obsłużony poprawnie.
- `BleGateway` — kontrola długości hex (`hl < 12 || hl > 64 || (hl & 1)`) przed zapisem
  do `raw[32]`: dokładna, bez off-by-one.
- `parseMiBeacon` / `parseQingping` — kontrole długości **przed** odczytem pól, granice nonce
  i ciphertextu (`ctLen == 0 || ctLen > 16` przy `plain[16]`), `3u + vlen > ctLen`: poprawne.
- Rozpakowanie maski alpha w `RadarClient:70` i `RadarMap:97` — zapisane inaczej
  (`& (0x80 >> (x&7))` vs `>> (7 - (x&7)) & 1`), ale **równoważne**. Nie jest to błąd.
- `computeGeometry()` (`RadarMap.cpp:44`) — przeliczyłem: obszar 18,0-19,2°E / 54,30-54,84°N
  daje x∈[70.40, 70.83], y∈[40.59, 40.91] → mieści się w jednym kafelku z=7 (x=70, y=40),
  jak głosi komentarz. Poprawne (przy zmianie granic mapy — przeliczyć ponownie).
- `nowcast: []` (pułapka #3) — **nie dotyczy**: żaden z klientów radaru nie tyka `nowcast`,
  oba czytają wyłącznie `radar.past`.
- Filtr ArduinoJson na tablicy (`filter["radar"]["past"][0]`) — poprawny: wzorzec z indeksu 0
  stosuje się do wszystkich elementów.

---

## Podsumowanie — 5 najważniejszych ustaleń

1. **Radar nigdy nie pokaże ulewy.** `PNG_RGB565_BIG_ENDIAN` (`RadarClient.cpp:63`,
   `RadarMap.cpp:87`) sprawia, że PNGdec oddaje bajty zamienione, kod rozbiera je jakby były
   na miejscu, i **poziomy 3/4/5 są nieosiągalne** — czerwony piksel ulewy wychodzi jako
   „deszcz", żółty jako „mżawka" (udowodnione liczbowo na całej palecie, 6/8 przypadków źle);
   tryb demo generuje poziomy z pominięciem PNG, więc od lat dowodzi, że „paleta działa".
2. **Piec przy pustej odpowiedzi API melduje `online = true` i komplet zer** — ekran pokazuje
   CWU 0,0°C i gaz 0,0 m3 jako świeży pomiar (`Viessmann.cpp:456`), a `apiGet()` jako jedyne
   miejsce nie sprawdza `statusCode` w ciele, czyli pułapka #1 ma tam żywe rodzeństwo.
3. **Pułapka #2 (`chunked`) siedzi jeszcze w dwóch klientach radaru** — `getSize()` +
   `getStreamPtr()` (`RadarClient.cpp:107-118`, `RadarMap.cpp:123-134`) działa wyłącznie
   dlatego, że RainViewer dziś wysyła `Content-Length`; wszystkie pozostałe klienty używają
   `getString()` i są bezpieczne.
4. **Falownik: pięć z dziesięciu odczytów nie jest liczonych do progu awarii, a dwie porażki
   przechodzą jako „online"** (`PvClient.cpp:218-239`) — gdy padnie rejestr bilansu sieci,
   `houseLoadW = powerAcW - 0` i ekran (oraz zapisany do NVS wykres, oraz Home Assistant)
   pokazuje **pobór domu równy całej produkcji**.
5. **Błąd `k < 4` przy ośmiu slotach żyje dalej w `MqttClient.cpp` (linie 217, 493, 502)** —
   naprawiony w `.ino` i `WeatherUi`, pominięty w MQTT, więc piąty czujnik **widać na ekranie,
   ale nie ma go w Home Assistancie**; częściowa naprawa jest tu gorsza od żadnej, bo usuwa
   objaw, po którym dałoby się to zauważyć.

**Sugerowana kolejność:** 1 (jedno słowo, ogromny efekt — ale zaraz po nim R7, bo K1 go maskuje),
5 i 2 (ciche, dotyczą danych, na które ktoś patrzy), 4, 3.
