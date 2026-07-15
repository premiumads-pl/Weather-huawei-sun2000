# Audyt A — źródła prawdy

Data: 2026-07-15 · zakres: kod własny (bez `WeatherIcons.h`, `PlFont*.h`, `MapData*.h`)
Kryterium sortowania: **co realnie ugryzie i kiedy**, nie „co jest brzydkie”.

Legenda ryzyka poprawki: **niskie** = zmiana lokalna, bez wpływu na RAM/układ ·
**średnie** = dotyka kilku plików albo NVS · **wysokie** = zmienia zachowanie lub RAM.

---

## 1. `MqttClient.cpp` — zaszyta czwórka ŻYJE. To jest dokładnie ten sam błąd, który naprawiłeś dziś.

**Gdzie:**
- `MqttClient.cpp:217` — `for (int i = 0; i < 4; ++i)` w `sendBleDiscovery()`
- `MqttClient.cpp:493` — `for (int i = 0; i < ble::count() && i < 4; ++i)` w `publishBle()`
- `MqttClient.cpp:502` — `for (int k = 0; k < 4; ++k)` (szukanie slotu)

**Na czym polega:** naprawiłeś `WeatherUi.cpp` i `pogoda-gdynia.ino`, ale MQTT został z czwórką.
To trzecia kopia tej samej pętli „znajdź indeks `bc` w `settings().ble[]`” — i jedyna, która
nadal ma limit 4.

**Jaki błąd z tego wyniknie:** czujnik w slocie 5–8 skonfigurowany w panelu **nigdy nie dostanie
encji w Home Assistancie** (`sendBleDiscovery` go pomija) i **nigdy nie trafi do `pogoda-gdynia/ble/state`**
(`publishBle` go pomija). Bez żadnego komunikatu — dokładnie jak z ekranem przed dzisiejszą
naprawą. Użytkownik ma 2 czujniki i dokłada 2 (komentarz `BleSensors.h:21`), więc **piąty czujnik
odpala ten błąd**. Dziś ekran już go pokaże, a HA nadal nie — czyli objaw będzie jeszcze bardziej
mylący niż poprzednio („na ekranie jest, w HA go nie ma”).

**PUŁAPKA przy naiwnej poprawce — nie zmieniaj samej czwórki na 8.**
`publishBle()` buduje JSON w `char p[224]` (`MqttClient.cpp:489`). Jeden czujnik to ~44 B
(`"s0t":-12.3,"s0h":100.0,"s0b":100,"s0r":-100`). Cztery = ~180 B, mieści się. Osiem = ~350 B.
Zabezpieczenie `if (n >= sizeof(p) - 8) break;` (`:513`) stoi **na końcu iteracji**, a wewnątrz
iteracji lecą trzy `snprintf(p + n, sizeof(p) - n, ...)` bez sprawdzenia. `n` jest `int`,
`sizeof(p)` jest `size_t` — gdy `n` przekroczy 224, `sizeof(p) - n` **przekręca się na ogromny
`size_t`** i `snprintf` pisze poza bufor. Dziś jest to nieosiągalne **wyłącznie dzięki limitowi 4**.
Podniesienie limitu bez powiększenia bufora zamienia cichy brak encji w **przepełnienie stosu
netTask**.

**Poprawka:**
1. `p[224]` → `p[512]` (stos netTask, nie RAM statyczny; netTask ma 16 kB, zapas widać
   w `/api/diag` → `mem.stack_net_spare`).
2. Rzutować `sizeof(p) - n` na `int` albo dodać `if (n >= (int)sizeof(p) - 48) break;` przed
   każdą trójką `snprintf`.
3. Czwórki → jedna stała (patrz ustalenie 2).

**RAM statyczny: 0 B.** Bufor jest lokalny (stos).
**Ryzyko: niskie** (poprawka 1–2), **średnie** (3, bo dotyka kontraktu discovery — encje
`ble4_*`…`ble7_*` dojdą jako nowe retained configi; stare nie znikną, ale nic nie psują).

---

## 2. „Ile jest czujników BLE” — CZTERY różne odpowiedzi, jedna z nich kłamie użytkownikowi w twarz

| Gdzie | Wartość | Co naprawdę znaczy |
|---|---|---|
| `Settings.h:41` | `} ble[8];` | literał — **to jest realne źródło prawdy** |
| `Settings.h:68` | `static constexpr int BLE_SLOTS = 8;` | stała, która **nic nie definiuje**, stoi 27 linii niżej |
| `BleSensors.h:23` | `MAX_SENSORS = 8` | ile czujników umiemy *usłyszeć* (inna oś!) |
| `RoomHistory.h:22` | `ROOMS = 6` | ile trafia do historii i na ekran |
| `MqttClient.cpp:217,493,502` | `4` | ile trafia do HA |
| **`Portal.cpp:822`** | **„maks. 4 czujniki”** | **komunikat dla użytkownika — nieprawdziwy** |

**Jaki błąd z tego wyniknie — trzy różne, wszystkie ciche:**

**(a) `Portal.cpp:822` kłamie.** `bleSet()` zwraca `false` dopiero gdy zajętych jest **8** slotów
(`Settings.cpp:185-199`), ale komunikat brzmi „Brak wolnego miejsca (maks. 4 czujniki)”. Użytkownik
z pięcioma czujnikami zobaczy ten tekst **nigdy** (bo zapis się uda), za to gdyby kiedykolwiek go
zobaczył, byłby wprowadzony w błąd o 100%. To zdanie zostało po epoce czwórki i nikt go nie ruszył.

**(b) Sloty 6–7 są konfigurowalne, ale niewidzialne.** `pogoda-gdynia.ino:432` i
`WeatherUi.cpp:2203` iterują `k < RoomHistory::ROOMS` (6) po tablicy `settings().ble[]` (8).
Komentarze przyznają to wprost („Sloty 6-7 z Settings nie maja ani koloru, ani miejsca w historii —
pomijamy je swiadomie”). To **ta sama klasa błędu co czwórka**, tylko przesunięta o dwa: siódmy
i ósmy czujnik da się wpisać w panelu i **nigdy się nie pokaże, bez komunikatu**. „Świadomie”
w komentarzu nie jest widoczne dla użytkownika stojącego przed wyświetlaczem.

**(c) `BLE_SLOTS` to atrapa.** Zmiana `BLE_SLOTS = 8` → `12` **nie zmieni nic** — tablica ma
zaszyte `[8]`. Kod skompiluje się i będzie czytał `ble[8..11]` **poza tablicą** w
`Settings.cpp:58,173,185,192`. To jest gotowe do wybuchu.

**Poprawka (dwa warianty — polecam A):**

**A. Zero kosztu RAM — uczciwie przyznać, że limit to 6.**
```
Settings.h:41   } ble[BLE_SLOTS];          // stała PRZED tablicą, nie po niej
Settings.h:68   → przenieść wyżej, dodać:  static_assert(BLE_SLOTS >= RoomHistory::ROOMS);
Settings.cpp    bleSet(): odrzucać slot >= RoomHistory::ROOMS z jasnym komunikatem
Portal.cpp:822  „Brak wolnego miejsca (maks. 6 czujników)” — liczba z jednej stałej
MqttClient.cpp  4 → RoomHistory::ROOMS
```
Wtedy jedna prawda („6”) rządzi panelem, ekranem, historią i MQTT, a `ble[8]` zostaje jako zapas
w NVS, który nikogo nie okłamuje.
**RAM: 0 B.** **Ryzyko: niskie.**

**B. Podnieść historię do 8.**
`ROOMS: 6 → 8` daje `sizeof(RoomHistory)`: **1736 → 2312 B (+576 B)**. Instancji statycznych są
**trzy**: `gRooms` (`.ino:65`), `uiRooms` (`.ino:71`), `kEmpty` (`WeatherUi.cpp:2165`).
**Koszt: 3 × 576 = 1728 B statycznego RAM.** Zapas 4400 B → zostaje **2672 B**.
Dochodzi `RoomHistory snap = gRooms;` (`.ino:439`) — kolejne **2312 B na stosie netTask** (dziś 1736).
Trzeba też dopisać 2 kolory do `roomCol[]` (`WeatherUi.cpp:2179`) i sprawdzić układ kafelków
przy 7–8 czujnikach (`WeatherUi.cpp:2226`: `cols = n > 4 ? 3 : 2` — przy 7–8 wychodzi 3 kolumny × 3 rzędy,
`ch=38`, `cy=54` → `54 + 2*(38+6) = 142`, a wykres startuje na `gy0=148`; **zmieści się na styk, 6 px luzu**).
Klucz NVS **nie wymaga bumpu**: 1736 ≠ 2312, więc `getBytesLength` złapie starą strukturę.
**Ryzyko: średnie/wysokie** (RAM + układ).

---

## 3. NVS: jeden globalny `Preferences prefs` obsługuje trzy przestrzenie nazw i dwa wątki — bez mutexa

**Gdzie:** `Settings.cpp:12` — `Preferences prefs;` w anonimowej przestrzeni nazw.
Używają go: `Settings::load/save/viSave/meterSave/meterAdd/meterDel/bleSet/clearWifi`,
`pvHistoryLoad/Save/Clear`, `roomHistoryLoad/Save` — w **dwóch przestrzeniach** (`pogoda`, `pvday`).

**Kto woła, z którego wątku:**

| Funkcja | Wątek |
|---|---|
| `roomHistorySave` (`.ino:441`) | **netTask** |
| `pvHistorySave` (`.ino:507`) | **netTask** |
| `settings().viSave()` (`Viessmann.cpp:160`, odświeżenie tokena) | **netTask** |
| `settings().save()` ×10 (`Portal.cpp:442,534,550,847,1050…1120`) | **webTask** |
| `settings().bleSet()` (`Portal.cpp:818`) | **webTask** |
| `settings().viSave()` (`Portal.cpp:865`) | **webTask** |

**Jaki błąd z tego wyniknie:** `prefs.begin(NS, ...)` na **tym samym obiekcie** z dwóch wątków.
Scenariusz: netTask wykonuje `roomHistorySave` → `prefs.begin("pvday", false)`; w tej samej chwili
użytkownik klika „Zapisz” w panelu → webTask wchodzi w `Settings::save()` → `prefs.begin("pogoda", false)`
**na tym samym uchwycie**. `Preferences::begin()` robi `end()` na poprzednim uchwycie i otwiera nowy.
Efekty, zależnie od przeplotu:
- `putBytes("rh2", ...)` ląduje w przestrzeni `pogoda` zamiast `pvday`,
- `prefs.end()` z jednego wątku zamyka uchwyt, którego drugi właśnie używa → `putString` cicho nie zapisuje,
- ustawienia WiFi/MQTT „zapisują się” w panelu i **znikają po restarcie**.

**Kiedy:** okno jest wąskie (zapisy trwają ms), ale `roomHistorySave` leci **co 10 minut** przez całą
dobę, a `viSave` co ~55 min. Przy aktywnym panelu to kwestia czasu. **Objaw: „zapisałem hasło i się
nie zapisało”** — nie do zdiagnozowania z ekranu.

**Poprawka:** albo (a) własny `Preferences` w każdej funkcji (jak robi to `OtaGuard.cpp:62,75,233`
— i **dlatego on jest bezpieczny**), albo (b) jeden mutex NVS obejmujący `begin()…end()`.
Wariant (a) jest prostszy i bezstanowy; `Preferences` to cienka nakładka na `nvs_handle`, koszt
otwarcia jest znikomy w porównaniu z samym zapisem.
**RAM: 0 B statycznego** (obiekt schodzi z globala na stos, ~16 B).
**Ryzyko: niskie.** *(Pokrywa się z zakresem agenta B — zgłaszam, bo to warstwa NVS.)*

---

## 4. Blobi NVS, gdzie kontrola rozmiaru jest JEDYNYM zabezpieczeniem — rodzeństwo pułapki `rh`

Szukałem dokładnie tego wzorca. Znalazłem trzy żywe przypadki.

### 4a. `"w"` i `"l"` mają IDENTYCZNY rozmiar 288 B
`Settings.cpp:252-277`, `PvData.h:34-35`:
```
uint16_t watts[144] = 288 B   → klucz "w"
uint16_t load[144]  = 288 B   → klucz "l"
```
Kontrola: `getBytesLength("w") == sizeof(h.watts)` (`:254`) i `getBytesLength("l") == sizeof(h.load)` (`:258`).
**Obie przechodzą dla obu blobów.** Literówka `prefs.getBytes("w", h.load, ...)` albo zamiana
kolejności w `pvHistorySave` **nie zostanie złapana nigdy** — wykres pokaże produkcję jako pobór
i odwrotnie, czyli czerwone i zielone słupki zamienią się miejscami. Dokładnie ta klasa co `rh`:
**cicha korupcja, nie crash**.
Drugi wariant tego samego: zmiana jednostki (W → 10 W, żeby zmieścić większe instalacje) zachowuje
rozmiar → stary profil zostanie wczytany i pokazany 10× za mało.

**Poprawka:** te dwa bloby to jedna informacja („profil doby”) rozbita na dwa klucze o tym samym
rozmiarze. Zapisywać **jedną strukturą** pod jednym kluczem z wersją w nazwie: `"prof1"`, o rozmiarze
`sizeof(watts) + sizeof(load)` = 576 B (asymetryczny wobec obu składowych, więc pomyłka jest
niemożliwa). Bump klucza kasuje profil bieżącego dnia — strata: jeden wykres do północy.
**RAM: 0 B.** **Ryzyko: niskie.**

### 4b. `"mets"` — 64 B, kontrola tylko rozmiarem
`Settings.cpp:48` — `if (prefs.getBytesLength("mets") == sizeof(meters))`.
`MeterCfg{uint32_t day; float m3;}` = 8 B × `METERS=8` = 64 B. Zmiana `METERS` złapie się (rozmiar
się zmieni), ale **zamiana `m3` z m³ na litry albo `day` z `epoch/86400` na `tm_yday`** — nie.
Patrz jednak ustalenie 8: **cała ta funkcja jest martwa**, więc najtańszą poprawką jest usunięcie.

### 4c. `"rh2"` żyje w przestrzeni `"pvday"` — a `pvHistoryClear()` robi `prefs.clear()`
`Settings.cpp:280-284` vs `Settings.cpp:296,309`.
`pvHistoryClear()` woła `prefs.clear()` na **całej przestrzeni `pvday`**, w której siedzi też `"rh2"`
(historia BLE 24 h). Funkcja jest **dziś nieużywana** (zero wywołań — sprawdzone), więc to nie jest
żywy błąd. To **nabity pistolet**: pierwszy, kto doda do panelu przycisk „wyczyść profil PV”,
skasuje przy okazji dobę historii z czujników i nie dowie się o tym.

**Poprawka:** albo przenieść historię pokoi do własnej przestrzeni (`"rooms"`), albo zamienić
`prefs.clear()` na trzy jawne `prefs.remove("day"/"w"/"l")`. Drugie jest tańsze i nie rusza NVS
użytkownika.
**RAM: 0 B.** **Ryzyko: niskie.**

### Czego NIE znalazłem (żeby było wiadomo, że sprawdzone)
- `"b%dkey"` — kontrola `== 16` na surowym kluczu, zmiana układu niemożliwa. OK.
- `otaguard` (`rst`, `panics`, `badver`, `badcnt`, `trialver`) — same skalary, każdy własny typ. OK.
- `"rh2"` — wersjonowany kluczem. OK (twoja dzisiejsza naprawka).

---

## 5. Lista widoków ma TRZY źródła prawdy + czwarte, nieaktualne, w komentarzu

| Gdzie | Co mówi |
|---|---|
| `Config.h:85-92` | `VIEW_COUNT = 9`, `VIEW_RADAR=2`, `VIEW_DAYS=3`, `VIEW_HOME=4`, `VIEW_BOILER=5`, `VIEW_PV=6`, `VIEW_FLIGHTS=7`, `VIEW_STATS=8` |
| `WeatherUi.cpp:1263-1296` | `switch (view) { case 0: … case 8: }` — **gołe literały** |
| `Portal.cpp:200` | `const NAMES=['Auto','Teraz','Godziny','Radar','5 dni','W domu','Piec','Fotowoltaika','Samoloty','Statystyki']` — tablica w JS |
| `pogoda-gdynia.ino:5-10` | **„5 widoków rotujących automatycznie”** + lista 5 pozycji — nieaktualne o 4 ekrany |

**Jaki błąd z tego wyniknie:** dołożenie ekranu albo zmiana kolejności wymaga trafienia w trzy
miejsca w trzech językach (C++ stała, C++ `switch`, JS tablica). Trafienie w dwa z trzech daje:
- pominięcie `switch` → **czarny ekran** na nowym widoku (`default: break;` nic nie rysuje, tło
  zostaje z `drawContentBg`), pasek postępu pokazuje segment, rotacja stoi na nim 9 s. Bez błędu.
- pominięcie `NAMES` → panel podpisuje przyciski **przesuniętymi nazwami** — klikasz „Piec”,
  dostajesz „W domu”. `pinView()` przyjmie każdy `idx < VIEW_COUNT` (`WeatherUi.cpp:2655`), więc
  żadnej walidacji nie ma.

Zauważ, że `drawProgress` (`:384-386`), `render` (`:1379-1389`), `prevView` (`:2630-2632`) i
`holdFor` (`:1237-1239`) **używają** `cfg::VIEW_*` poprawnie. Wyłamuje się tylko `drawView()` —
jedyne miejsce, gdzie mapowanie numer→funkcja jest zapisane literałami.

**Poprawka:**
1. `WeatherUi.cpp:1263` — `case cfg::VIEW_NOW:` … `case cfg::VIEW_STATS:` (trzeba dodać
   `VIEW_NOW=0`, `VIEW_HOURS=1` do `Config.h`, dziś ich nie ma).
2. `static_assert(cfg::VIEW_STATS == cfg::VIEW_COUNT - 1)` w `WeatherUi.cpp` — łapie rozjazd
   w czasie kompilacji.
3. `Portal.cpp:200` — nazw nie da się elegancko wyprowadzić z C++ do JS bez generatora; **realne
   minimum**: `/api/view` niech zwraca `count`, a JS niech asertuje `NAMES.length-1 === count`
   i wypisze ostrzeżenie w panelu, zamiast po cichu przesuwać podpisy.
4. `.ino:5-10` — poprawić komentarz na 9 widoków (albo skasować listę; `Config.h:85` już ją ma).
**RAM: 0 B.** **Ryzyko: niskie.**

---

## 6. `drawViewHours` — tablice `[13]` sprzężone z `WX_HOURS=12` wyłącznie przez wiarę

**Gdzie:** `WeatherUi.cpp:738-743` (`float temp[13]; float rain[13]; int code[13]; int hourLbl[13];
bool ok[13]; bool day[13];`), pętla `WeatherUi.cpp:756-764` (`ok[i + 1] = s.valid;` dla `i < WX_HOURS`),
`WeatherData.h:6` (`constexpr int WX_HOURS = 12;`).

**Na czym polega:** `13` musi być **dokładnie** `WX_HOURS + 1` (teraz + 12 godzin). Nigdzie tego nie
napisano ani nie sprawdzono. Dodatkowo w tej samej funkcji stoją literały `12` i `13` w pięciu
pętlach (`:769`, `:804`, `:819`, `:847`, `:861`) oraz `px = ox + 16 + i * 24` (`:789`), gdzie `24`
to „szerokość na punkt” dobrana tak, żeby `16 + 12*24 = 304` zmieściło się w 320.

**Jaki błąd z tego wyniknie:** ktoś (za pół roku, Ty) chce „prognozę na 24 h” i zmienia
`WX_HOURS = 12` → `24`. Kompiluje się bez ostrzeżenia. Pętla `:756` pisze `ok[13]`…`ok[24]` —
**12 elementów poza koniec sześciu tablic na stosie rdzenia 1**, w funkcji rysującej wołanej
30 razy na sekundę. To nie jest cicha korupcja danych, to **rozwalony stos loopTask i restart
w pętli**. Urządzenie wisi w łazience — objaw: mruga i się resetuje, bez logu (bo log ginie
z restartem), a `/api/diag` pokaże `reset.was_crash: true` dopiero jeśli zdążysz kliknąć.

**Poprawka:** `constexpr int N = WX_HOURS + 1;` na górze funkcji, tablice `[N]`, pętle do `N`,
`const int step = (W - 32) / WX_HOURS;` zamiast `24`.
**RAM: 0 B** (stos, ta sama wielkość).
**Ryzyko: niskie** — czysto mechaniczne, wynik identyczny dla `WX_HOURS=12`.

---

## 7. `lastAlertAt[8]` — indeksowana enumem bez sentinela

**Gdzie:** `pogoda-gdynia.ino:121` — `uint32_t lastAlertAt[8] = {};`
indeksowane `pogoda-gdynia.ino:863` — `const int idx = static_cast<int>(a.kind);`
enum: `WeatherUi.h:11-20` — `AlertKind { NONE, STORM, WIND, FROST, HEAT, HEAVY_RAIN, PV_FAULT, PV_OFFLINE }`.

**Na czym polega:** enum ma **dokładnie 8** wartości, tablica ma **dokładnie 8** miejsc, i nic
tego nie pilnuje. `AlertKind::PV_OFFLINE` jest zdefiniowany, ale **nigdy nie ustawiany** —
`buildAlert()` (`.ino:564-649`) go nie produkuje. Czyli tablica jest już „na wyrost o jeden”
i wygląda, jakby miała zapas. Nie ma.

**Jaki błąd z tego wyniknie:** dodanie dziewiątego alertu (np. `AlertKind::SNOW`, całkiem
prawdopodobne — kody WMO 71/73/75 są już obsługiwane w ikonach) daje `lastAlertAt[8]` = **zapis
4 bajtów poza globalną tablicę**, wprost w sąsiedni symbol w `.bss`. Sąsiadem jest `lastAlertKind`
albo `uiWeather` — zależnie od linkera. Objaw: losowo psująca się pogoda na ekranie albo alert,
który nie chce zgasnąć. Nie do powiązania z przyczyną.

**Poprawka:** `COUNT` na końcu enuma (`WeatherUi.h:20`) i `lastAlertAt[static_cast<int>(AlertKind::COUNT)]`.
**RAM: 0 B.** **Ryzyko: niskie.**

---

## 8. Cała weryfikacja licznikiem gazu jest martwa — a zajmuje RAM, gLock i NVS

**Gdzie:**
- `pogoda-gdynia.ino:70` — `GasHistory gGas{};` (**248 B statycznie**)
- `pogoda-gdynia.ino:391-392` — `gGas.advance()` / `gGas.push()` **pod `gLock`**, co 3 minuty
- `Settings.h:58-63` — `METERS=8`, `meters[8]` (**64 B**), `meterAdd`, `meterDel`, `meterSave`
- `Settings.cpp:48-50,121-170` — wczytywanie i zapis blobu `"mets"`
- `GasMeter.h:65` — `sumBetween()`
- `GasMeter.h:83` — `struct MeterRead`

**Na czym polega:** sprawdziłem wszystkie wywołania. **`gGas` nigdy nie jest czytany** — jest
zapisywany co 3 min i nigdy nie odczytany przez nikogo. **Nie jest też utrwalany w NVS**, więc ginie
przy każdym restarcie. `meterAdd`/`meterDel`/`sumBetween`/`MeterRead` **nie mają ani jednego
wywołującego**; w `Portal.cpp` nie ma trasy `/api/meter*` (sprawdzone w `routes()`, `:943-971`).

Komentarz `GasMeter.h:5-16` opisuje działającą funkcję („zbieramy je sami, dzien po dniu, i sumujemy
miedzy odczytami licznika”) — **opisuje kod, którego nie ma**. To źródło prawdy o zachowaniu
urządzenia, które kłamie czytelnikowi po pół roku najskuteczniej ze wszystkiego w tym repo, bo
brzmi konkretnie i ma daty pomiarów.

**Jaki błąd z tego wyniknie:** sam z siebie żaden — to nie jest bug, to **312 B RAM-u przy zapasie
4400 B** (7% bariery) i jedno zajęcie `gLock` co 3 minuty za nic. Realna szkoda jest inna:
za pół roku ktoś przeczyta `GasMeter.h`, uzna że funkcja działa, i będzie szukał, czemu nie widzi
wyników. Albo dopisze `sumBetween()` do ekranu i zobaczy zera, bo `gGas` nie przeżywa restartu.

**Poprawka (do decyzji — to jest zakres agenta D, zgłaszam bo dotyczy blobu `"mets"`):**
albo dokończyć (trasa `/api/meter`, zapis `gGas` do NVS, wyświetlenie), albo usunąć i zamienić
komentarz `GasMeter.h:5-16` na dwa zdania w `BACKLOG.md`. **Trzecia opcja — zostawić jak jest —
jest najgorsza**, bo to jedyny stan, w którym dokumentacja kłamie.
**RAM: −312 B** (odzysk) przy usunięciu.
**Ryzyko: niskie** (usunięcie martwego kodu; blob `"mets"` w NVS zostanie i nikomu nie zaszkodzi).

---

## 9. `cfg::FOOTER_Y = 208` i `cfg::FOOTER_H = 32` — stałe, które NIE definiują stopki i podają złe liczby

**Gdzie:** `Config.h:51-52` vs `WeatherUi.cpp:436,441`.

```
Config.h:51        constexpr int FOOTER_Y = 208;     ← nieużywane
Config.h:52        constexpr int FOOTER_H = 32;      ← nieużywane
WeatherUi.cpp:436  const int y = VIEW_H;   // 206    ← PRAWDZIWA pozycja stopki
WeatherUi.cpp:441  dst.fillRect(0, y, W, 34, ...);   ← PRAWDZIWA wysokość
```
Sprawdzone grepem: `FOOTER_Y` i `FOOTER_H` **nie występują nigdzie poza własną definicją**.
Stopka realnie zaczyna się na **206** (nie 208) i ma **34** px (nie 32).

To jest **dokładnie wzorzec z Twojego dowodu startowego**: stała, która miała coś definiować, stoi
obok literału, który to naprawdę definiuje — tyle że tutaj stała jest w dodatku **nieprawdziwa**.
Ta sama choroba: `WeatherUi.cpp:36` — `constexpr int CB = cfg::CONTENT_Y + cfg::CONTENT_H; // 206`
z komentarzem „(poza obszarem)” — też **nieużywane**. Czyli liczba 206 ma **trzy** niezależne
byty: `VIEW_H` (`WeatherUi.h:110`, jedyny żywy), `CB` (martwy) i `CONTENT_Y+CONTENT_H` (martwe),
a `FOOTER_Y` twierdzi, że to 208.

**Jaki błąd z tego wyniknie:** ktoś przesuwa stopkę i w dobrej wierze zmienia `cfg::FOOTER_Y`.
**Nic się nie dzieje.** Traci pół godziny na zrozumienie, czemu. W wersji gorszej: dopisuje nowy
element „pod stopką” licząc od `FOOTER_Y + FOOTER_H = 240` i rysuje poza ekranem, bo prawdziwy
dół stopki to też 240, ale z innych liczb (206+34) — trafia przypadkiem. Przy następnej zmianie
już nie trafi.

**Poprawka:** usunąć `FOOTER_Y`, `FOOTER_H` z `Config.h` i `CB` z `WeatherUi.cpp:36`, albo
zdefiniować `FOOTER_Y = VIEW_H` i faktycznie użyć w `drawFooterTo`. Polecam usunięcie — `VIEW_H`
jest już jedynym źródłem i działa.
**RAM: 0 B** (constexpr nie zajmuje RAM).
**Ryzyko: niskie.**

---

## 10. „Slot = 10 minut” — jedna prawda rozsiana po ośmiu miejscach w czterech plikach

| Gdzie | Zapis | Co to naprawdę jest |
|---|---|---|
| `RoomHistory.h:16` | `SLOTS = 144` | 24 h / 10 min |
| `RoomHistory.h:45` | `epoch / 600` | 600 s = 10 min |
| `PvData.h:33` | `SLOTS = 144` | to samo, drugi raz |
| `PvData.h:57` | `(hour * 60 + minute) / 10` | to samo, trzeci raz |
| `GasMeter.h:98` | `SLOTS = 144` | to samo, czwarty raz |
| `GasMeter.h:110` | `(hour * 60 + minute) / 10` | piąty raz |
| `WeatherUi.cpp:1041` | `s0 = 30, s1 = 137` | 05:00…22:50 **wyrażone w slotach** |
| `WeatherUi.cpp:1082` | `const int s = hh * 6;` | **6 = slotów na godzinę** |
| `WeatherUi.cpp:2153` | `(hh * 6 - s0)` | to samo, drugi raz |
| `WeatherUi.cpp:2323` | `SLOTS - 1 - hh * 6` | trzeci raz |
| `WeatherUi.cpp:2375` | `SLOTS - 1 - 72` | **72 = 12 h × 6**, czwarty raz, już bez `*6` |
| `.ino:442` | `nextRoomSaveAt = millis() + 600000` | 600000 ms = 10 min, szósty raz |
| `Config.h:68` | `PV_STORE_MS = 5 min` | **inna** wartość — zapis co 5 min przy slocie 10 min |

**Jaki błąd z tego wyniknie:** zmiana rozdzielczości na 5 minut (`SLOTS = 288`) — całkiem realna,
bo wykres PV ma 26 px wysokości i 144 punkty, więc rozdzielczość czasu jest tu jedynym zapasem —
wymaga trafienia w **trzynaście** miejsc, z czego `6`, `30`, `137` i `72` są **nierozpoznawalne
bez czytania komentarza**. Trafienie w dwanaście z trzynastu:
- `s0=30, s1=137` przy `SLOTS=288` → wykres PV pokaże **wycinek 02:30–11:25** zamiast całego dnia,
  bo `s0/s1` to indeksy, nie godziny. Bez błędu, bez pustego miejsca — po prostu inne dane.
- `hh * 6` → znaczniki godzin 8/12/16/20 wylądują na **4:00/6:00/8:00/10:00**. Podpisy zostaną te same.

To jest cicha korupcja **prezentacji**, dokładnie jak `rh` była cichą korupcją danych: wszystko
się rysuje, nic nie krzyczy, liczby są nieprawdziwe.

**Poprawka (minimalna, bez ruszania struktur):**
```
// jedno miejsce, np. Config.h albo nowy TimeGrid.h
constexpr int SLOT_MIN   = 10;
constexpr int SLOTS_HOUR = 60 / SLOT_MIN;      // 6
constexpr int SLOTS_DAY  = 24 * SLOTS_HOUR;    // 144
```
i `static_assert(RoomHistory::SLOTS == SLOTS_DAY)`, `static_assert(PvHistory::SLOTS == SLOTS_DAY)`,
`static_assert(BurnerHistory::SLOTS == SLOTS_DAY)`. Same struktury zostawić — mają prawo do
własnej `SLOTS`, byle asercja pilnowała zgodności. W `WeatherUi.cpp` zamienić `hh * 6` →
`hh * SLOTS_HOUR`, `72` → `12 * SLOTS_HOUR`, `s0 = 30` → `5 * SLOTS_HOUR`,
`s1 = 137` → `23 * SLOTS_HOUR - 1`.
**RAM: 0 B.** **Ryzyko: niskie** (wartości identyczne, `static_assert` udowodni to w kompilacji).

---

## 11. Współrzędne układu w `WeatherUi.cpp` — kto pilnuje, żeby się nie nachodziły?

Odpowiedź: **nikt. Pilnuje to komentarz i Twoja pamięć.** Wypisuję sprzężenia, które znalazłem —
każde jest parą liczb, które **muszą** się zgadzać, a łączy je wyłącznie zdanie po polsku.

| Sprzężenie | Gdzie | Co je łączy dziś |
|---|---|---|
| opis pogody kończy się na `y=128` ↔ kafelki startują `cy0 = 131` | `:570` ↔ `:641` | komentarz „nizej niz 122: druga linia opisu siega y=128” |
| `gl()` kotwiczy górą → `sub` zajmuje 104..112 ↔ „PRODUKCJA / ZUŻYCIE” na `128` | `:999` ↔ `:1034` | komentarz „104, nie 108…” |
| 4 wiersze źródeł `52 + i/2*17` kończą się na `y=120` ↔ karty `cy0 = 128` | `:2438` ↔ `:2560` | komentarz „4 wiersze po 17 px koncza sie na y=120, karty zaczynaja sie na 128” |
| kafelki „W domu” `cy=54`, `ch=38` ↔ wykres `gy0 = 148` | `:2230` ↔ `:2286` | **nic** (przy 5–6 czujnikach: 54+38+6+38 = 136, luz 12 px) |
| `kDescMaxW = 116` ↔ `icx = ox + 258` ↔ `W = 320` | `:539` ↔ `:608` | komentarz „pod ikona (srodek x=258) miesci sie najwyzej 116 px” |
| zachód `ox+98` + szerokość ↔ opad „dokleja się do UV od prawej” | `:678` ↔ `:720-726` | **kod liczy to w locie** — jedyne miejsce zrobione porządnie |
| lista lotów `y0 = 52 + i*25`, `grow 23` ↔ `CY=34`, `CH=172` | `:1599` ↔ `:1575` | **nic** (5 lotów: 52+5*25 = 177 < 206) |
| stopka `y = VIEW_H` + `34` ↔ `SCREEN_H = 240` | `:436,441` | `206 + 34 = 240` — arytmetyka w głowie |

**Jaki błąd z tego wyniknie:** każda zmiana czcionki, dodanie linii tekstu albo zmiana liczby
kafelków przesuwa jeden koniec pary. Drugi zostaje. Objaw: **napisy nachodzą na siebie o 2–5 px** —
brzydko, ale czytelne, więc **nikt tego nie zgłosi**, a Ty zobaczysz to dopiero na zrzucie ekranu
z panelu. Historia projektu ma tego już cztery przypadki (komentarze przy `:996`, `:641`, `:716`,
`:2224` opisują dokładnie takie zderzenia, już naprawione).

**Poprawka — NIE polecam wielkiej siatki układu.** To by był refaktor za 2000 linii przy zapasie
4,4 kB i działającym ekranie. Polecam **najtańszą rzecz, która zamienia ciszę w krzyk**:
```
// WeatherUi.cpp, obok HDR_Y
static_assert(131 >= 128, "kafelki TERAZ musza zaczynac sie pod opisem pogody");
static_assert(128 >= 52 + 4*17, "karty STATYSTYK musza zaczynac sie pod wierszami zrodel");
static_assert(148 >= 54 + 2*(38+6), "wykres W DOMU musi zaczynac sie pod trzecim rzedem kafelkow");
```
— czyli te same zdania, które dziś są komentarzem, zapisane tak, żeby kompilator je sprawdził.
Zero kosztu, zero ryzyka, a następna zmiana czcionki **nie skompiluje się** zamiast wyglądać źle.
Docelowo (BACKLOG, nie teraz): stałe `NOW_CARDS_Y`, `STATS_CARDS_Y`, `HOME_CHART_Y` obok `HDR_Y`,
który jest już zrobiony poprawnie (`:521`) i jest dowodem, że ten wzorzec działa w tym projekcie.
**RAM: 0 B.** **Ryzyko: niskie.**

---

## 12. `drawViewStats` — `src[8]` i `i < 8` dwa razy ta sama ósemka

**Gdzie:** `WeatherUi.cpp:2411` (`} src[8] = {`) i `WeatherUi.cpp:2436` (`for (int i = 0; i < 8; ++i)`).

**Jaki błąd z tego wyniknie:** dodanie dziewiątego źródła (np. „Bramka #2”) do inicjalizatora bez
zmiany pętli → **źródło nie rysuje się** i nikt nie zauważy, że go nie ma (ekran wygląda pełny,
bo 8 pozycji ładnie wypełnia 4 wiersze × 2 kolumny). Odwrotnie: zmiana pętli bez dopisania do
tablicy → **czytanie poza tablicę** i rysowanie śmieci jako nazwy źródła.

**Poprawka:** `for (int i = 0; i < static_cast<int>(sizeof(src)/sizeof(src[0])); ++i)`.
Uwaga na układ: 9 źródeł = 5 wierszy = `52 + 4*17 + 17 = 137` > `cy0 = 128` (patrz ustalenie 11) —
czyli dziewiąte źródło **wjedzie w karty**. To jest właśnie ta para, którą warto zabezpieczyć
`static_assert`-em.
**RAM: 0 B.** **Ryzyko: niskie.**

---

## 13. Viessmann: „HTTP 200 to nie sukces” wiadomo w JEDNYM miejscu z dwóch

**Gdzie:** `Viessmann.cpp:208-227` (`setCircuitTemp` — **sprawdza** `statusCode` w ciele)
vs `Viessmann.cpp:246` (ścieżka odczytu — `if (code != 200) { ... }` i **koniec**).

**Na czym polega:** komentarz `:208-211` opisuje regułę „Viessmann odsyla HTTP 200 nawet gdy komenda
zostala ODRZUCONA — prawdziwy status siedzi w ciele”, powołuje się na PyViCare i jest w 100%
słuszny. Reguła jest **zastosowana tylko w ścieżce zapisu**. Ścieżka odczytu (`GET /features`)
ufa samemu kodowi HTTP.

**Jaki błąd z tego wyniknie:** jeśli API odda 200 z ciałem błędu (rate limit — a limit to
1450/dobę i 120/10 min, przy odpycie co 3 min = 480/dobę, więc **margines jest, ale nie jest
ogromny**), parser nie znajdzie `features` i zwróci model z zerami zamiast błędu. Ekran „PIEC”
pokaże **ciepłą wodę 0.0 °C** zamiast „Piec nie odpowiada”, a `/api/state` zamelduje `ok: true`.
To jest kłamstwo w tę samą stronę co pominięte flagi `hasTemp`/`hasHum` w `BleSensors.h:42-45`
(„Bez tych flag rysowalismy 0.0 C, dopoki nie doszla ramka z temperatura — czyli klamalismy”) —
ta lekcja jest już wyciągnięta w BLE i **nie została przeniesiona na Viessmanna**.

**Poprawka:** w ścieżce odczytu po sparsowaniu sprawdzić `doc["statusCode"] | 0` i `doc["error"]`
zanim uznamy odpowiedź za dane; przy braku `features` ustawić `valid=false` z komunikatem.
**RAM: 0 B.** **Ryzyko: niskie.** *(Kontrakty API to zakres agenta C — zgłaszam jako
niespójność jednej prawdy między dwiema ścieżkami tego samego pliku.)*

---

## 14. Drobne, ale z tej samej rodziny

**14a. `ox + 318` zamiast `ox + W - 2`** — `WeatherUi.cpp:1581`, `:1614`.
W całym pliku prawa krawędź to `ox + W - 10` / `ox + W - 12`. Tu dwa razy literał `318`.
Przy zmianie `SCREEN_W` te dwa napisy zostaną w starym miejscu. **Ryzyko: niskie.** Zamienić na `ox + W - 2`.

**14b. `WeatherUi.cpp:33-36`** — `W`, `CY`, `CH` jako lokalne aliasy `cfg::*` **to dobry wzorzec**,
zostawić. Tylko `CB` (`:36`) jest martwy — usunąć (patrz 9).

**14c. `Config.h:85` `VIEW_DAYS = 3` vs `WeatherData.h:7` `WX_DAYS = 5`** — dwie stałe o mylnie
podobnych nazwach, znaczą zupełnie co innego (indeks ekranu vs liczba dni prognozy). Nie jest to
błąd, ale przy czytaniu po pół roku to pułapka na 10 minut. Rozważyć `VIEW_IDX_DAYS`.

**14d. `Portal.cpp:966`** — `server.on("/api/vi/set", apiViSet)` bez `HTTP_POST`.
Zmiana nastawy pieca jest osiągalna zwykłym **GET-em**, czyli `<img src="http://<ip>/api/vi/set?t=70">`
na dowolnej stronie otwartej w domowej sieci przestawi ogrzewanie. Komentarz `:916` mówi „Jawne,
reczne — zaden automat tego nie wola” — to prawda o **naszym** kodzie, nie o cudzym.
Poprawka: `HTTP_POST`. **Ryzyko: niskie.** *(Zakres agenta B/C, ale bariera „sekrety/dostęp” jest w moim.)*

---

## Czego szukałem i NIE znalazłem (sprawdzone, żeby nie szukać drugi raz)

- **`320`/`240` zaszyte zamiast `cfg::SCREEN_W/H`** — **czysto**. Jedyne trafienia to komentarze
  (`WeatherUi.cpp:188`, `:1894`, `:2675`). Kod konsekwentnie używa `cfg::SCREEN_W`, `cfg::SCREEN_H`,
  `W`, `VIEW_H`. Ten obszar jest zrobiony dobrze.
- **Progi powtórzone w kilku plikach** — **prawie czysto**. `cfg::CPU_T_*`, `cfg::HEAP_*` mają po
  jednym źródle i są używane z `cfg::`. Dwa wyjątki, oba świadome i lokalne:
  `WeatherUi.cpp:501` (`cpuTempC_ >= 75.f` w stopce — próg *stopki*, nie ekranu statystyk;
  `cfg::CPU_T_OK` to 70) i `WeatherUi.cpp:1112` (`inverterTempC > 65.f` — temperatura **falownika**,
  inna oś niż CPU). Progi RSSI są trzy razy (`.ino:94` `kRssiRoamBelow`, `WeatherUi.cpp:1721` paski
  na ekranie IP, `:2595` paski na statystykach) — ale to **trzy różne skale do trzech różnych celów**,
  słusznie osobne. Nie ruszać.
- **Sekrety w repo** — **czysto**. `Config.h:5-7` deklaruje zasadę i jest jej wierne. `/api/state`
  (`Portal.cpp:371-395`) zwraca `mq_pass_set` jako flagę, nigdy hasła. `/api/diag` (`:719-728`)
  jawnie pomija user/hasło. `/api/vi` (`:900-901`) zwraca `cid` (publiczny) i `auth` jako flagę,
  nigdy `viRefresh`. `/api/ble` (`:780`) zwraca `hasKey`, nigdy klucza. Konsola szeregowa
  (`:1127`, `:1131`) też pilnuje. **Ta bariera trzyma.**
- **`OtaGuard`** — używa **własnych, lokalnych** `Preferences` (`:62`, `:75`, `:233`), ma
  `portMUX` na werdykt (`:41`, `:91-100`) i jednorazowe klucze skalarne. **Wzorzec do naśladowania
  w `Settings.cpp`** (patrz ustalenie 3).

---

## Podsumowanie — kolejność wdrożenia

| # | Ustalenie | RAM | Ryzyko | Dlaczego teraz |
|---|---|---|---|---|
| 1 | `MqttClient.cpp` — czwórka + bufor `p[224]` | 0 | niskie | **żywy błąd**, odpali się przy 5. czujniku |
| 2 | Jedna prawda o liczbie czujników (wariant A) | 0 | niskie | domyka 1; kasuje kłamstwo w panelu |
| 3 | `Preferences` per-funkcja | 0 | niskie | **żywy wyścig**, gubi ustawienia |
| 4 | Bloby `"w"`/`"l"` → jeden `"prof1"`; `pvHistoryClear` bez `clear()` | 0 | niskie | rodzeństwo `rh`, cicha korupcja |
| 6 | `drawViewHours` — `N = WX_HOURS+1` | 0 | niskie | mina pod stopą przyszłej zmiany |
| 7 | `AlertKind::COUNT` | 0 | niskie | j.w., jedna linia |
| 5 | Widoki — `case cfg::VIEW_*` + `static_assert` | 0 | niskie | j.w. |
| 9 | Usunąć `FOOTER_Y`/`FOOTER_H`/`CB` | 0 | niskie | stałe, które kłamią |
| 12 | `src[8]` → `sizeof` | 0 | niskie | jedna linia |
| 10 | `SLOT_MIN`/`SLOTS_HOUR` + `static_assert` | 0 | niskie | 13 miejsc, ale zmiana mechaniczna |
| 11 | `static_assert` na 3 pary układu | 0 | niskie | zamienia ciszę w błąd kompilacji |
| 13 | Viessmann — `statusCode` też w odczycie | 0 | niskie | zakres agenta C |
| 8 | Martwy licznik gazu — dokończyć albo usunąć | **−312** | niskie | decyzja produktowa (agent D) |
| 14 | Drobne (`318`, `/api/vi/set` na POST) | 0 | niskie | przy okazji |

**Suma wpływu na RAM statyczny: 0 B, albo −312 B jeśli wejdzie ustalenie 8.**
Żadna z propozycji nie zbliża się do bariery 76000 B. Jedyny wariant, który by ją naruszył
(ustalenie 2B, `ROOMS = 8`, **+1728 B**), świadomie odrzucam na rzecz wariantu A.
</content>
</invoke>
