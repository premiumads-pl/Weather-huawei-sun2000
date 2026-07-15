# Audyt D — przerost inżynierski: co usunąć

Data: 2026-07-15 · zakres: kod własny (bez `WeatherIcons.h`, `PlFont*.h`, `MapData*.h`)
Metoda: liczenie wywołań (grep po całym repo), rozmiary symboli z **linkera**
(`build/pogoda-gdynia.ino.map`, build z 15.07 20:50), pomiar linii skryptem.

Każda liczba RAM w tym raporcie pochodzi z mapy linkera, nie z `sizeof` policzonego w głowie.

---

## 1. WERDYKT

**NIE — projekt nie jest przeinżynierowany. Martwego kodu jest ~193 linie, czyli 2,0 %
własnego kodu; to jest poniżej normy, a nie powyżej.**

Uzasadnienie liczbami:

| Miara | Wartość | Ocena |
|---|---|---|
| Kod własny | ~9 800 linii | — |
| **Martwy kod (udowodniony)** | **~193 linie = 2,0 %** | typowy projekt ma 5–15 % |
| Statyczny RAM | 71 600 / 76 000 B | zapas 4 400 B |
| **RAM do odzysku z usunięć** | **352 B = 0,5 %** | **RAM nie jest tu problemem** |
| Abstrakcje z jedną implementacją | 0 | brak fabryk, interfejsów, DI |
| Warstwy pośrednie bez treści | 0 | `Portal` → `Settings` → NVS, bez owijek |
| Konfigurowalność, której nikt nie używa | 2 flagi debug (`PROFILE_FRAME`, `COLOR_TEST_MODE`) | uzasadnione |
| Największa funkcja | `netTask` 366 linii, `drawViewStats` 247 | duże, ale nie patologiczne |

**Czego tu NIE MA, a co jest typowym objawem przerostu:** nie ma ani jednej klasy
abstrakcyjnej z jedną implementacją. Nie ma wstrzykiwania zależności. Nie ma
`IWeatherProvider`. Nie ma warstwy repozytoriów nad NVS. Nie ma systemu zdarzeń.
Nie ma menedżera menedżerów. Klienty (`PvClient`, `WeatherClient`, `RadarClient`,
`FlightClient`, `Viessmann`) to wolne funkcje w przestrzeniach nazw wypełniające POD-y —
najprostsza rzecz, jaka mogła zadziałać. **To jest architektura płaska, nie przerośnięta.**

**Ale jedno zastrzeżenie — CZĘŚCIOWO, w jednym miejscu:** projekt ma **jedną funkcję
zbudowaną w połowie** (weryfikacja licznika gazu). To nie jest przerost z ambicji —
to jest **niedokończona robota, która wygląda na skończoną**. I to jest gorsze niż
przerost, bo kłamie czytelnikowi. 145 linii, 312 B RAM, zero wartości. Szczegóły w § 2.1.

**Wrażenie „całość jest over-engineered" pochodzi najpewniej z dwóch miejsc, i oba są
mylące:**

1. **`WeatherUi.cpp` ma 2732 linie.** Wygląda źle. Nie jest źle — patrz § 4.1.
   Dziewięć rendererów to 1426 linii (52 %), największy ma 247. Reszta to gęsto
   używana warstwa pomocnicza (~280 wywołań). Podział przesunąłby linie między pliki
   i nic by nie uprościł.
2. **`Portal.cpp` ma 1143 linie** — z czego **321 to osadzony HTML/CSS/JS** w `PROGMEM`
   (linie 42–362, `PAGE[]`). Realny C++ to ~822 linie na **24 endpointy** = ~34 linie
   na endpoint. To nie jest przerost, to jest panel WWW.

---

## 2. DO USUNIĘCIA OD RAZU

### 2.1. Weryfikacja licznika gazu — CAŁA FUNKCJA JEST MARTWA · ~145 linii · 312 B RAM

To jest największe i najtwardsze ustalenie tego audytu. Trop z zadania **potwierdzony
w całości i szerszy, niż brzmiał**.

**Dowód, że nikt tego nie woła** (grep po całym repo, bez wyjątków):

| Symbol | Gdzie | Zapis | Odczyt |
|---|---|---|---|
| `GasHistory gGas` | `pogoda-gdynia.ino:70` | `.ino:391` `advance()`, `:392` `push()` | **BRAK — ani jednego** |
| `GasHistory::sumBetween()` | `GasMeter.h:65-79` | — | **0 wywołań** (tylko definicja) |
| `struct MeterRead` | `GasMeter.h:83-86` | — | **0 instancji w całym repo** |
| `Settings::meterAdd()` | `Settings.cpp:128-158` | — | **0 wywołań** |
| `Settings::meterDel()` | `Settings.cpp:160-170` | — | **0 wywołań** |
| `Settings::meterSave()` | `Settings.cpp:121-125` | woła tylko `meterAdd`/`meterDel` (też martwe) | — |
| `Settings::meters[8]` | `Settings.h:60` | `Settings.cpp:49` (wczytanie z NVS) | **nikt nie czyta** |

**`Portal.cpp` nie zawiera ani jednego wystąpienia słów `meter`, `mets`, `gas`, `licznik`.**
Nie ma formularza. Nie ma endpointu. Nie ma komendy szeregowej. **Nie ma jak wpisać
odczytu licznika** — a bez odczytu `sumBetween()` nie ma czego z czym porównać.
Funkcja nie ma wejścia i nie ma wyjścia. Zbiera dane co 3 minuty w próżnię.

> Potwierdzenie niezależne: audyt B znalazł to samo od strony współbieżności (W-4,
> „`gGas` — brak pary `uiGas`, dane zapisywane w próżnię"). Dwie różne metody, ten sam wynik.

**RAM (z mapy linkera, nie z szacunku):**
```
.bss.gGas       0x3fca5b4c   0xf8  = 248 B   <- pogoda-gdynia.ino.cpp.o
.data.gSettings 0x3fc9bbbc  0x630  = 1584 B  <- w tym meters[8] = 8 x 8 B = 64 B
```
**Razem: 312 B statycznego RAM.**

**Do usunięcia:**

| Plik | Linie | Ile |
|---|---|---|
| `GasMeter.h` | 1–17 (komentarz nagłówkowy o gazie), 18–80 (`GasHistory`), 83–86 (`MeterRead`) | ~86 |
| `Settings.h` | 57–63 (`METERS`, `MeterCfg`, `meters[]`, 3 deklaracje) | 7 |
| `Settings.cpp` | 8 (`#include`), 48–50 (wczytanie `mets`), 121–125, 128–158, 160–170 | ~49 |
| `pogoda-gdynia.ino` | 70 (`GasHistory gGas{}`), 391–393 (`advance`/`push`) | 4 |
| **Razem** | | **~145 linii** |

**Zostaje `BurnerHistory`** (`GasMeter.h:97-125`) — ta **żyje**: `gBurner`/`uiBurner`
(`.data`, 0x124 = **292 B każda**) są kopiowane pod mutexem i rysowane przez
`drawGasChart()` (`WeatherUi.cpp:2130`). Plik po cięciu warto przemianować na
`BurnerHistory.h`, bo o gazie nie będzie już w nim mowy.

**⚠️ ALE — decyzja należy do właściciela, nie do audytora.** Rozumowanie w komentarzu
`GasMeter.h:5-12` jest **poprawne i cenne**: liczniki `currentMonth`/`currentYear`
Viessmanna faktycznie kłamią (5,3 m³ po 4 latach instalacji), więc własny log dobowy
to jedyna droga. Kod jest napisany dobrze — `advance()` poprawnie obsługuje cofnięcie
zegara, przespane dni i przepełnienie okna. **To nie jest zły kod. To jest dobry kod
bez wtyczki.** Do wyboru:

* **(A) Dokończyć** — dodać w panelu formularz „stan licznika [m³]" → `meterAdd()`,
  a na ekranie PIECA jedną linijkę „od 12.06: piec twierdzi 41,2 m³ · licznik 43,0 m³
  (+4 %)". Koszt: ~40 linii `Portal.cpp` + ~10 linii `WeatherUi.cpp`. Wtedy te 312 B
  **zarabiają**.
* **(B) Usunąć** — ~145 linii, 312 B. Historia jest w gicie, wróci gdy będzie potrzebna.

**Czego NIE robić: zostawić jak jest.** Teraz jest najgorszy wariant z możliwych —
płaci się RAM-em i miejscem w głowie czytelnika, dostaje zero. Repo jest publiczne;
osoba z zewnątrz przeczyta ten świetny komentarz o zepsutych licznikach i będzie
szukać ekranu, którego nie ma.

**Dodatkowo martwe w tym samym pliku:**
`BurnerHistory::peak()` (`GasMeter.h:120-124`, **5 linii**) — **0 wywołań**.
`drawGasChart()` skaluje słupki na sztywno do 0–100 % (`WeatherUi.cpp:2146`:
`(h.mod[s] / 100.f)`), więc szczyt nie jest mu potrzebny. (Uwaga: `PvHistory::peak()`
w `PvData.h:68` **jest** używany — `WeatherUi.cpp:1042`. To dwie różne metody
o tej samej nazwie, nie pomylić.)

---

### 2.2. `cfg::FOOTER_Y` / `cfg::FOOTER_H` — nieużywane **I NIEPRAWDZIWE** · 2 linie · 0 B

Trop z zadania **potwierdzony w obu członach**.

```
Config.h:51:  constexpr int FOOTER_Y = 208;
Config.h:52:  constexpr int FOOTER_H = 32;
```
**0 wywołań w całym repo** — grep po `FOOTER_Y|FOOTER_H` zwraca wyłącznie te dwie
definicje. RAM: 0 B (`constexpr` nieużywany nie trafia do binarki).

**Ale to nie o RAM tu chodzi — one KŁAMIĄ.** Prawdziwa stopka, z `WeatherUi.cpp:436-441`:
```cpp
const int y = VIEW_H;   // 206      <- nie 208
...
dst.fillRect(0, y, W, 34, col::HEADER);   // <- wysokość 34, nie 32
```
Rzeczywistość: **y = 206, wysokość = 34** (bo `VIEW_H = 206`, a `SCREEN_H - VIEW_H
= 240 - 206 = 34`). Config mówi **208 / 32**. **Rozjazd o 2 piksele w obu polach.**

Zgadza się tylko suma (208+32 = 206+34 = 240), co jest najgorszym rodzajem błędu:
wygląda na spójny. Ktoś (albo współautor z GitHuba, albo Ty za pół roku) użyje
`cfg::FOOTER_Y` do wyrównania nowego elementu i dostanie dwupikselowe przesunięcie,
którego nie znajdzie, bo *stała z Config.h musi być prawdziwa*.

**Usunąć obie linie.** Źródłem prawdy jest `WeatherUi::VIEW_H` i ono ma zostać
tam, gdzie jest. Ryzyko: **zerowe** (nic tego nie kompiluje).

---

### 2.3. `WeatherUi::drawFatal()` — martwy ekran · 12 linii · 0 B

```
WeatherUi.h:35        void drawFatal(const char* msg);      <- deklaracja
WeatherUi.cpp:295-305 void WeatherUi::drawFatal(...)        <- definicja
```
**0 wywołań.** Pełna lista `ui.*` w `pogoda-gdynia.ino` (21 metod) nie zawiera
`drawFatal`. Nie woła go też `Portal.cpp` ani nic innego.

Ścieżka błędu krytycznego idzie dziś przez `drawBoot()` z komunikatem
(`.ino:716`) — czyli **funkcja została zastąpiona i nikt jej nie sprzątnął**.
RAM: 0 B (kod, nie dane; ~200 B flash).

**Usunąć.** Ryzyko: zerowe.

---

### 2.4. `PlText.h` — 4 martwe pomocniki, pozostałość po usuniętym foncie GLCD · 19 linii · 0 B

| Funkcja | Linie | Wywołań |
|---|---|---|
| `drawStringLg()` | 133–136 | **0** |
| `drawStringRight()` | 142–146 | **0** |
| `drawChartLabel()` | 149–154 | **0** |
| `chartLabelWidth()` | 156–159 | **0** |

**Dowód, że to pozostałość, a nie zapas:** `WeatherUi.cpp:96-131` ma **własny,
równoległy komplet** owijek (`plStr`, `plCenter`, `plRight`, `gl`, `glCenter`,
`glRight`) i to ich używa — 163 wywołania łącznie. `drawStringRight()` z `PlText.h`
robi dokładnie to, co `plRight()` z `WeatherUi.cpp:112`.

A `drawChartLabel()`/`chartLabelWidth()` są martwe **z definicji projektu** — obie
wołają `s.drawString()` / `s.textWidth()` na wbudowanym foncie GLCD, a komentarz
`WeatherUi.cpp:115-118` stwierdza wprost:

> „Do v81 szły przez wbudowany GLCD (font 1) — a ten nie ma polskich znaków ani stopnia
> […] **GLCD zniknął z projektu** i błąd razem z nim."

Czyli: font zniknął, a dwa pomocniki do niego zostały. Gdyby ktoś ich użył, wróciłby
dokładnie ten błąd, który kosztował 5 wersji („CIEP A WODA", „52.4[]C").
**To nie jest martwy kod obojętny — to jest nabita pułapka na współautora.**
Repo jest publiczne.

**Usunąć wszystkie cztery.** RAM: 0 B (`inline` nieużywany nie trafia do binarki).
Ryzyko: zerowe.

---

### 2.5. Pola zapisywane i nigdy nieczytane · 10 linii · 40 B RAM

**(a) `vi::Model` — 3 pola, 24 B** (`gVi` + `uiVi`, każdy `.bss` 0x90 = 144 B):

| Pole | Deklaracja | Zapis | Odczyt |
|---|---|---|---|
| `burnerHours` | `Viessmann.h:33` | `Viessmann.cpp:432` | **brak** |
| `burnerStarts` | `Viessmann.h:34` | `Viessmann.cpp:433` | **brak** |
| `wifiRssi` | `Viessmann.h:47` | `Viessmann.cpp:452` | **brak** |

**Dowód jest w kodzie, napisany przez Ciebie** — `WeatherUi.cpp:2100`:
> „Licznik uruchomień i siła WiFi pieca — **wyrzucone**. Ekran ma pokazywać to, co
> sprawdza się codziennie, a nie wszystko, co API potrafi oddać."

Decyzja była świadoma i słuszna. **Tylko parsowanie zostało.** Usuń 3 pola + 3 linie
parsera. (`burnerHours` i `burnerStarts` idą jeszcze przez `propF()` na już pobranym
JSON-ie, więc nie kosztują zapytania — tylko RAM i linie.)

**(b) `PvSnapshot` — 2 pola, 16 B, ale KOSZT JEST WIĘKSZY NIŻ RAM**
(`gPv` + `uiPv`, każdy `.bss` 0x5c = 92 B):

| Pole | Deklaracja | Zapis | Odczyt |
|---|---|---|---|
| `efficiencyPct` | `PvData.h:14` | `PvClient.cpp:225` — **rejestr 32086** | **brak** |
| `meterOk` | `PvData.h:16` | `PvClient.cpp:227` — **rejestr 37100** | **brak** |

To nie są pola „za darmo". Każde to **osobna transakcja Modbus TCP**, wykonywana
**co 30 sekund**, po to, żeby wynik trafił do pola, którego nikt nie czyta:
```cpp
PvClient.cpp:225:  if (readU16(32086, u16)) snap.efficiencyPct = ...;  // nikt nie czyta
PvClient.cpp:227:  if (readU16(37100, u16)) snap.meterOk = (u16 == 1);  // nikt nie czyta
```
Cykl odpytania ma **10 odczytów rejestrów** (`PvClient.cpp:218-228`). **Dwa z nich,
czyli 20 %, są wyrzucane.** Przy 30-sekundowym odpycie to **2 880 zbędnych transakcji
Modbus na dobę**. Falownik nie zauważy, ale każda z nich wydłuża okno, w którym
`netTask` trzyma gniazdo — a to jest ten sam `netTask`, który potem walczy o pamięć
dla PNG radaru. Usunięcie skraca cykl PV o 20 % bez żadnej straty.

Dla porządku: **`inverterTempC` (32087), `pvVoltageV` (32016) i `statusCode` (32089)
są używane** (`WeatherUi.cpp:1112`, `:995`, `:982`) — nie ruszać.

---

### 2.6. `Ota::lastRemoteVersion()` + `lastRemote_` · 3 linie · 4 B

```
Ota.h:32   int lastRemoteVersion() const { return lastRemote_; }   <- 0 wywołań
Ota.h:35   int lastRemote_ = 0;                                    <- czyta tylko powyższe
Ota.cpp:258  lastRemote_ = remote;                                 <- jedyny zapis
```
Klasyczny martwy łańcuch: pole → getter → nikt. UI bierze wersję zdalną
z `otaStatus().remoteVersion` (`OtaStatus`, `Ota.h:19`), które jest żywe.
**Usunąć wszystkie trzy linie.** Ryzyko: zerowe.

---

### 2.7. Dwa martwe kolory · 2 linie · 0 B

`Colors.h`: **`col::BG_CARD_HI`** i **`col::SNOW`** — 0 wywołań w `.cpp`/`.ino`.
`constexpr`, więc 0 B. Sprzątanie przy okazji. (`col::SNOW` kusi — ekran pogodowy
ma śnieg; to znaczy, że kolor był planowany i nie wszedł.)

---

### Podsumowanie § 2

| Pozycja | Linie | RAM |
|---|---:|---:|
| 2.1 Weryfikacja licznika gazu (cała funkcja) | ~145 | **312 B** |
| 2.5 Pola pisane w próżnię (Viessmann + PV) | 10 | **40 B** |
| 2.4 `PlText.h` — 4 pomocniki po GLCD | 19 | 0 |
| 2.3 `drawFatal()` | 12 | 0 |
| 2.6 `Ota::lastRemoteVersion()` + `lastRemote_` | 3 | 4 B |
| 2.2 `FOOTER_Y` / `FOOTER_H` (nieprawdziwe!) | 2 | 0 |
| 2.7 Martwe kolory | 2 | 0 |
| **RAZEM** | **~193** | **~356 B** |

**Uczciwie o RAM: to jest 0,5 % z 71 600 B. Zapasu do bariery 76 000 masz 4 400 B.
Usunięcie tego kodu NIE rozwiązuje żadnego problemu z pamięcią, bo problemu z pamięcią
teraz nie ma.** Powód do usunięcia jest inny i ważniejszy: **te 193 linie kłamią.**
Opisują funkcje, których nie ma (§ 2.1), podają nieprawdziwe współrzędne (§ 2.2)
i zastawiają pułapkę na współautora (§ 2.4). W publicznym repo to kosztuje więcej
niż 356 bajtów.

---

## 3. DO ROZWAŻENIA

### 3.1. `BAND_N = 1` — dwa pasy **już nie istnieją**, a rusztowanie po nich stoi

**To wymaga sprostowania założeń zadania.** Brief wymienia „renderowanie w dwóch pasach
(132 → 66 kB)" jako świętość, której nie wolno ruszać. **Kod mówi co innego —
dwóch pasów już nie ma.** `WeatherUi.h:110-115`:

```cpp
static constexpr int VIEW_H = 206;
// Dwa pasy istniały tylko po to, żeby bufor miał 66 kB zamiast 132 kB — a to
// było potrzebne tylko dlatego, że nie wiedzieliśmy o 2 MB PSRAM (v50).
// Teraz bufor mieszka w PSRAM, więc rysujemy JEDEN raz zamiast dwa: pół roboty.
static constexpr int BAND_H = VIEW_H;   // = 206, nie 103
static constexpr int BAND_N = 1;        // <- pętla po pasach chodzi RAZ
```

Sam to zrobiłeś w v50 i sam to opisałeś. Czyli argument „66 kB przy barierze 76 kB"
**już nie obowiązuje** — bufor 320×206×2 = 132 kB nie leży w statycznym RAM ani na
stercie, tylko w PSRAM. Zostawiam to jako ustalenie, bo brief opierał się na
nieaktualnym stanie.

**Co z tego zostało martwe:** `BAND_N` jest parametrem, który zawsze ma wartość 1 —
podręcznikowa „spekulatywna ogólność". Pętla `for (int b = 0; b < BAND_N; ++b)`
występuje **w dwóch kopiach**: `pushBands()` (`WeatherUi.cpp:200-208`) i drugi raz,
przepisana ręcznie, w `render()` (`:1412-1422`, bo tam dochodzi pomiar `micros()`).

**ALE — i to jest powód, dla którego mówię „rozważyć", a nie „usunąć":**
mechanizm pasów **nie jest martwy, tylko przeprowadził się do zrzutu ekranu**.
`streamScreenshot()` (`WeatherUi.cpp:2713`) woła `setBand(shot, top, HT)`
w pętli po **10 pasach po 24 px** (`SHOT_H = 24`, `HT = 240`):
```cpp
for (int top = HT - SHOT_H; top >= 0; top -= SHOT_H) {
    setBand(shot, top, HT);
    paintFrame(shot, ...);
```
Bez `setBand()` i bez viewportu z `vpDatum` zrzut BMP do przeglądarki nie ma prawa
działać w 15 kB zamiast 150 kB. **Cała maszyneria viewportów zarabia na siebie —
tylko nie tam, gdzie ją napisano.** Tak samo przeciąganie `heapNow`/`nowMs` przez stos
(`WeatherUi.h:130-137`): dla ekranu (1 pas) jest zbędne, dla zrzutu (10 pasów
przedzielonych setkami ms transmisji) jest **konieczne** i komentarz to tłumaczy.

| ZA usunięciem `BAND_N`/`BAND_H`/`pushBands` | PRZECIW |
|---|---|
| `BAND_N` zawsze = 1 — pętla myli czytelnika | `setBand`/viewport **muszą zostać** dla zrzutu — cięcie musi być chirurgiczne |
| Ta sama pętla w 2 kopiach (`:200`, `:1412`) | Zysk: **~10 linii, 0 B RAM** — kosmetyka |
| Komentarz `WeatherUi.h:104-115` opisuje projekt, którego nie ma (mówi o 66 kB i „rysowany DWA RAZY") | Dotyka **najgorętszej ścieżki** — 20–30 fps, 9 ekranów; regresja jest kosztowna, a urządzenie jest nieosiągalne |
| `WeatherUi.cpp:195-197` ostrzega przed `fillSprite()` „bo 132 kB do bufora 66 kB" — bufor ma teraz 132 kB, więc ostrzeżenie ma złe uzasadnienie (choć nadal słuszną konkluzję: przy viewporcie `fillSprite` jest zły) | |

**Rekomendacja: NIE usuwać teraz. Poprawić komentarze.** Zysk z cięcia (10 linii, 0 B)
nie równoważy ryzyka na ścieżce, której nie ma jak zdebugować na wiszącym urządzeniu.
Ale **komentarz `WeatherUi.h:104-115` i `WeatherUi.cpp:195-197` opisują nieaktualną
rzeczywistość** i to warto naprawić w 10 minut — inaczej za rok ktoś (Ty) uwierzy,
że bufor ma 66 kB. Jeśli już cokolwiek ciąć: zostawić `setBand()`, `VIEW_H` i viewporty,
skasować `BAND_N`/`BAND_H`/`pushBands` i zastąpić je prostym „narysuj raz, wypchnij raz".

### 3.2. Trzy kopie kafelka (`struct Card`) — powtórzenie realne, ale **DRY tu może zaszkodzić**

Trzy ekrany budują rząd kafelków niemal tak samo:

| Ekran | Linie | Kafelków | Szerokość / skok | `chh` |
|---|---|---|---|---|
| `drawViewNow` | 621–662 | 4 | 74 / 78 | 52 |
| `drawViewPv` | 1088–1140 | 4 | 74 / 78 | 41 |
| `drawViewBoiler` | 2083–2113 | 3 | 98 / 102 | 40 |

Wspólny szkielet powtórzony **3×** (~12 linii każdy):
```cpp
const int grow = static_cast<int>(chh * clampf(e * 1.3f - i * 0.08f, 0.f, 1.f));
if (grow < 4) continue;
spr.fillRoundRect(x, cy0 + (chh - grow), cw, grow, 6, col::BG_CARD);
if (grow < chh - 2) continue;
spr.fillRoundRect(x, cy0, 3, chh, 1, cards[i].color);
```
**Co realnie boli:** formuła animacji `e * 1.3f - i * 0.08f` i dwa progi (`< 4`,
`< chh - 2`) są **fizycznie tą samą decyzją projektową** w trzech miejscach. Zmienisz
tempo wyjeżdżania kafelków — musisz trafić we wszystkie trzy, a kompilator nie
przypomni. To jest sprzężenie, nie zbieg okoliczności.

**Co przemawia PRZECIW scalaniu** (i dlatego to nie jest zalecenie):
* Trzy struktury `Card` mają **różne pola**: `Now` ma `const char* extra`
  (kierunek wiatru, dosunięty do prawej), `Pv` ma `char unit[6]` zamiast `const char*`
  (bo jednostka jest wyliczana przez `fmtPower`), `Boiler` nie ma ani jednego z nich.
* **Różne kolory wartości**: `Now` rysuje wartość zawsze `col::TEXT`, `Pv` i `Boiler` —
  kolorem kafelka.
* `Pv` ma dodatkowo **gałąź ASCII/nie-ASCII** dla jednostki (`WeatherUi.cpp:1129-1138`) —
  bo „°C" wymaga PlFont, a „kW" mieści się w węższym font10.

Wspólna `drawCardRow()` musiałaby przyjąć ~6 parametrów + `std::optional`-podobne pola
+ callback na rysowanie wartości. **To byłby przerost udający sprzątanie** — dokładnie
ten rodzaj nadgorliwego DRY, przed którym ostrzega brief.

**Rekomendacja: zostawić, ale wyciągnąć JEDNĄ rzecz** — formułę animacji do stałej
albo funkcji jednolinijkowej:
```cpp
// jedyna wspólna decyzja: tempo wyjeżdżania kafelków
inline float cardGrow(float e, int i) { return clampf(e * 1.3f - i * 0.08f, 0.f, 1.f); }
```
Zysk: 3 linie mniej i **koniec sprzężenia, które faktycznie boli**. Cała reszta
niech zostanie osobno — te kafelki tylko *wyglądają* podobnie, a robią co innego.

### 3.3. `PROFILE_FRAME` — flaga opisuje nieaktualne zachowanie

`Config.h:82-83` mówi: „Pomiar czasu klatki […] **co 2 s na Serial**. […] domyślnie
wyłączone, bo to tylko log". Ale `WeatherUi.cpp:1409-1424` mierzy klatkę
**zawsze** i wysyła do `diag()` — z komentarzem „Pomiar klatki idzie do diagnostyki,
a nie na Serial — urządzenie wisi na ścianie i portu szeregowego nikt nie zobaczy".

Czyli pomiar jest teraz bezwarunkowy (koszt: 2× `micros()` na klatkę, pomijalny),
a `PROFILE_FRAME` (`:1431`) włącza już tylko dodatkowy log na Serial. **Flaga została,
opis się zestarzał.** Zostawić flagę (jest tania i przydaje się przy stole),
poprawić komentarz w `Config.h`.

---

## 4. ZOSTAWIĆ MIMO POZORÓW

### 4.1. `WeatherUi.cpp` — 2732 linie. **NIE dzielić.** To jest wynik pomiaru, nie gust.

Pytanie z briefu: „czy to realny problem, czy tylko wygląda źle?" — **wygląda źle,
nie jest źle.** Dowody:

**(a) Rozkład linii — nie ma potwora:**

| Funkcja | Linie |
|---|---:|
| `drawViewStats` | 247 |
| `drawViewHome` | 216 |
| `drawViewPv` | 193 |
| `drawViewRadar` | 154 |
| `drawViewNow` | 149 |
| `drawViewFlights` | 143 |
| `drawViewHours` | 141 |
| `render` | 127 |
| `drawViewBoiler` | 102 |
| `drawViewDays` | 81 |

**9 rendererów = 1426 linii = 52 % pliku.** Największy ma 247 linii i jest to ekran
serwisowy z ośmioma wskaźnikami — czyli tyle treści, ile widać na ekranie. Żadna
funkcja nie jest zagmatwana; są **długie liniowo**, bo rysowanie jest z natury
liniowe: postaw prostokąt, napisz tekst, narysuj słupek. Cyklomatycznie to płaskie.

**(b) To NIE jest 9 niezależnych wysp — i to jest argument PRZECIW podziałowi:**

| Pomocnik | Wywołań |
|---|---:|
| `plStr` | 47 |
| `gl` | 38 |
| `plCenter` | 37 |
| `clampf` | 19 |
| `plRight` | 16 |
| `viewHeader` | 14 |
| `lerp565` | 13 |
| `glCenter` | 13 |
| `easeOutCubic` | 12 |
| `glRight` | 12 |
| `drawNoData` | 10 |
| `tempColor` | 9 |
| `bigStr` | 6 |
| `fmtPower` | 6 |
| `smoothArc` | 5 |
| `zoneGauge` | 3 |

**~280 wywołań wspólnej warstwy.** Renderery dzielą font, kolory, easing, nagłówek
ekranu, ekran „brak danych" i formatery. To jest **jeden spójny język wizualny**,
a nie 9 kopii tego samego.

**(c) Co by dał podział — konkretnie:**
Pomocniki z linii 38–190 (~155 linii) siedzą w **anonimowej przestrzeni nazw**
(łączenie wewnętrzne). Podział na `WeatherUiViews.cpp` wymaga wypchnięcia ich do
prywatnego nagłówka jako `inline` — czyli **+1 plik, +155 linii przeniesionych,
+deklaracje**. Renderery to prywatne metody `WeatherUi` grzebiące w prywatnym stanie
(`animAcW_`, `rooms_`, `boiler_`, `burner_`, `view_`, `alert_`) — mogą mieszkać
w innej jednostce kompilacji bez zmiany ani jednej linii ciała, **ale sprzężenie
zostaje w całości**. Przeniósłbyś linie między plikami i **nic** nie uprościł.
Dokładnie to, przed czym ostrzega brief.

**Jedyny realny koszt obecnego stanu: czas kompilacji.** Zmiana jednego piksela
w `drawViewDays` przekompilowuje 2732 linie + nagłówki TFT_eSPI. Jeśli **to** zacznie
boleć — a nie „bo 2700 linii to dużo" — naturalny szew przebiega tak:

| Moduł | Co | Linie |
|---|---|---:|
| silnik | `render`, `paintFrame`, `drawView`, `setBand`, `pushBands`, `holdFor`, `prevView` | ~300 |
| widoki | 9 rendererów | ~1426 |
| ekrany statyczne | `drawBoot`, `drawSetup`, `drawOta`, `drawNetInfo`, `drawLedTest`, `drawColorTest` | ~260 |
| zrzut | `streamScreenshot`, `drawFooterTo` | ~130 |

**Rekomendacja: zostawić.** Plik jest duży, ale jednorodny — to katalog rysunków,
a nie splątana logika. Dzielenie go teraz to praca bez zwrotu.

### 4.2. `OtaGuard` (392 linie) + rollback — **ubezpieczenie, nie przerost**

Urządzenie wisi w łazience, **nieosiągalne fizycznie**, jedyna droga aktualizacji to
OTA z GitHub Releases. Rachunek jest prosty: **392 linie kontra jedyny egzemplarz
sprzętu, do którego nie ma jak podejść z kablem.** Jedna nieudana aktualizacja bez
rollbacku = koniec projektu (albo demontaż ze ściany). Przy takiej asymetrii
`OtaGuard` byłby uzasadniony nawet gdyby miał 1000 linii. **Nie ruszać.**

To samo dotyczy `hardTimeoutCb` (`OtaGuard.cpp:217`) — wygląda na nieużywany, bo woła
go **timer sprzętowy** (`:275`, `.callback = &hardTimeoutCb`), nie kod. Żywy.

### 4.3. Własne fonty (`PlFont10/14/18` + `PlText.h`) — **zarabiają na siebie**

Brief każe udowodnić liczbami, gdybym uznał je za przerost. **Nie uznaję.** Dowód
działa w drugą stronę i jest w kodzie: `WeatherUi.cpp:115-118` dokumentuje, że
wbudowany GLCD dawał „CIEP A WODA", „52.4[]C" i „m-|" **przez 5 wersji**.
Wbudowany font nie ma ani polskich znaków, ani °, ani ³ (a `drawViewBoiler:2085`
pisze „m³"). Dane fontów to `PROGMEM` — **0 B statycznego RAM**, tylko flash,
którego przy `min_spiffs` jest 1,7 MB z 1,9 MB. Koszt: zero tam, gdzie boli
(RAM). Zysk: koniec z klasą błędów, która wracała pięć razy. **Zostawić.**

Zastrzeżenie z § 2.4 dotyczy **wyłącznie 4 martwych pomocników** w `PlText.h`
(19 linii), a nie fontów. Same fonty (`font10/14/18`, `drawString`, `stringWidth`,
`drawGlyph`, `decodeUtf8`, `glyphIndex`) są używane intensywnie.

### 4.4. Komentarze (1289 linii, 13 %) — **najlepiej wydane linie w tym repo**

Zgodnie z briefem nie zgłaszam ich jako problemu — ale idę dalej: **w tym audycie
komentarze były głównym narzędziem dowodowym.**

* `WeatherUi.cpp:2100` („licznik uruchomień […] wyrzucone") **udowodnił** § 2.5a —
  bez niego uznałbym `burnerHours` za pole, którego jeszcze nie zdążono pokazać.
* `WeatherUi.cpp:115-118` („GLCD zniknął z projektu") **udowodnił** § 2.4.
* `WeatherUi.h:110-115` („BAND_N = 1 […] PSRAM") **udowodnił** § 3.1.
* `GasMeter.h:5-12` (zepsute liczniki Viessmanna) **uratował** § 2.1 przed werdyktem
  „usuń" — pokazał, że funkcja ma sens i warto ją raczej dokończyć.

Komentarz `WeatherUi.cpp:151-165` (dlaczego własny `smoothArc` zamiast
`TFT_eSPI::drawSmoothArc` — „zapis leci ~7 kB ZA koniec bufora i rozwala stertę,
sprawdzone w TFT_eSPI 2.5.43") to jest **dokładnie ten komentarz, który oszczędza
tydzień**. Nie ruszać żadnego z nich.

**Jedyny zarzut do komentarzy:** kilka z nich zestarzało się razem z kodem
(§ 3.1 — opis dwóch pasów i bufora 66 kB; § 3.3 — `PROFILE_FRAME` „na Serial").
Komentarz, który kłamie, jest gorszy niż brak komentarza — **poprawić te dwa**.

### 4.5. Płaska architektura klientów — **nie mylić z brakiem architektury**

`PvClient`, `WeatherClient`, `RadarClient`, `FlightClient`, `Viessmann` — pięć
integracji sieciowych, każda jako wolne funkcje w `namespace` wypełniające POD.
Zero wspólnej klasy bazowej, zero `IClient`. **Ktoś mógłby to nazwać brakiem
abstrakcji. To jest właściwa decyzja** i argument przemawia za projektem, nie
przeciw: te pięć klientów **nie ma wspólnego zachowania** (Modbus TCP binarnie /
JSON po HTTPS / PNG / JSON / OAuth+PKCE z odświeżaniem tokenu). Wspólny interfejs
musiałby być tak ogólny, że nic by nie znaczył. **Zostawić.**

### 4.6. Pary `g*` / `ui*` (7 par, ~5 kB RAM) — cena za brak rwania obrazu

`gWeather`/`uiWeather`, `gPv`/`uiPv`, `gHist`/`uiHist`, `gRooms`/`uiRooms` (2×1736 B!),
`gVi`/`uiVi`, `gBurner`/`uiBurner`, `gFlights`/`uiFlights`. Podwojenie ~5 kB RAM
wygląda na marnotrawstwo — **nie jest**. To jest snapshot pod mutexem
(`.ino:837-845`), dzięki któremu `netTask` nie przepisuje modelu w trakcie rysowania
klatki. Alternatywa (trzymanie `gLock` przez cały render) zablokowałaby sieć na
30–40 ms co klatkę. **5 kB za brak rwania obrazu i brak wyścigów to dobry kurs.**
Zostawić. (Wyjątek: `gGas` nie ma pary `uiGas` — bo nikt go nie czyta, § 2.1.)

---

## 5. Ustalenia dla triażu (#10)

Kolejność wg **stosunku zysku do ryzyka**, nie wg wielkości:

| # | Co | Linie | RAM | Ryzyko | Uwaga |
|---|---|---:|---:|---|---|
| 1 | § 2.2 `FOOTER_Y`/`FOOTER_H` | 2 | 0 | **zerowe** | **nieprawdziwe o 2 px** — usunąć dziś |
| 2 | § 2.4 `PlText.h` × 4 | 19 | 0 | **zerowe** | pułapka GLCD w publicznym repo |
| 3 | § 2.3 `drawFatal()` | 12 | 0 | **zerowe** | |
| 4 | § 2.6 `Ota::lastRemoteVersion()` | 3 | 4 B | **zerowe** | |
| 5 | § 2.7 2 kolory | 2 | 0 | **zerowe** | |
| 6 | § 2.5b PV: `efficiencyPct`, `meterOk` | 4 | 16 B | niskie | **−20 % transakcji Modbus** |
| 7 | § 2.5a Viessmann × 3 pola | 6 | 24 B | niskie | Twój komentarz to potwierdza |
| 8 | § 2.1 **Gaz: dokończyć albo usunąć** | ~145 | 312 B | średnie | **decyzja właściciela, nie audytora** |
| 9 | § 3.1 + § 3.3 poprawić zestarzałe komentarze | ~15 | 0 | **zerowe** | komentarz, który kłamie |
| 10 | § 3.2 wyciągnąć `cardGrow()` | −3 | 0 | niskie | tylko to jedno, reszty nie scalać |

Pozycje 1–5 to **38 linii przy zerowym ryzyku** — nic ich nie kompiluje, więc
usunięcie nie ma jak zepsuć obrazu. Warto zrobić hurtem i sprawdzić buildem:

```
cd /Users/maciuso/Desktop/Maciej.5000/esp32/pogoda-gdynia && TMPDIR=/tmp arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs,PSRAM=enabled" .
```
Oczekiwany statyczny RAM po pozycjach 1–7: **71 600 → ~71 556 B** (−44 B).
Po pozycji 8 (wariant „usuń"): **~71 244 B** (−356 B). Bariera 76 000 B nietknięta
w obu wariantach — **bo ona i tak nie była zagrożona**.

---

## 6. Czego szukałem i NIE znalazłem

Uczciwość wymaga wypisania hipotez, które się nie potwierdziły — brief kazał obalać
wrażenie dowodami, a nie tylko je potwierdzać:

* **Abstrakcje z jedną implementacją** — szukałem klas bazowych, `virtual`, fabryk.
  Jedyne `override` w projekcie (`Viessmann.cpp:30-32,47` — `allocate`/`deallocate`/
  `reallocate`/`peek`) to **wymagane** implementacje interfejsów ArduinoJson i `Stream`,
  wołane przez biblioteki. Nie martwe, nie spekulatywne. **Zero sztucznych abstrakcji.**
* **Warstwy pośrednie** — `Portal` woła `settings()` wprost, `settings()` woła
  `Preferences` wprost. Żadnego repozytorium, żadnego DAO. Płasko.
* **Konfigurowalność bez użytkownika** — `Config.h` ma 60 stałych, **wszystkie
  używane poza `FOOTER_Y`/`FOOTER_H`**. To zaskakująco dobry wynik; spodziewałem się
  cmentarza stałych.
* **Parametry zawsze z tą samą wartością** — znalazłem dokładnie **jeden**: `BAND_N = 1`
  (§ 3.1), i ten ma uzasadnienie historyczne sprzed 5 dni.
* **Duplikacja** — jedyna realna to 3× kafelek (§ 3.2), i to taka, której scalanie
  zaszkodziłoby bardziej niż pomogło. `viewHeader`/`drawNoData`/`plStr`&spółka
  pokazują, że wspólne rzeczy **już są** wyciągnięte.
* **Martwe pliki** — żadnego. Wszystkie 51 plików `.h`/`.cpp`/`.ino` są w grafie
  zależności.

**To jest profil projektu, który był RE-faktoryzowany, a nie PRE-faktoryzowany.**
Wspólne rzeczy wyciągnięto dopiero, gdy się powtórzyły (`viewHeader` — 14 użyć,
`drawNoData` — 10, `plStr` — 47). Tak się to robi. Wrażenie przerostu bierze się
z **objętości**, a objętość bierze się z **9 ekranów i 6 integracji sieciowych** —
czyli z zakresu, nie z inżynierii. To jest po prostu spory projekt, nie napuszony.

---

*Audyt D · nie edytowano żadnego pliku poza tym raportem · nie refaktoryzowano.*
