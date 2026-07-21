# KATALOG DANYCH — co to urządzenie naprawdę wie

Dokument dla projektanta interfejsu. Opisuje **wyłącznie dane**: skąd pochodzą, w jakich
jednostkach, w jakich zakresach, jak często się odświeżają i co się dzieje, gdy źródło
milczy. **Celowo nie ma tu ani słowa o obecnych ekranach, układach ani kolorach** — projekt
ma powstać od zera, z danych, a nie z podglądu tego, co już jest.

Wszystkie liczby (interwały, progi, zakresy) są wyciągnięte z kodu wersji **FW 126**, nie
zgadnięte. Tam, gdzie kod czegoś nie rozstrzyga, jest to napisane wprost.

---

## Jak czytać ten katalog

**Trzy różne rodzaje „braku danych"** — projekt musi je rozróżniać, bo urządzenie je
rozróżnia i pomylenie ich było w tym projekcie źródłem realnych błędów:

| Rodzaj | Co znaczy | Jak się objawia w danych |
|---|---|---|
| **Nigdy nie skonfigurowane** | Użytkownik nie podał adresu / nie autoryzował / nie ma czujnika | Całej grupy danych nie ma i nie będzie, dopóki ktoś nie kliknie w panelu |
| **Chwilowo niedostępne** | Źródło jest, ale ostatni odczyt się nie udał | Flaga `valid`/`ready` + komunikat błędu; wartości albo stare, albo wyzerowane (zależy od źródła — patrz niżej) |
| **Pole nie przyszło w odpowiedzi** | Źródło odpowiedziało, ale bez tej konkretnej wartości | Osobna flaga „mam to" (`hasX`); wartość wynosi wtedy 0 i **0 jest kłamstwem** |

Ostatni przypadek jest w tym projekcie traktowany bardzo serio. W kodzie stoi to wprost:
*„UI ma pokazać `—` tam, gdzie flaga jest false, a nie zero"*. Historia: bojler bez odczytu
pokazywał „CWU 0,0 °C", co było nie do odróżnienia od zamarzniętego zasobnika. Wszędzie,
gdzie w tabelach poniżej jest kolumna **flaga obecności**, brak flagi musi dać myślnik,
nigdy zero.

**Kolumna „opcjonalne"** oznacza: czy to pole może nie istnieć **nigdy**, u konkretnego
użytkownika, bo sprzęt/usługa nie jest skonfigurowana.

**Starzenie się danych.** Urządzenie prawie nigdy nie kasuje starych danych po nieudanym
odczycie — woli pokazać stare niż puste. Skutek dla projektu: **prawie każda wartość
potrzebuje możliwości pokazania swojego wieku.** Wyjątki wymienione są przy każdej grupie.

---

## 1. POGODA BIEŻĄCA

**Źródło:** Open-Meteo, `api.open-meteo.com/v1/forecast`, HTTPS, bez walidacji certyfikatu.
Współrzędne są konfigurowalne (domyślnie Gdynia, 54,4870 / 18,5216).
**Odświeżanie:** co **15 minut** (`WEATHER_REFRESH_MS`). Po nieudanym odczycie ponowienie
po **30 sekundach**. Brak wykładniczego backoffu.
**Gdy źródło milczy:** cały model pogody zostaje **nietknięty** — poprzednia prognoza
zostaje na ekranie bezterminowo. Jedyny ślad awarii to komunikat błędu w diagnostyce.
**Nie ma żadnego progu przeterminowania pogody** — dane nie „wygasają" same z siebie.
Wiek można policzyć wyłącznie z pola `weatherOkAt` (rozdz. 10).

| Nazwa | Typ / jednostka | Zakres realny | Typowa wartość | Uwagi |
|---|---|---|---|---|
| Temperatura powietrza | float, °C | −20 … +36 | 14 | |
| Temperatura odczuwalna | float, °C | −28 … +40 | 12 | Uwzględnia wiatr i wilgotność |
| Wilgotność względna | int, % | 25 … 100 | 75 | Nadmorska — rzadko poniżej 40 |
| Zachmurzenie | int, % | 0 … 100 | 60 | |
| Prędkość wiatru | float, km/h | 0 … 90 | 15 | Wiatr przyziemny, 10 m |
| Kierunek wiatru | int, ° | 0 … 359 | 270 | 0 = północ, zgodnie z zegarem |
| Kod stanu pogody | int, kod WMO | 28 wartości | 3 | Pełna lista w rozdz. 1.1 |
| Ciśnienie | float, hPa | 975 … 1040 | 1013 | Zredukowane do poziomu morza |
| Opad bieżący | float, mm | 0 … 15 | 0 | Suma z bieżącej godziny |
| **UV skorygowany** | float, indeks | 0 … 9 | 2 | **To jest wartość do pokazania** |
| UV surowy | float, indeks | 0 … 9 | 5,5 | Wartość z API, praktycznie bezchmurna |
| Dzień / noc | bool | — | true | Z API; decyduje o wariancie ikony |
| Godzina wschodu | tekst „HH:MM" | 03:30 … 08:30 | 04:32 | Tylko dziś, nie na kolejne dni |
| Godzina zachodu | tekst „HH:MM" | 15:20 … 21:20 | 21:14 | Tylko dziś |
| Maks. UV dzisiaj | float, indeks | 0 … 9 | 5 | **Bez korekty o chmury** |
| Suma opadu dzisiaj | float, mm | 0 … 40 | 1,2 | Cała doba |
| Znacznik ostatniego odczytu | tekst „HH:MM" | — | 14:15 | Czas z API, nie lokalny zegar |

**Prawdopodobieństwo opadu dla „teraz" NIE ISTNIEJE.** Pole jest w strukturze, ale nigdy
nie jest wypełniane dla wartości bieżących — zawsze wynosi 0. Prawdopodobieństwo istnieje
wyłącznie w prognozie godzinowej (rozdz. 2). Nie projektować „szansa opadu teraz".

**Dlaczego są dwa UV.** Open-Meteo twierdzi, że jego `uv_index` uwzględnia zachmurzenie,
ale w praktyce tego nie robi. Zmierzone w Gdyni 13.07 o 12:15 przy 99% zachmurzenia:
`uv_index` = 6,05 wobec `uv_index_clear_sky` = 6,25 — różnica 3%, podczas gdy realne
promieniowanie wynosiło 282 W/m² zamiast ~800. Urządzenie liczy więc własną korektę:

```
cmf = 1 − 0,75 × (zachmurzenie/100)^3,4
UV_pokazywane = UV_z_API × cmf
```

Skutek: przy 50% chmur korekta jest prawie niewidoczna (×0,93), przy 80% już wyraźna
(×0,65), przy pełnym zachmurzeniu ×0,25. **Maks. UV dzisiaj nie przechodzi przez tę
korektę** — jest surowe. Dwa pola UV na jednym widoku mogą się więc rozjeżdżać i to jest
poprawne zachowanie, nie błąd.

### 1.1 Kody pogody — 28 rozróżnialnych stanów

Urządzenie ma gotowe polskie opisy dla wszystkich 28 kodów WMO. To jest istotne dla
projektu: **jest z czego robić opis słowny i nie trzeba go sprowadzać do ośmiu ogólników.**

| Kod | Opis | Kod | Opis |
|---|---|---|---|
| 0 | Bezchmurnie *(nocą: Czyste niebo)* | 66 | Marznący deszcz |
| 1 | Prawie bezchmurnie | 67 | Silny marznący deszcz |
| 2 | Częściowe zachmurzenie | 71 | Słabe opady śniegu |
| 3 | Zachmurzenie całkowite | 73 | Opady śniegu |
| 45 | Mgła | 75 | Intensywny śnieg |
| 48 | Mgła i szron | 77 | Krupa śnieżna |
| 51 | Słaba mżawka | 80 | Przelotny deszcz |
| 53 | Mżawka | 81 | Przelotne opady |
| 55 | Gęsta mżawka | 82 | Ulewne opady |
| 56 | Marznąca mżawka | 85 | Przelotny śnieg |
| 57 | Gęsta marznąca mżawka | 86 | Silne opady śniegu |
| 61 | Słaby deszcz | 95 | Burza |
| 63 | Deszcz | 96 | Burza z gradem |
| 65 | Silny deszcz | 99 | Burza z silnym gradem |

Najdłuższy opis to „Gęsta marznąca mżawka" (21 znaków), najdłuższe pojedyncze słowo:
„Zachmurzenie". Opis ma wariant nocny tylko dla kodów 0–2.

### 1.2 Faza księżyca — liczona lokalnie, zawsze dostępna

Nie pochodzi z żadnego API — jest liczona z kalendarza (miesiąc synodyczny 29,530588853
dnia, punkt odniesienia: nów 6 stycznia 2000, 18:14 UTC). **Nigdy nie zawodzi**, o ile
zegar NTP jest zsynchronizowany; bez zegara zwraca pełnię.

| Nazwa | Typ / jednostka | Zakres | Uwagi |
|---|---|---|---|
| Faza | float, 0…1 | 0 = nów, 0,25 = I kwadra, 0,5 = pełnia, 0,75 = III kwadra | |
| Oświetlona część tarczy | float, 0…1 | 0 … 1 | Do rysowania terminatora |
| Nazwa fazy | tekst | 8 wartości | Nów, Sierp przybywa, Pierwsza kwadra, Garb przybywa, Pełnia, Garb ubywa, Ostatnia kwadra, Sierp ubywa |

Konwencja półkuli północnej: księżyc przybywający oświetlony **z prawej**, ubywający
**z lewej**.

---

## 2. PROGNOZA GODZINOWA

**Liczba punktów czasowych: 12** (`WX_HOURS`), od **+1 h do +12 h**. Z API pobierane jest
14 godzin — dwie na zapas, żeby zawsze dało się znaleźć pierwszą godzinę w przyszłości.
**Źródło i odświeżanie:** identyczne jak pogoda bieżąca (jeden zapytanie HTTP obsługuje
wszystko).

| Nazwa | Typ / jednostka | Zakres realny | Typowa | Uwagi |
|---|---|---|---|---|
| Przesunięcie godzinowe | int, h | 1 … 12 | — | Zawsze wypełnione, nawet gdy slot nieważny |
| Godzina zegarowa | int, 0…23 | 0 … 23 | — | Czas lokalny (CET/CEST) |
| Temperatura | float, °C | −20 … +36 | 14 | |
| Kod pogody | int, WMO | tabela 1.1 | 3 | |
| Opad | float, mm/h | 0 … 20 | 0 | |
| **Prawdopodobieństwo opadu** | int, % | 0 … 100 | 20 | Istnieje **tylko tutaj** |
| Prędkość wiatru | float, km/h | 0 … 90 | 15 | |
| Dzień / noc | bool | — | — | Osobno dla każdej godziny |
| Ważność slotu | bool | — | — | Patrz niżej |

**Prognoza godzinowa może być częściowo pusta.** Jeśli nie uda się znaleźć pierwszej
godziny w przyszłości (rozjazd zegara, dziwna odpowiedź API), **wszystkie 12 slotów są
oznaczone jako nieważne**, ale nadal mają wypełnione przesunięcie godzinowe. Podobnie
sloty wykraczające poza tablicę z API. Projekt musi umieć narysować „mam 7 z 12 godzin"
— nie zakładać, że jest komplet albo nic.

**Wilgotność, ciśnienie i UV nie występują w prognozie godzinowej** — tylko w bieżącej.

---

## 3. PROGNOZA DZIENNA

**Liczba punktów czasowych: 5** (`WX_DAYS`), od **jutra do +5 dni**. Z API pobierane jest
6 dni; dzień zerowy (dzisiaj) jest zużywany wyłącznie na wschód/zachód słońca, maks. UV
i sumę opadu dzisiaj — **nie ma go w tablicy dni**.

| Nazwa | Typ / jednostka | Zakres realny | Typowa | Uwagi |
|---|---|---|---|---|
| Przesunięcie dobowe | int, dni | 1 … 5 | — | 1 = jutro |
| Temperatura maksymalna | float, °C | −15 … +36 | 18 | |
| Temperatura minimalna | float, °C | −20 … +24 | 9 | |
| Suma opadu | float, mm | 0 … 60 | 1,5 | Cała doba |
| Maksymalny UV | float, indeks | 0 … 9 | 5 | **Bez korekty o chmury** |
| Maksymalny wiatr | float, km/h | 0 … 110 | 25 | |
| Kod pogody | int, WMO | tabela 1.1 | 3 | Reprezentatywny dla doby |
| Nazwa dnia | tekst, max 7 zn. | — | „PON" | NDZ, PON, WT, ŚR, CZW, PT, SOB |
| Data | tekst „DD.MM" | — | „13.07" | |

**Nie ma wschodu/zachodu dla kolejnych dni** — tylko dla dzisiaj. Nie ma też prawdopodobieństwa
opadu ani wilgotności w prognozie dziennej.

---

## 4. OPADY I RADAR

Urządzenie ma **dwa niezależne systemy radarowe**, z różnych zapytań i o różnej geometrii.
Mogą się ze sobą nie zgadzać i to jest normalne. Oba korzystają z **RainViewer**
(`api.rainviewer.com`), po zwykłym HTTP bez TLS (świadoma decyzja — oszczędza ~40 kB pamięci
potrzebnej dekoderowi PNG).

**To jest realny pomiar radarowy, nigdy prognoza.** Darmowe API zwraca zero klatek
nowcastu, więc wszystko, co urządzenie ma, to zmierzona przeszłość.

### 4.1 Radar punktowy — natężenie opadu nad lokalizacją

**Odświeżanie:** co **5 minut** (`RADAR_REFRESH_MS`), po błędzie ponowienie po **60 s**.
**Zasięg przestrzenny:** jeden kafelek zoom 7 (~710 m/piksel); wartość to **maksimum z okna
5×5 pikseli** (~3,5 km) wokół dokładnej lokalizacji.

| Nazwa | Typ / jednostka | Zakres | Uwagi |
|---|---|---|---|
| Poziom opadu | uint8, 0…5 | 0 … 5 | Skala w tabeli 4.3 |
| Wiek pomiaru | uint32, sekundy | 0 … ~900 | 0 = zegar nieznany, nie „świeże"! |
| Ważność | bool | — | |

### 4.2 Radar mapowy — animowana mapa opadów

| Właściwość | Wartość |
|---|---|
| **Liczba klatek** | **13** |
| **Odstęp między klatkami** | **10 minut** |
| **Pokrywany czas** | **−120 min … 0**, czyli dokładnie **2 godziny wstecz** |
| **Rozmiar rastra** | **320 × 172 piksele**, 1 bajt na piksel |
| **Rozdzielczość** | **~937,5 m/piksel** (identyczna w obu osiach, celowo) |
| **Obszar** | szer. geogr. **53,8457 – 55,2943**, dł. geogr. **16,2756 – 20,9244** |
| **Rozmiar terenu** | **~300 km w poziomie × ~161 km w pionie**, wyśrodkowany na Zatoce Gdańskiej |
| Pamięć | 13 × 55 040 B = **~715 kB** w PSRAM |
| **Odświeżanie** | co **10 minut**, po błędzie ponowienie po **2 minutach** |

Dodatkowo model niesie **wektor przesunięcia chmur** (przesunięcie X i Y w pikselach),
liczony z prędkości i kierunku wiatru pomnożonych przez współczynnik **2,0**. Powód: fronty
płyną z wiatrem na wysokości echa radarowego (~3 km), typowo 1,5–2,5× szybszym niż wiatr
przyziemny — jedyny, jaki mamy z API. **To jest jawne przybliżenie, nie pomiar.** Wektor
wynosi zero, gdy jest cisza, gdy pogoda jest nieświeża albo dla najnowszej klatki.

Model niesie też flagę **„czy w którejkolwiek klatce jest opad"** — liczoną przez próbkowanie
co siódmego piksela wszystkich ważnych klatek, próg to ponad 60 mokrych próbek. Ta flaga jest
projektowo ważna: mówi, czy jakikolwiek widok radaru ma w ogóle sens.

### 4.3 Poziomy opadu — co znaczą

**Nie są to mm/h ani dBZ.** Poziomy są odtwarzane z kolorów renderowanych przez RainViewer
— to uporządkowanie natężenia, nie jednostka fizyczna. Nigdzie w kodzie nie ma przeliczenia
na milimetry.

| Poziom | Znaczenie | Etykieta w kodzie |
|---|---|---|
| 0 | Brak opadu | bez opadu |
| 1 | Ślady / mżawka | mżawka |
| 2 | Deszcz | deszcz |
| 3 | Silny deszcz | silny deszcz |
| 4 | Bardzo silny | bardzo silny |
| 5 | Ulewa | ULEWA |

Poziom 3 jest osiągalny z dwóch różnych rodzin kolorów (granat i żółć) — paleta źródłowa
jest gradientem beż → błękit → żółć → pomarańcz → czerwień i te dwie gałęzie się tam
spotykają.

### 4.4 Zachowanie przy awarii — trzy poziomy „wszystko albo nic"

To jest wyjątkowo istotne dla projektu, bo różni się od reszty urządzenia:

1. **Całe pobranie:** jeśli serwer oferuje mniej niż 13 klatek (typowe przez kilkanaście
   minut po awarii RainViewera), **całe pobranie jest odrzucane**. Stare klatki zostają.
2. **Pojedyncza klatka:** klatka liczy się za udaną tylko, gdy **wszystkie jej kafelki**
   dojechały i zdekodowały się. Jeden zły kafelek unieważnia całą klatkę — „pół mapy z tej
   chwili i pół sprzed 10 minut to gorsze kłamstwo niż uczciwa dziura".
3. **Klatka nieważna:** jest **zerowana w całości**, ale zachowuje swój prawdziwy znacznik
   czasu. Nigdy nie zostają w niej stare piksele.

**Licznik klatek zawsze zwraca 13**, także gdy część klatek jest pusta — nieważne klatki są
puste, a nie nieobecne. Animacja od 0 do 12 jest więc zawsze bezpieczna, ale **może zawierać
puste kadry w środku**. Projekt musi to znieść.

Przy krytycznie niskiej pamięci (poniżej 64 kB wolnej sterty) radar punktowy **odpuszcza
cały cykl** i podbija licznik pominięć. Na realnym urządzeniu ten licznik wynosi zero.

---

## 5. JAKOŚĆ POWIETRZA

**Źródło:** miejska sieć ARMAAG / sensorbox (`pomorskie.cas.sensorbox.pl`), zwykłe HTTP.
Dane to **średnie godzinowe** — nowa próbka pojawia się raz na godzinę.
**Odświeżanie:** co **15 minut** (`AIR_REFRESH_MS`), po błędzie ponowienie po **30 s**.
Wymaga działającego zegara NTP; bez niego ponawia co 5 s i **nie liczy tego jako błąd**.

**Dwie stacje z automatycznym przełączaniem:**

- **GA17 „SANDOMIERSKA"** (Sandomierska 3, Mały Kack) — stacja **główna**.
- **GA24 „HALICKA"** (Halicka 8) — **zapas**, włączany wyłącznie wtedy, gdy GA17 nie ma nic
  świeższego niż **3 godziny** (`AIR_STALE_S`).

Warunek przełączenia to **alternatywa, nie koniunkcja**: wystarczy, że **jeden** z dwóch
pyłomierzy GA17 jest świeży, żeby zostać przy stacji głównej.

| Nazwa | Typ / jednostka | Zakres realny | Typowa | Flaga obecności | Opcjonalne |
|---|---|---|---|---|---|
| PM10 | float, µg/m³ | 2 … 200 | 18 | tak | nie |
| PM2,5 | float, µg/m³ | 1 … 150 | 11 | tak | nie |
| Indeks cząstkowy PM10 | int, 1…6 | 1 … 6 | 2 | 0 = brak | nie |
| Indeks cząstkowy PM2,5 | int, 1…6 | 1 … 6 | 2 | 0 = brak | nie |
| **Indeks ogólny** | int, 1…6 | 1 … 6 | 2 | 0 = brak | nie |
| Nazwa stacji | tekst, max 15 zn. | — | „SANDOMIERSKA" | — | nie |
| Czy zapas | bool | — | false | — | nie |
| Czas próbki | epoch UTC | — | — | 0 = nieznany | nie |
| Temperatura | float, °C | −15 … +33 | 14 | **wspólna flaga** | **tak** |
| Wilgotność | float, % | 30 … 100 | 78 | **wspólna flaga** | **tak** |
| Ciśnienie | float, hPa | 975 … 1040 | 1013 | **wspólna flaga** | **tak** |

**Temperatura, wilgotność i ciśnienie pochodzą wyłącznie z GA17** i **znikają całkowicie**,
gdy urządzenie przełączy się na GA24 — bo pokazywanie ich pod nagłówkiem „HALICKA"
sugerowałoby pomiar spod Sandomierskiej. Mają jedną wspólną flagę: albo są wszystkie trzy,
albo żadnej. To znaczy, że **te trzy pola potrafią zniknąć w środku normalnej pracy**, gdy
stacja główna zamilknie — bez żadnej awarii.

**Nazwa stacji jest daną pierwszorzędną, nie ozdobnikiem.** Bez niej użytkownik pomyliłby
dwa różne punkty pomiarowe.

### 5.1 Tabela indeksu ARMAAG

To **regionalna tabela trójmiejska**, nie ogólnopolska GIOŚ. Granice górne włącznie, µg/m³.

| Indeks | PM10 | PM2,5 | Etykieta |
|---|---|---|---|
| 1 | 0 – 20 | 0 – 13 | BARDZO DOBRE |
| 2 | 20,1 – 50 | 13,1 – 35 | DOBRE |
| 3 | 50,1 – 80 | 35,1 – 55 | ZADOWALAJĄCE |
| 4 | 80,1 – 110 | 55,1 – 75 | DOSTATECZNE |
| 5 | 110,1 – 150 | 75,1 – 110 | ZŁE |
| 6 | > 150 | > 110 | BARDZO ZŁE |
| 0 | — | — | BRAK DANYCH |

**Indeks ogólny to MAKSIMUM z dwóch cząstkowych, nigdy średnia.** Uśrednienie PM10 = 155
(indeks 6) z PM2,5 = 5 (indeks 1) dałoby ~3–4, czyli „znośnie" w dzień realnego smogu.
Urządzenie trzyma **wszystkie trzy** indeksy, żeby dało się pokazać, **który składnik
ustala ten gorszy wynik** — to jest informacja projektowo użyteczna, nie nadmiarowa.

**Uwaga na pułapkę:** próg 3 godzin decyduje **tylko o wyborze stacji**, nie filtruje
wyświetlanych wartości. Jeśli GA17 ma świeży PM10 i przeterminowany PM2,5, **oba są
pokazywane**. Dane z GA24 nie są sprawdzane pod kątem świeżości **w ogóle**.

**Gdy źródło milczy:** ostatnia dobra próbka zostaje, a jej wiek rośnie w nieskończoność.
Przed pierwszym udanym odczytem cała grupa jest oznaczona jako niegotowa.

---

## 6. FOTOWOLTAIKA

**Źródło:** falownik Huawei SUN2000, **Modbus TCP** w sieci lokalnej (port domyślnie 502).
Adres IP konfiguruje użytkownik — **bez niego całej grupy nie ma**.
**Odświeżanie:** co **30 sekund** (`PV_REFRESH_MS`); w oknie nocnym co **5 minut**
(`PV_REFRESH_NIGHT_MS`). Powrót do trybu szybkiego następuje natychmiast, gdy falownik
odpowie.

| Nazwa | Typ / jednostka | Zakres realny | Typowa (południe) | Opcjonalne |
|---|---|---|---|---|
| Moc DC (z paneli) | int32, W | 0 … 6600 | 3200 | tak (cała grupa) |
| Moc AC (do domu/sieci) | int32, W | 0 … 6000 | 3100 | tak |
| **Moc na liczniku sieci** | int32, W | −8000 … +6000 | +1800 | tak |
| **Zużycie domu** | int32, W | 0 … 8000 | 1300 | tak — **wyliczane** |
| Energia dzisiaj | float, kWh | 0 … 40 | 18 | tak |
| Energia całkowita | float, kWh | tysiące | 21 400 | tak |
| Napięcie stringu PV | float, V | 0 … 600 | 420 | tak |
| Temperatura falownika | float, °C | −20 … 80 | 42 | tak |
| Sprawność | float, % | 0 … 99 | 97,5 | tak |
| Kod stanu falownika | uint16 | tabela 6.1 | 0x0200 | tak |
| Licznik sieci obecny | bool | — | true | tak |

**Znak mocy sieciowej: dodatnia = oddajemy do sieci, ujemna = pobieramy.**
**Zużycie domu nie jest mierzone** — jest liczone jako `moc AC − moc sieciowa`, i tylko
wtedy, gdy oba odczyty się powiodły.

**Moc szczytowa instalacji** (domyślnie 6000 W) jest wartością konfiguracyjną — nadaje się
na mianownik dla wskaźników procentowych.

### 6.1 Stany falownika

| Kod | Etykieta |
|---|---|
| 0x0000–0x0003 | Czuwanie |
| 0x0100 | Start |
| 0x0200 | Praca |
| 0x0201 | Praca (limit) |
| 0x0202 | Praca (derating) |
| 0x0300 | **AWARIA** |
| 0x0301–0x0308 | Wyłączony |
| 0x0500 – 0x0900 | Test |
| 0xA000 | Brak słońca |
| **cokolwiek innego** | **Stan nieznany** |

„Stan nieznany" jest celowy. Wcześniej nieznane kody mapowały się na uspokajające „Praca",
czyli ekran twierdził, że wszystko gra, gdy falownik zgłaszał coś, czego nie znamy.

### 6.2 Noc to nie awaria

Huawei **wyłącza Modbus TCP po zachodzie**. Milczenie falownika w nocy jest stanem
normalnym, nie usterką, i model niesie na to osobną flagę („śpi"), różną od „offline".

Okno snu: od **30 minut po zachodzie** do **30 minut po wschodzie**. Margines jest w obie
strony ten sam, bo falownik potrafi odpowiadać jeszcze chwilę po zmroku, a rano budzi się
z opóźnieniem (rozgrzewka Modbusa sięga ~100 s). **Bez godzin wschodu/zachodu z prognozy
okno nie działa i urządzenie pokazuje uczciwy błąd zamiast zamiatać awarię pod dywan.**

### 6.3 Profil produkcji dnia bieżącego

| Właściwość | Wartość |
|---|---|
| Liczba próbek | **144** |
| Rozdzielczość | **10 minut** |
| Horyzont | **doba bieżąca**, reset o północy |
| Serie | **dwie**: produkcja PV [W] i pobór domu [W] |
| Flaga wypełnienia | osobna dla każdego slotu |
| Trwałość | zapis do pamięci nieulotnej co **5 minut** |

Dwie serie, bo pobór potrafi przewyższyć produkcję (dobieramy z sieci) — obie muszą się
mieścić w jednej skali. Sloty niewypełnione to **dziury, nie zera**.

### 6.4 Zachowanie przy awarii — PV jest wyjątkiem!

**To jedyne źródło w całym urządzeniu, które przy nieudanym odczycie ZERUJE widoczne
wartości** zamiast zostawić stare. Po nieudanym cyklu wszystkie liczby to zera z flagą
„nieważne".

**Konsekwencja dla projektu: nigdy nie wnioskować ze samych liczb.** „0 W produkcji"
oznacza albo noc, albo awarię łącza — rozstrzyga wyłącznie flaga ważności.

**Trzy pola nie mają żadnego zabezpieczenia** — napięcie stringu, temperatura falownika
i sprawność. Przy nieudanym odczycie **tych konkretnych rejestrów** zostają zerami i
docierają do interfejsu jako pomiar. „0,0 °C" zimą wygląda całkiem wiarygodnie. To jest
znana, nienaprawiona luka i projekt powinien traktować te trzy wartości jako mniej pewne
niż resztę.

Profil dnia (rozdz. 6.3) **nie jest** zerowany — historia przeżywa awarię.

---

## 7. OGRZEWANIE I GAZ

**Źródło:** piec Viessmann Vitodens 050-W przez **chmurowe API ViCare**. Nie lokalnie —
moduł WiFi pieca nie nasłuchuje na żadnym porcie, łączy się wyłącznie na zewnątrz.
**Wymaga jednorazowej autoryzacji w przeglądarce.** Bez niej **całej grupy nie ma.**
**Odświeżanie:** co **3 minuty** przy powodzeniu, co **2 minuty** po błędzie (szybciej!).
Limity API: 120 zapytań / 10 min, 1450 / dobę — przy 3 minutach zużywamy 480/dobę.

| Nazwa | Typ / jednostka | Zakres realny | Typowa | Flaga obecności | Opcjonalne |
|---|---|---|---|---|---|
| Temperatura CWU | float, °C | 10 … 65 | 48 | tak | tak |
| Zadana temp. CWU | float, °C | 10 … 60 | 50 | tak | tak |
| Tryb CWU | tekst | `comfort` / `eco` / `off` | comfort | **BRAK** | tak |
| Temperatura zasilania | float, °C | 15 … 80 | 32 | tak | tak |
| Palnik pracuje | bool | — | false | tak | tak |
| Modulacja palnika | int, % | 0 … 100 | 0 | tak | tak |
| **Godziny pracy palnika** | float, h | 0 … tysiące | 1847,23 | tak | tak |
| **Liczba startów palnika** | uint32 | 0 … dziesiątki tys. | 24 310 | tak *(osobna)* | tak |
| Tryb obiegu | tekst | `heating` / `standby` | standby | **BRAK** | tak |
| Zadana temp. obiegu | float, °C | 2 … 80 | 20 | tak | tak |
| Gaz na CWU dzisiaj | float, m³ | 0 … 15 | 0,8 | wspólna | tak |
| Gaz na CO dzisiaj | float, m³ | 0 … 15 | 0,0 | wspólna | tak |
| Ciepło na CWU dzisiaj | float, kWh | 0 … 150 | 7 | wspólna | tak |
| Ciepło na CO dzisiaj | float, kWh | 0 … 150 | 0 | wspólna | tak |
| Prąd zużyty przez piec | float, kWh | 0 … 2 | 0,3 | **BRAK** | tak |
| Siła sygnału WiFi pieca | int, dBm | −90 … −30 | −62 | tak | tak |
| Dni do wygaśnięcia autoryzacji | int, dni | 0 … 180 | 142 | −1 = nieznane | tak |

**Trzy pola nie mają flagi obecności** — tryb CWU, tryb obiegu i prąd pieca. Pusty tekst
oznacza „cecha nie przyszła", nie „tryb jest pusty". Traktować pusty tekst jak myślnik.

**Liczniki kumulacyjne palnika to jedyne dane o piecu odporne na aliasing.** Odpytujemy co
3 minuty, a cykl grzania CWU trwa czasem krócej — cykl, który zaczął się i skończył między
dwoma odpytaniami, dla „palnik pracuje" i „modulacja" **nie istniał**. Stąd bierze się
sytuacja „licznik gazu pokazuje 1,1 m³ na dobę przy modulacji cały czas zero". Różnica
dwóch odczytów licznika mówi, ile palnik chodził, nawet jeśli **ani razu nie złapaliśmy go
w akcji**. Rozdzielczość licznika godzin to **0,01 h = 36 sekund**.

**Odczyt CWU jest obowiązkowy.** Jeśli go brak, **cała odpowiedź jest odrzucana** — bo
alternatywą był komplet zer podany jako świeży pomiar („CWU 0,0 / zadana 0 / zasilanie 0,0
/ palnik wyłączony / gaz 0,0"), co zimą czyta się jak „kotłownia stanęła".

**Gdy źródło milczy:** poprzednie wartości **zostają** (odwrotnie niż PV), zmienia się tylko
flaga ważności. Flaga „online" **nie jest** wiarygodnym sygnałem świeżości — wiek trzeba
liczyć z osobnego znacznika czasu (rozdz. 10).

**Autoryzacja wygasa.** Token odświeżania żyje ~180 dni i **nie odnawia się sam po
wygaśnięciu** — użytkownik musi raz kliknąć w przeglądarce. To jest przewidywalny, powtarzalny
stan degradacji, wart własnego potraktowania w projekcie (jest na to licznik dni).

### 7.1 Historia gazu — własny dziennik

API Viessmanna ma zepsute agregaty (zweryfikowane 15.07.2026: „ostatnie 7 dni" = 5,8 m³,
ale „bieżący miesiąc" = 5,3 m³, czyli **mniej niż ostatni tydzień**, i „bieżący rok" = 5,3
przy 4-letniej instalacji). Ufamy wyłącznie wartości dobowej i budujemy własny szereg.

| Właściwość | Wartość |
|---|---|
| Liczba dni | **120** |
| Rozdzielczość | **0,01 m³** |
| Horyzont | **120 dni** |
| Zapis do pamięci | raz na dobę, przy przewinięciu daty |

Dodatkowo użytkownik może wpisać ręcznie do **8 odczytów fizycznego licznika gazu** (data +
stan w m³) — do porównania z tym, co twierdzi piec.

### 7.2 Profil pracy palnika

| Właściwość | Wartość |
|---|---|
| Liczba próbek | **144** |
| Rozdzielczość | **10 minut** |
| Horyzont | **doba bieżąca** |
| Wartość | modulacja **0 … 100 %** |
| Flaga wypełnienia | osobna dla każdego slotu |

Przechowywana jest **modulacja, nie gaz** — piec nie udostępnia chwilowego przepływu gazu,
a licznik dobowy ma rozdzielczość 0,1 m³ przy trzyminutowym przyroście rzędu 0,002 m³,
czyli pięćdziesiąt razy poniżej rozdzielczości. Wykres gazu byłby schodami sterowanymi
szumem. **Pole pod krzywą modulacji to zużyty gaz.**

---

## 8. CZUJNIKI W POMIESZCZENIACH

**Źródło:** czujniki Xiaomi LYWSD03MMC (i zgodne: pvvx, ATC, MiBeacon, Qingping), nasłuch
**pasywnego rozgłaszania BLE**. Bez parowania i bez łączenia — czujnik sam nadaje ramkę co
kilka/kilkanaście sekund.
**Dwie drogi odbioru:** własne radio urządzenia **oraz** do **3 bramek** (Shelly / ESP32-C3)
odpytywanych po HTTP.
**Odświeżanie:** własny nasłuch **4 sekundy co 20 sekund**; bramki odpytywane **co 20 sekund**.

**Pojemność: do 8 czujników.** Nazwy pokoi i klucze szyfrujące konfiguruje użytkownik, ale
**tylko 6 pierwszych slotów** ma miejsce w historii — czujnik wpisany ponad ten limit
zapisze się i nie dostanie ani historii, ani przypisanego identyfikatora pokoju.

| Nazwa | Typ / jednostka | Zakres realny | Typowa | Uwagi |
|---|---|---|---|---|
| Nazwa pokoju | tekst, max 23 zn. | — | „Łazienka Góra" | **Może być adresem MAC** |
| Temperatura | float, °C | −40 … +85 (sanity) / 5 … 30 realnie | 22,4 | flaga obecności |
| Wilgotność | float, % | 0 … 100 | 55 | flaga obecności |
| **Wiek odczytu** | uint32, sekundy | 0 … 9999 | 35 | **9999 = nigdy się nie odezwał** |
| Siła sygnału | int16, dBm | −100 … −40 | −72 | **0 = brak świeżego źródła** |
| Bateria | int8, % | 0 … 100 | 87 | **0 = nieznana** |
| Źródło odczytu | bool | — | — | własne radio / bramka |
| Identyfikator pokoju | int8 | −1 … 5 | 0 | **−1 = brak wpisu w ustawieniach** |
| Ważność wiersza | bool | — | — | jakikolwiek odczyt się zdekodował |

**Dwie liczby czujników, celowo różne:**
- **ile ma odczyt** — czujniki z zdekodowaną temperaturą lub wilgotnością;
- **ile widzi warstwa nasłuchu** — łącznie z tymi bez odczytu (obce nadajniki w bloku też
  zajmują sloty).

**Czujnik bez wpisu w ustawieniach jest podpisany adresem MAC.** To realny, częsty stan —
projekt musi znieść etykietę w postaci `a4:c1:38:54:f9:a9` tam, gdzie normalnie stoi
„Sypialnia".

**Dwa różne progi świeżości — nie mylić:**

| Próg | Co rozstrzyga |
|---|---|
| **90 sekund** | **Który pomiar siły sygnału jest wiarygodny** — arbitraż między własnym radiem a bramką. Poza tym progiem siła sygnału to 0, czyli „nie wiem". |
| **900 sekund (15 min)** | **Kiedy odczyt uznajemy za nieaktualny.** To jest próg „brak łączności" dla całego wiersza. |

Uwaga na subtelność: znacznik „ostatnio słyszany" jest odświeżany także wtedy, gdy ramka
przyszła, ale **nie dała się rozszyfrować** (brak klucza, zły klucz). Czujnik może więc mieć
świeży wiek i nie nieść żadnego odczytu.

**Osobny stan: „widzę czujnik, ale brakuje klucza".** Fabryczne oprogramowanie Xiaomi
szyfruje rozgłaszanie i wymaga klucza wyciągniętego z chmury. To jest odrębny przypadek od
„czujnika nie ma" i wart własnego potraktowania.

### 8.1 Historia temperatury w pokojach

| Właściwość | Wartość |
|---|---|
| Liczba próbek | **144** |
| Rozdzielczość | **10 minut** |
| Horyzont | **24 godziny, okno ruchome** (nie doba kalendarzowa) |
| Liczba pokoi | **6** |
| Wielkość | **wyłącznie temperatura** |
| Trwałość | zapis do pamięci nieulotnej co **10 minut**; przeżywa zanik zasilania |

**Wilgotności w historii nie ma** — świadomie usunięta, bo dawała drugą linię na każdy pokój.
Czas, w którym urządzenie nie działało, jest zapisany jako **dziura, nie zero**.
Historia **nie zapisuje się przed synchronizacją zegara**.

---

## 9. SAMOLOTY

**Źródło:** ADS-B z `opendata.adsb.fi`, promień **40 mil morskich** (~74 km) wokół
skonfigurowanej lokalizacji. Trasy (skąd–dokąd) pochodzą z **drugiego, innego serwisu**
(`vrs-standing-data.adsb.lol`).

**Odświeżanie:** **co 15 sekund, ale tylko wtedy, gdy dane są potrzebne.** Urządzenie nie
odpytuje ADS-B w tle bez przerwy — istnieje mechanizm zapowiedzi z wyprzedzeniem **6 sekund**,
żeby dane były gotowe, zanim będą pokazane. Po błędzie ponowienie po **20 sekundach**.

**Maksymalnie 6 samolotów** w liście.

| Nazwa | Typ / jednostka | Zakres realny | Typowa | Uwagi |
|---|---|---|---|---|
| Znak wywoławczy | tekst, max 9 zn. | 3–8 znaków | „LOT3821" | Spacje usunięte |
| Typ maszyny | tekst, max 5 zn. | ICAO | „B738" | **Może być pusty** |
| Trasa | tekst, max 11 zn. | — | „WAW-GDN" | Pusta, gdy nieznana |
| Lotnisko startu | tekst IATA | — | „WAW" | |
| Lotnisko docelowe | tekst IATA | — | „GDN" | |
| Szerokość geogr. | float, ° | 54,3000 … 54,8400 | — | Przycięte do prostokąta mapy |
| Długość geogr. | float, ° | 17,9999 … 19,2109 | — | |
| Wysokość | int32, stopy | 0 … 41 000 | 34 000 | Ruch naziemny odfiltrowany |
| Prędkość | int16, węzły | 0 … 550 | 430 | |
| Kurs | int16, ° | 0 … 359 | 245 | **0 znaczy też „nie wiem"** |
| Czy trasa znana | bool | — | false | Często false |

**Dwie różne liczby samolotów:**
- **ile jest w liście** — 0 … 6;
- **ile jest w zasięgu mapy** — wszystkie, które przeszły filtry, typowo **10–50** nad
  Trójmiastem. To nie jest surowa liczba z API ani liczba w promieniu 40 Mm.

**Kolejność listy nie jest po odległości.** Najpierw wybieranych jest 18 najbliższych, potem
sortowanie priorytetowe: **trasa dotyka Gdańska** → trasa znana → reszta (lokalne lotnictwo
ogólne). Skutek do świadomej akceptacji: samolot lecący do Gdańska, który jest 19. co do
odległości, **nigdy nie awansuje**.

**Trasa jest często nieznana** — lokalne lotnictwo ogólne, cele tylko-MLAT, zimna pamięć
podręczna albo 10-minutowa blokada serwisu tras po błędzie. Interfejs musi wyglądać sensownie
z samym znakiem wywoławczym.

**Kurs równy 0 jest niejednoznaczny** — oznacza zarówno „dokładnie na północ", jak i „brak
danych o kursie". Nie da się tego rozróżnić.

**Gdy źródło milczy:** poprzednia lista **zostaje** na ekranie. **Nie ma żadnego znacznika
wieku danych o samolotach** — po dłuższej awarii pokazywane są pozycje sprzed wielu minut
bez jakiejkolwiek informacji o tym. To jedyna grupa danych, gdzie starzenie jest całkowicie
niewidoczne, i warto to w projekcie naprawić.

---

## 10. STAN URZĄDZENIA I DIAGNOSTYKA

Grupa najbogatsza liczbowo i najbardziej „inżynierska". Poniżej to, co ma szansę być
użyteczne w interfejsie; pełna migawka ma ~70 pól.

### 10.1 Świeżość źródeł — po jednym znaczniku na źródło

Dla **każdego** z sześciu źródeł sieciowych (pogoda, PV, radar, samoloty, powietrze, MQTT)
oraz dla pieca istnieje para: **znacznik ostatniego udanego odczytu** + **tekst ostatniego
błędu** (max 48–56 znaków). To jest jedyna droga do pokazania wieku danych dla pogody
i samolotów.

Komunikaty błędów są **po polsku i przeznaczone dla człowieka**, np.: „Falownik nie
odpowiada", „Modbus bez odpowiedzi", „Brak rejestru mocy 37113", „Ustaw IP falownika
w panelu", „Brak danych ADS-B", „Radar: brak kafelka", „Za mało RAM (48 320 B)",
„brak autoryzacji", „Brak czasu NTP".

### 10.2 Zasoby i kondycja

| Nazwa | Typ / jednostka | Zakres | Typowa | Odświeżanie |
|---|---|---|---|---|
| Czas pracy | sekundy | 0 … tygodnie | — | na żywo |
| Wolna pamięć RAM | bajty | 25 000 … 160 000 | 90 000 | na żywo |
| Minimalna wolna pamięć | bajty | — | 48 000 | narastająco |
| PSRAM całkowita / wolna | bajty | — | 8 MB / ~7 MB | na żywo |
| **Temperatura układu** | float, °C | 20 … 125 | 52 | **co 10 s** |
| Siła sygnału WiFi | int, dBm | −90 … −30 | −58 | na żywo |
| Rozmiar firmware / flasha | bajty | — | — | **raz przy starcie** |
| Zapas stosu (2 zadania) | bajty | — | — | co 250 ms |
| Wersja firmware | int | — | 126 | stała |

Progi zdrowia obecne w kodzie: pamięć — **25 000 B** (krytycznie, radar nie ma jak
zdekodować PNG), **45 000 B** (ostrzegawczo), **160 000 B** (pełna skala).
Temperatura — skala **20 … 125 °C**, spokojnie do **70 °C**, gorąco powyżej **90 °C**,
kreska katalogowa na **85 °C**.

**Temperatura układu to temperatura struktury krzemu, nie otoczenia.** Nie nadaje się na
„temperaturę w pomieszczeniu" i nie wolno jej tak podpisać.

### 10.3 Wydajność rysowania

Trzy czasy w mikrosekundach: rysowanie klatki, wypchnięcie na wyświetlacz oraz
**rzeczywisty okres klatki** (uśredniany). To jedyne źródło informacji o realnej płynności.

### 10.4 Restarty, awarie, aktualizacje

| Nazwa | Typ | Uwagi |
|---|---|---|
| Przyczyna bieżącego restartu | kod | |
| Przyczyna poprzedniego restartu | kod | Z pamięci nieulotnej |
| **Licznik awarii od zawsze** | uint16 | panic / watchdog / brownout |
| Stan okresu próbnego aktualizacji | 0/1/2 | stabilna / próbna / potwierdzona |
| Odrzucona wersja po wycofaniu | int | 0 = brak |
| Liczba połączeń WiFi | uint32 | |
| Liczba przeskoków między punktami dostępu | uint32 | |
| Liczba połączeń i publikacji MQTT | uint32 | |

Nowa wersja firmware ma **3 minuty okresu próbnego**; jeśli w tym czasie nie udowodni, że
działa (WiFi + udane pobranie po TLS + pamięć powyżej 40 000 B), urządzenie **samo wraca do
poprzedniej wersji**. Wynika z tego przewidywalny, cykliczny stan: **restart co kilkanaście
minut w dniu wydania nowej wersji.**

### 10.5 Liczniki awarii łącza do falownika (przeżywają restart)

Sześć liczników rozbitych na **przyczynę × porę doby**:

| Licznik | Znaczenie |
|---|---|
| Brak połączenia — dzień / noc | Nie ma sesji TCP w ogóle |
| Milczące rejestry — dzień / noc | Sesja żyje, odczyty nie wracają |
| Brak rejestru mocy — dzień / noc | Sesja żyje, brakuje jednego konkretnego rejestru |

Rozbicie na dzień/noc jest obowiązkowe: nocne zasypianie falownika jest normalne i zalałoby
wspólny licznik szumem. Dopiero „47 w nocy, 0 w dzień" cokolwiek znaczy.
Do tego histogramy: **ile rejestrów padło w jednym cyklu** (0–5), osobno dla dwóch grup.

---

## 11. CZUJNIKI LOKALNE (ruch, światło, dotyk, temperatura układu)

### 11.1 Światło — fotorezystor

| Nazwa | Typ / jednostka | Zakres | Odświeżanie |
|---|---|---|---|
| Jasność otoczenia | uint16, **mV** | 0 … 3300 | **co 250 ms**, uśrednione z 8 próbek |
| Jasność surowa | uint16, ADC | 0 … 4095 | jw. |
| Poziom podświetlenia | 0 / 1 / 2 | — | pochodna jasności |

**Zmierzone realne stany w tej łazience** (16.07.2026 — to są prawdziwe liczby, nie z noty
katalogowej):

| Stan | Napięcie |
|---|---|
| Prawdziwa ciemność (23:30, zgaszone) | **17–26 mV** |
| Tylko światło pod prysznicem | **603–617 mV** |
| Zmierzch (19:30) | 251 mV |
| Półmrok (20:50) | 1050 mV |
| Światło zapalone | **2576–3164 mV** |

Zależność jest **logarytmiczna**, nie liniowa — skala liniowa 0–3300 mV zgniecie całą
ciemność w pierwszy procent. Progi przełączania poziomów mają **histerezę** (400/650 mV
i 1500/2200 mV).

**Nie ma wykrywania awarii tego czujnika.** Zostało usunięte, bo próg oparto na pomiarze
zrobionym o złej porze i **działający czujnik był uznawany za zepsuty**. Nie da się odróżnić
„odłączony" (~0 mV) od „ciemno" (~20 mV).

#### Statystyki światła (pamięć RTC — przeżywają aktualizację, giną przy zaniku zasilania)

| Dane | Rozdzielczość | Horyzont |
|---|---|---|
| **Histogram jasności** | **16 koszy**, zagęszczonych w spornej strefie 256–768 mV | od początku zbierania (docelowo **tydzień**) |
| **Sekundy na każdym z 3 poziomów** | 1 sekunda | jw. |
| **Zdarzenia „zostawione światło"** | **pierścień 8 zdarzeń** | jw. |

Kosze histogramu [mV]: <8, 8-16, 16-32, 32-64, 64-128, 128-256, 256-384, 384-512, 512-640,
640-768, 768-1024, 1024-1536, 1536-2048, 2048-2560, 2560-3072, ≥3072. Ponieważ próbkowanie
jest stałe (4 na sekundę), **zliczenia przeliczają się wprost na czas** — kosz o wartości
14 400 to godzina.

Zdarzenie „zostawione światło" powstaje, gdy jasność przekracza **400 mV nieprzerwanie przez
20 minut** przy zerowym ruchu. Każde zdarzenie niesie:

| Pole | Uwagi |
|---|---|
| Czas rozpoczęcia | **0 = zaczęło się przed synchronizacją zegara, godziny nie znamy** |
| Czas trwania | Aktualizowany **co sekundę w trakcie**, widoczny na żywo |
| Ile sekund wcześniej był ruch | **Odróżnia „wyszedł i zostawił" od „nikogo tu nie było"**. Wartość skrajna = od startu nie było ani jednego ruchu |

Jest też licznik **wszystkich** zdarzeń od początku — może przekraczać 8, i wtedy wiadomo,
że pierścień się zawinął.

### 11.2 Ruch — czujnik PIR

Moduł AM312: impuls ~2 s, okno martwe ~2 s. **Nie steruje niczym** — to wyłącznie pomiar.

| Nazwa | Typ / jednostka | Uwagi |
|---|---|---|
| Stan bieżący | bool | Podgląd odświeżany co 250 ms |
| Czas ostatniego ruchu | znacznik czasu | **Ginie przy restarcie** (celowo) |

#### Statystyki ruchu (pamięć RTC — przeżywają aktualizację)

| Dane | Rozdzielczość | Horyzont |
|---|---|---|
| **Rytm doby** | **24 kosze — po jednym na godzinę czasu lokalnego** | od początku zbierania (docelowo **tydzień**) |
| **Histogram szerokości impulsów** | **6 koszy**: <100 ms, 0,1–1 s, 1–3 s, 3–10 s, 10–60 s, ≥60 s | jw. |
| **Histogram przerw między impulsami** | **7 koszy**: <2 s, 2–5 s, 5–15 s, 15–60 s, 1–5 min, 5–30 min, ≥30 min | jw. |
| Liczba impulsów / zboczy / wyzwoleń | 1 | jw. |
| Najkrótszy / najdłuższy / ostatni impuls | milisekundy | jw. |
| Suma czasu w stanie „ruch" | milisekundy | jw. |

**Rytm doby to 24 sumy bez daty** — nie da się z nich odtworzyć konkretnej wizyty ani
konkretnej doby. To świadoma decyzja o prywatności: dane pochodzą z łazienki i pusty pas
godzin 8–16 mówiłby obcemu, kiedy w domu nikogo nie ma.

Liczba zboczy kontra liczba impulsów to **kontrola czystości sygnału**: przy czystym sygnale
zboczy jest dokładnie dwa razy tyle co impulsów. Nadmiar oznacza drgania i wtedy reszta liczb
kłamie.

### 11.3 Ciągłość pomiarów — trzy niezależne, porównywalne 1:1

Każdy z trzech zestawów statystyk (ruch, światło, awarie falownika) niesie własny komplet:

| Pole | Znaczenie |
|---|---|
| **Zbieram od** (epoch) | 0 = zegar jeszcze nie doszedł |
| **Sekundy realnego zbierania** | **To NIE to samo, co „teraz − początek"** |
| **Liczba restartów** | Ile razy przeżyły aktualizację / awarię |

Różnica między „teraz − początek" a „sekundy zbierania" mówi wprost, **ile pomiaru zjadły
restarty**. Przykład z realnego urządzenia: 3 restarty, 5 sekund straconego pomiaru.
Wszystkie trzy zestawy tykają z tego samego źródła, więc są porównywalne co do sekundy.

### 11.4 Dotyk

**Nie ma współrzędnych. Nie ma pozycji. Nie ma nacisku.** Cały model danych to trójwartościowy
stan: **brak / pojedyncze / podwójne**. Jedna elektroda pojemnościowa.

**Istotne dla projektu: pojedyncze dotknięcie jest zgłaszane dopiero po 550 ms** — tyle trwa
okno oczekiwania na ewentualne drugie dotknięcie. Reakcja na pojedynczy dotyk jest z natury
opóźniona o ponad pół sekundy; podwójny reaguje natychmiast przy drugim kontakcie.

Dostępne diagnostycznie: odczyt surowy i linia bazowa (dryfuje w stronę otoczenia tylko wtedy,
gdy nikt nie dotyka).

---

## 12. ZDARZENIA I PROGI ALARMOWE

Urządzenie samo wykrywa **7 rodzajów sytuacji wyjątkowych** z posiadanych danych. Każde
niesie tytuł (max 23 znaki), tekst (max 47 znaków) i opcjonalny kod pogody do ikony.
Sytuacje są sprawdzane w podanej kolejności i **wygrywa pierwsza pasująca** — nigdy nie ma
dwóch naraz.

| Rodzaj | Warunek | Tytuł | Przykładowy tekst |
|---|---|---|---|
| Awaria falownika | kod stanu = 0x0300 przy łączności | „Awaria falownika" | „Status 0x0300 - sprawdź instalację" |
| Burza | kod pogody ≥ 95 teraz **lub** w ciągu 12 h | „Burza" | „Prognozowana za 4 h" |
| Silny wiatr | wiatr ≥ **60 km/h** teraz lub w 12 h | „Silny wiatr" | „Do 72 km/h w ciągu 12 h" |
| Ulewa | opad ≥ **4 mm/h** w ciągu 12 h | „Ulewa" | „Do 6,3 mm/h - za 2 h" |
| Mróz | temperatura ≤ **−2 °C** w ciągu 12 h | „Mróz" | „Do -7°C w ciągu 12 h" |
| Upał | temperatura ≥ **+30 °C** w ciągu 12 h | „Upał" | „Do 32°C - pij wodę" |
| Falownik offline | *(zdefiniowany, patrz uwaga)* | — | — |

Zdarzenie jest pokazywane przez **6,5 sekundy**, a ten sam rodzaj nie może się powtórzyć
częściej niż **raz na 10 minut**.

**Warunki alarmowe są liczone wyłącznie z prognozy godzinowej (12 h) i stanu bieżącego** —
nigdy z prognozy 5-dniowej. Nie ma alarmu o śniegu, mimo że kody śniegu są obsługiwane.

---

## 13. DANE, KTÓRYCH NIE MAMY, A KTÓRE MOGŁYBY SIĘ WYDAWAĆ OCZYWISTE

Lista powstała po to, żeby nie zaprojektować widoku na dane, których nie ma i nie będzie bez
dopisania nowego kodu.

### Pogoda

| Czego nie ma | Uwagi |
|---|---|
| **Prawdopodobieństwa opadu „teraz"** | Istnieje tylko w prognozie godzinowej. Pole w strukturze jest, ale zawsze wynosi 0. |
| **Wschodu i zachodu słońca dla kolejnych dni** | Tylko dla dzisiaj. |
| **Wilgotności i ciśnienia w prognozie** | Ani godzinowej, ani dziennej — wyłącznie stan bieżący. |
| **Ciśnienia historycznego / trendu ciśnienia** | Nie ma żadnej historii ciśnienia, więc nie da się pokazać „rośnie / spada". |
| **Historii temperatury zewnętrznej** | Historia istnieje **wyłącznie** dla czujników w pomieszczeniach. |
| **Porywów wiatru** | Tylko prędkość średnia; maksymalny wiatr jest wyłącznie w prognozie dziennej. |
| **Widzialności, punktu rosy, zachmurzenia piętrami** | Nie pobierane. |
| **Prognozy UV godzina po godzinie** | Tylko bieżący i dobowe maksimum. |
| **Ostrzeżeń meteorologicznych IMGW** | Alarmy są liczone lokalnie z progów (rozdz. 12), nie pochodzą z żadnej instytucji. |
| **Jakiegokolwiek znacznika przeterminowania pogody** | Prognoza nigdy sama nie „wygasa". |

### Radar i opady

| Czego nie ma | Uwagi |
|---|---|
| **Prognozy opadu (nowcast)** | Darmowe API zwraca zero klatek prognozy. Wszystkie 13 klatek to przeszłość. |
| **Milimetrów na godzinę z radaru** | Poziomy 0–5 są odtworzone z palety kolorów, nie z jednostki fizycznej. |
| **Radaru poza obszarem ~300 × 161 km** | Poza prostokątem nie ma nic. |
| **Historii opadu dłuższej niż 2 godziny** | |

### Fotowoltaika

| Czego nie ma | Uwagi |
|---|---|
| **Produkcji z dni poprzednich** | Profil obejmuje **wyłącznie dobę bieżącą** i zeruje się o północy. Nie ma „wczoraj", „ten tydzień", „ten miesiąc". |
| **Rozbicia na stringi/MPPT** | Napięcie jest jedno. |
| **Stanu magazynu energii** | Instalacja go nie ma. |
| **Historii energii całkowitej** | Jest tylko bieżąca wartość licznika. |
| **Odróżnienia „0 W bo noc" od „0 W bo awaria"** — na podstawie samych liczb | Rozstrzyga wyłącznie flaga ważności. |
| **Wiarygodności napięcia / temperatury / sprawności** | Te trzy pola nie mają flagi obecności i przy nieudanym odczycie pokazują 0 jako pomiar. |

### Ogrzewanie

| Czego nie ma | Uwagi |
|---|---|
| **Chwilowego przepływu gazu** | Piec go nie udostępnia. Jest tylko licznik dobowy o rozdzielczości 0,1 m³. |
| **Temperatury w pomieszczeniach z pieca** | Piec nie ma termostatów pokojowych podpiętych do API. Temperatury pokojowe pochodzą **wyłącznie** z czujników BLE. |
| **Krzywej grzewczej, harmonogramów, trybu wakacyjnego** | Nie pobierane. |
| **Zużycia gazu za miesiąc/rok z API** | Agregaty API są zepsute i celowo ignorowane. Własny dziennik ma 120 dni. |
| **Historii temperatury CWU / zasilania** | Zapisywana jest tylko modulacja palnika. |
| **Możliwości odróżnienia „tryb pusty" od „tryb nie przyszedł"** | Tryby CWU i obiegu nie mają flagi obecności. |

### Czujniki w pomieszczeniach

| Czego nie ma | Uwagi |
|---|---|
| **Historii wilgotności** | Świadomie usunięta. Tylko temperatura. |
| **Historii dłuższej niż 24 h** | |
| **Więcej niż 6 pokoi w historii** | Choć czujników może być 8. |
| **Napięcia baterii w procentach dla każdego formatu ramki** | Bateria bywa nieznana (0). |
| **Jakiejkolwiek nazwy dla czujnika bez wpisu w ustawieniach** | Zostaje adres MAC. |

### Samoloty

| Czego nie ma | Uwagi |
|---|---|
| **Wieku danych** | Jedyna grupa bez jakiegokolwiek znacznika świeżości. |
| **Prędkości pionowej, wznoszenia/opadania** | Nie parsowane. |
| **Rozkładów lotów, opóźnień, numerów rejsu** | Tylko znak wywoławczy i trasa IATA. |
| **Linii lotniczej** | Da się zgadnąć ze znaku wywoławczego, ale urządzenie tego nie robi. |
| **Historii / śladu trasy** | Tylko pozycja bieżąca. |
| **Odróżnienia kursu 0° od braku kursu** | |

### Ogólne

| Czego nie ma | Uwagi |
|---|---|
| **Temperatury otoczenia urządzenia** | Czujnik mierzy krzem układu, nie powietrze. Najbliższa prawdy jest temperatura z czujnika BLE w tym samym pomieszczeniu albo z GA17. |
| **Dotyku z pozycją** | Trzy stany, zero współrzędnych. |
| **Historii czegokolwiek dłuższej niż 120 dni** | Najdłuższy szereg to gaz. |
| **Kalendarza, powiadomień, poczty, komunikatów** | Urządzenie nie ma takich źródeł. |
| **Cen energii, taryf, kosztów** | Nigdzie nie występują. |
| **Danych o wodzie, ściekach, innych mediach** | |

---

## 14. STANY DEGRADACJI — co realnie się psuje i jak często

Rozdział istnieje po to, żeby zaprojektować **sensowne stany zastępcze zamiast pustych pól**.
Uporządkowane od najczęstszych.

### 14.1 Zdarzenia codzienne, przewidywalne — nie są awarią

| Stan | Jak często | Objaw w danych | Czego wymaga projekt |
|---|---|---|---|
| **Falownik śpi w nocy** | **Codziennie**, od 30 min po zachodzie do 30 min po wschodzie. Zimą **16 godzin na dobę.** | Osobna flaga „śpi", inna niż „offline". Wszystkie liczby PV wyzerowane. | Stan **neutralny**, nie alarmowy. Zimą to jest stan dominujący, nie wyjątek. |
| **Brak opadu** | Większość czasu w roku | Flaga „czy gdziekolwiek pada" = false | Jest to normalny stan pogody, nie brak danych. |
| **Nieznana trasa samolotu** | Bardzo często | Puste pola trasy | Wygląd musi działać z samym znakiem wywoławczym. |
| **Czujnik BLE bez nazwy** | Zawsze dla nowego czujnika i dla obcych nadajników w bloku | Etykietą jest adres MAC | Miejsce na 17-znakowy MAC tam, gdzie normalnie stoi nazwa pokoju. |
| **Nasłuch BLE blokuje odpytywanie** | **Co 20 sekund, na 4 sekundy** | Wszystkie inne terminy przesuwają się do 4 s | Nie zakładać sekundowej regularności odświeżania czegokolwiek. |
| **Brak zegara po starcie** | Przy każdym starcie, do ~10 s | Historie się nie zapisują, wiek radaru nieznany, godzina zdarzeń nieznana | Pierwsze sekundy po starcie to osobny stan. |

### 14.2 Awarie o znanej częstotliwości

| Stan | Jak często | Objaw | Zachowanie danych |
|---|---|---|---|
| **Nieudany odczyt pogody** | Ponowienie po 30 s zamiast 15 min | Komunikat błędu | **Stare dane zostają**, brak znacznika starości |
| **Nieudany odczyt PV** | Cykl 30 s; przy problemie kanał potrafi paść na **~45 minut** (odnotowane 19/20.07.2026) | Trzy różne komunikaty | **Wartości WYZEROWANE** — jedyne takie źródło |
| **Nieudany odczyt pieca** | Ponowienie po 2 min | Komunikat | **Stare dane zostają** |
| **Nieudany radar** | Ponowienie po 60 s / 2 min | Komunikat | Stare klatki zostają; niekompletne pobranie odrzucane w całości |
| **Nieudany ADS-B** | Ponowienie po 20 s | Komunikat | **Stara lista zostaje, bez śladu wieku** |
| **Serwis tras zablokowany** | **10 minut** po każdym błędzie innym niż 404 | Wszystkie nowe trasy nieznane | Stan trwa realnie długo |
| **Bramka BLE nie odpowiada** | Narastające opóźnienia: 20 s → 60 s → 120 s | Czujniki tej bramki tracą źródło sygnału natychmiast | Do 3 bramek, każda psuje się osobno |
| **Przeskok na inny punkt WiFi** | Przegląd co 3 min, przeskok gdy sygnał < −68 dBm i jest o 8 dB lepszy | **1–3 sekundy bez sieci** | Krótkie, ale realne przerwy w łączności |

### 14.3 Awarie rzadkie, ale długie

| Stan | Charakterystyka |
|---|---|
| **Wygaśnięcie autoryzacji pieca** | Co ~180 dni. **Nie naprawia się samo** — wymaga kliknięcia w przeglądarce. Jest licznik dni do wygaśnięcia, więc da się ostrzec z wyprzedzeniem. Do tego czasu cała grupa „ogrzewanie" znika. |
| **Stacja GA17 milczy > 3 h** | Przełączenie na GA24 i **zniknięcie temperatury, wilgotności i ciśnienia** z tej grupy. Dane pyłowe zostają, ale z innego punktu miasta. |
| **RainViewer po awarii oferuje < 13 klatek** | Trwa kilkanaście minut. Całe pobranie odrzucane, stare klatki zostają. |
| **Brak PSRAM przy starcie** | Radar mapowy nie działa **w ogóle przez całą sesję** — wszystko albo nic. |
| **Mało wolnej pamięci (< 64 kB)** | Radar punktowy odpuszcza cykle. Na realnym urządzeniu licznik pominięć = 0. |
| **Wycofanie aktualizacji** | Nowa wersja ma 3 minuty na udowodnienie, że działa. W dniu wydania oznacza to serię restartów co kilkanaście minut. |
| **Awaria z restartem** | Jest licznik „od zawsze" (panic / watchdog / brownout) oraz przyczyna poprzedniego restartu. Statystyki ruchu i światła **przeżywają** restart; profile PV, gazu, palnika i pokoi też (pamięć nieulotna). |
| **Zanik zasilania** | **Kasuje statystyki ruchu, światła i awarii falownika** (pamięć RTC). Profile w pamięci nieulotnej przeżywają. |

### 14.4 Stany „nie skonfigurowane" — całe grupy danych mogą nie istnieć nigdy

To nie są awarie, tylko normalne warianty instalacji. Projekt musi wyglądać sensownie
**bez każdej z tych grup z osobna**:

| Grupa | Warunek istnienia |
|---|---|
| **Fotowoltaika** | Użytkownik wpisał adres IP falownika |
| **Ogrzewanie i gaz** | Użytkownik autoryzował się w chmurze Viessmanna |
| **Czujniki w pomieszczeniach** | Jest co najmniej jeden czujnik BLE w zasięgu |
| **Bramki BLE** | Skonfigurowana co najmniej jedna z trzech |
| **MQTT / Home Assistant** | Domyślnie **wyłączone** |
| **Ręczne odczyty licznika gazu** | Użytkownik wpisał choć jeden z ośmiu |
| **Temperatura/wilgotność/ciśnienie z sieci miejskiej** | Stacja główna odpowiada |

Minimalna sensowna instalacja to **samo WiFi**: wtedy istnieje pogoda, radar, jakość
powietrza, samoloty, czujniki lokalne i diagnostyka — a fotowoltaiki, ogrzewania i czujników
w pomieszczeniach nie ma wcale.

### 14.5 Pułapki liczbowe do świadomego zaprojektowania

| Wartość | Znaczy też | Skutek |
|---|---|---|
| Siła sygnału czujnika **= 0** | „Brak świeżego źródła", nie „0 dBm" | Nie rysować jako pełnego sygnału |
| Bateria czujnika **= 0%** | „Nieznana" | Nie rysować jako pustej baterii |
| Wiek odczytu **= 9999 s** | „Nigdy się nie odezwał" | Nie rysować jako „2,7 godziny temu" |
| Wiek radaru **= 0 s** | „Zegar nieznany", nie „świeże" | |
| Kurs samolotu **= 0°** | „Brak danych o kursie" | |
| Czas rozpoczęcia zdarzenia **= 0** | „Zaczęło się przed synchronizacją zegara" | Nie pokazywać północy |
| Napięcie PV / temperatura falownika / sprawność **= 0** | Może znaczyć „nie odczytano" | Brak flagi rozstrzygającej |
| Temperatura z pieca **= 0,0 °C** | Zawsze sprawdzać flagę obecności | Historycznie mylone z zamarzniętym zasobnikiem |
| Tryb CWU / obiegu **= pusty tekst** | „Cecha nie przyszła" | Pokazać myślnik, nie pusty tryb |
| Liczba klatek radaru **= 13** | Część może być pusta | Animacja musi znieść puste kadry |
| Prawdopodobieństwo opadu „teraz" **= 0%** | Zawsze 0, nigdy nie wypełniane | Nie pokazywać w ogóle |

---

## Podsumowanie liczbowe

| Grupa | Liczba pól |
|---|---|
| Pogoda bieżąca (+ księżyc, + kody pogody) | 24 |
| Prognoza godzinowa (× 12 punktów) | 9 |
| Prognoza dzienna (× 5 punktów) | 10 |
| Opady i radar (punktowy + mapowy) | 14 |
| Jakość powietrza | 16 |
| Fotowoltaika (+ profil doby) | 20 |
| Ogrzewanie i gaz (+ historie) | 40 |
| Czujniki w pomieszczeniach (+ historia) | 24 |
| Samoloty | 15 |
| Stan urządzenia i diagnostyka | ~70 |
| Czujniki lokalne (ruch, światło, dotyk) | 45 |
| Zdarzenia alarmowe | 7 rodzajów |
| **Razem** | **~290 pól** |

Najważniejsza rzecz do zapamiętania z całego dokumentu: **prawie każde pole ma trzeci stan
poza „jest wartość" i „nie ma wartości" — ma jeszcze „wartość jest, ale nieświeża".**
Projekt, który przewidzi tylko dwa stany, będzie kłamał dokładnie w tych momentach, w których
najbardziej opłaca się mówić prawdę.
