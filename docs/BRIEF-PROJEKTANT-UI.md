# Brief dla projektanta interfejsu — wyświetlacz informacyjny ESP32-S3

**Wersja dokumentu:** 1.1 · lipiec 2026 · firmware v117
**Repozytorium:** https://github.com/premiumads-pl/Weather-huawei-sun2000

> **Uwaga o wiarygodności tego dokumentu.** Wersja 1.0 zawierała dwa błędy rzeczowe: twierdziła, że urządzenie nie ma dotyku (ma) i że ikony pogody są rysowane wektorowo (są bitmapami zajmującymi ~98 kB flasha). Oba wynikały z pisania z pamięci zamiast sprawdzenia w kodzie. W wersji 1.1 **każda liczba i każde twierdzenie techniczne zostały zweryfikowane** bezpośrednio w źródłach albo odczytane z działającego urządzenia. Miejsca, gdzie czegoś nie zweryfikowano, są oznaczone wprost.

---

## 1. Czym jest to urządzenie

Wyświetlacz informacyjny **wiszący na ścianie w łazience**. Patrzy się na niego **z odległości 1,5–3 m, przelotnie, często kątem oka**, zwykle robiąc coś innego.

Urządzenie **samo przełącza ekrany w rotacji**. Czasy są zróżnicowane i zweryfikowane w kodzie:

| Ekran | Czas trzymania |
|---|---|
| większość ekranów | 9 s |
| PAMIĘĆ, RUCH | 14 s |
| SAMOLOTY | 15 s |
| RADAR | 20 s (tyle, żeby animacja przeszła dwa razy) |

### Interakcja — jest, choć minimalna

Urządzenie **ma dotyk pojemnościowy** (GPIO7, kanał TOUCH7). Nie ma fizycznego przycisku — dotyka się pinu albo doprowadzonego z niego kawałka folii czy śruby; wystarczy pojemność ciała.

Dostępne są **dwa gesty, działające globalnie na każdym ekranie**:

| Gest | Działanie |
|---|---|
| **jedno dotknięcie** | odliczanie bieżącego ekranu **od nowa** — czyli „zatrzymaj się, chcę popatrzeć" |
| **podwójne dotknięcie** | **poprzedni ekran** — czyli „cofnij, przegapiłem" |

Uwaga techniczna: pojedyncze dotknięcie jest zgłaszane z opóźnieniem (inaczej nie dałoby się go odróżnić od pierwszej połowy podwójnego). Próg czułości nie jest stały — liczy się od dryfującej linii bazowej, bo zależy od długości przewodu i wilgotności.

**Co z tego wynika dla projektu — i to jest ważna wskazówka:**

Te dwa gesty mówią, czego użytkownik naprawdę potrzebuje. „Zatrzymaj" znaczy, że ekran zawiera coś, co warto oglądać **dłużej niż 9 sekund**. „Cofnij" znaczy, że coś **umknęło w locie**. Dobry ekran powinien więc mieć dwie warstwy czytania:

- **warstwa pierwsza** — jedna rzecz czytelna z 3 m w ułamku sekundy (zwykle jedna duża liczba),
- **warstwa druga** — szczegóły, po które sięga się wzrokiem dopiero po zatrzymaniu ekranu.

Obecnie **nie ma żadnej wizualnej informacji zwrotnej na dotknięcie**. Użytkownik dotyka i nie widzi potwierdzenia — jedynym sygnałem jest to, że pasek postępu rusza od nowa. To jest realna luka w projekcie i dobry kandydat na poprawkę w V2.

### Podświetlenie

Zmienia się automatycznie na podstawie czujnika światła, trzy poziomy (skala 0–255):

| Warunki | Jasność |
|---|---|
| ciemno | **45** |
| półmrok | **130** |
| światło zapalone | **255** |

Projekt musi być czytelny także przy **45** — kontrast nie może opierać się na subtelnych różnicach jasności, bo przy przygaszeniu ciemne kolory zlewają się ze sobą.

---

## 2. Sprzęt — twarde ograniczenia

Liczby odczytane z działającego urządzenia (firmware v117) i z logu ostatniego budowania:

| Zasób | Stan | Uwaga |
|---|---|---|
| **SRAM statyczny** | **73 480 B** zajęte, limit **76 000 B** → **zapas 2 520 B** | Limit **twardo egzekwowany** — wersja go przekraczająca **nie zostanie wydana** |
| **Sterta (SRAM dynamiczny)** | ~87 kB wolne, największy spójny blok ~40 kB | Dołek historyczny schodził do ~8 kB |
| **PSRAM** | 2 MB, wolne **1,11 MB** | Bufor ekranu + 13 klatek radaru |
| **Flash (partycja aplikacji)** | 1 699 968 B z 1 966 080 B (**86%**) → wolne **260 kB** | Dwie partycje OTA po 1,97 MB |
| **RTC (przeżywa restart)** | 408 B z 7 680 B | Statystyki długoterminowe |

**Procesor:** dwurdzeniowy Xtensa LX7, 240 MHz. Rdzeń 1 rysuje, rdzeń 0 obsługuje sieć i panel WWW.

Ograniczeniem **nie jest** liczba kolorów ani rozdzielczość. Ograniczeniem są: **statyczny RAM** (2,5 kB zapasu), **flash** (260 kB) i **czas rysowania klatki**.

---

## 3. Wyświetlacz i geometria

- **Panel:** ST7789, 240×320, używany **poziomo → 320×240 px**
- **Kolor:** RGB565 — 32 odcienie czerwieni, 64 zieleni, 32 błękitu. Delikatne gradienty potrafią się „poprążkować"
- **Magistrala:** SPI 80 MHz

### Podział ekranu (piksele, y od góry) — zweryfikowany w kodzie

```
y =   0 .. 27    NAGŁÓWEK        (28 px) — nazwa ekranu, data, godzina, kropka stanu
y =  29 .. 31    PASEK POSTĘPU   (3 px)  — segmenty: który ekran, które pomijane
y =  32 .. 205   TREŚĆ WIDOKU            — pełna swoboda projektanta
y = 206 .. 239   STOPKA PV       (34 px) — produkcja, energia dzisiaj, bilans sieci
```

**Ograniczenie architektury:** obszar `0..205` jest rysowany do bufora w PSRAM i wypychany jednym transferem. Stopka `206..239` rysowana jest **osobno, bezpośrednio na wyświetlacz**, z własnym odświeżaniem tylko przy zmianie wartości.

Element graficzny **nie może przechodzić przez granicę y=205** — zostanie rozcięty na dwa niezależne przebiegi rysowania i rozjedzie się. To nie jest zalecenie stylistyczne.

---

## 4. Budżet czasu — ile wolno rysować

Zmierzone na urządzeniu (v117):

```
rysowanie treści do bufora   18,9 ms
wypchnięcie bufora na SPI    19,8 ms   (132 kB przez magistralę)
                             -------
klatka                       38,7 ms   →  20 kl./s  (sufit ~26)
```

Wypchnięcie to stała opłata, nieredukowalna bez zmniejszenia obszaru rysowania. Na całą treść ekranu zostaje **~19 ms**.

| Operacja | Koszt |
|---|---|
| Wypełniony prostokąt, linia pozioma/pionowa | **bardzo tanie** — podstawowe narzędzie |
| Tekst | tani — kilkadziesiąt znaków na ekran bez problemu |
| Rysowanie „ciągami" (odcinek jednego koloru) | tanie — tak rysujemy mapy i opad |
| Gradient pasami poziomymi | tanie i wygląda dobrze |
| **Pojedyncze piksele po całym ekranie** | **zabójcze** — 320×206 = 66 tys. pikseli |
| Przezroczystość, cienie rozmyte, wygładzanie krawędzi | **niedostępne** — biblioteka tego nie ma |

---

## 5. Typografia — najważniejsza sekcja

Fonty są **bitmapowe, wkompilowane w firmware**. Zawartość każdego zestawu znaków sprawdzona programowo w tablicach kodów:

| Font | Wysokość | Polskie znaki | ° | µ | ³ | Flash | Zastosowanie |
|---|---|---|---|---|---|---|---|
| **PlFont10** | 10 px | ✅ komplet | ✅ | ❌ | ✅ | ~12,8 kB | podpisy, etykiety |
| **PlFont14** | 13 px | ✅ komplet | ✅ | ❌ | ❌ | ~14,2 kB | wartości, nagłówki sekcji |
| **PlFont18** | 18 px | ✅ komplet | ✅ | ❌ | ❌ | ~21,1 kB | duże wartości |
| **FreeSansBold 24pt** | ~34 px | ❌ **brak** | ❌ | ❌ | ❌ | z biblioteki | tylko wielkie liczby |
| **RetroFont 8×8** | 8 px × skala 1–6 | ❌ **brak** | ✅ (jako `*`) | ❌ | ❌ | ~3,8 kB | ekran retro |

Polskie znaki obecne w PlFont10/14/18: **ą ć ę ł ń ó ś ź ż** i wielkie odpowiedniki.

### Pułapki, które już kosztowały błędy na ekranie

1. **Znak mikro `µ` nie istnieje w żadnym foncie.** Dlatego pyły opisujemy `ug/m3`, a nie `µg/m³`. Brakujący glif nie wywala programu — jest **po cichu pomijany**, więc „µg/m³" wyświetli się jako „g/m". Wygląda to jak literówka, nie jak błąd techniczny.
2. **Wielkie liczby (FreeSansBold) nie znają polskich znaków ani stopnia.** Temperatura idzie tym fontem, ale „°C" obok — już innym.
3. **Dwa układy współrzędnych.** Funkcje `gl…` kotwiczą tekst **górną krawędzią**, `pl…` — **linią bazową**. Pomylenie przesuwa tekst o wysokość fontu. **Projektant powinien podawać prostokąty, w których tekst ma się zmieścić**, nie punkty.
4. **Nie istnieją:** wielokropek `…`, myślnik `—`, punktor `•`, cudzysłowy typograficzne. Używać `...`, `-`, `:`.

Nowy font bitmapowy można dorobić (tak powstał RetroFont — koszt 3,8 kB). Trzeba to zgłosić na etapie projektu.

---

## 6. Kolory

**52 zdefiniowane kolory**, pogrupowane semantycznie: tła i karty, teksty (3 stopnie), pogoda (słońce, chmura, deszcz, śnieg, burza, wiatr, wilgotność, ciśnienie), temperatura (5 progów), fotowoltaika, stany (OK / uwaga / awaria), mapa, loty, jakość powietrza (6 klas).

Tło jest ciemne (prawie czarny granat) — urządzenie wisi w łazience także w nocy.

Projektant może zaproponować **własną paletę dla V2**; ograniczeniem jest tylko RGB565 i czytelność przy podświetleniu 45.

---

## 7. Grafika, którą już mamy — i ile realnie kosztuje

| Element | Format | Koszt flasha |
|---|---|---|
| **Ikony pogody** (8 sztuk) | **bitmapy 64×64, RGB565 + kanał alfa** | **~98 kB** (12 288 B na ikonę) |
| Mapa wąska (111 km) | odcinki lądu, generowana skryptem | ~6,4 kB |
| Mapa szeroka (300 km) | jw. | ~6,5 kB |
| Font retro 8×8 | bitmapa | ~3,8 kB |
| Sprite Mario 16×16, 4 klatki | 2 piksele na bajt | ~3,3 kB |
| Fazy księżyca | rysowane proceduralnie | ~3 kB kodu |

**To jest kluczowe dla budżetu V2.** Ikony pogody są bitmapami i zajmują ~98 kB z 260 kB wolnego flasha. Komplet nowych ikon w innym stylu, w tym samym formacie, kosztowałby **drugie tyle** — i przy dwóch wyglądach naraz może się nie zmieścić.

Wyjścia: mniejsze ikony, mniej kolorów (paleta indeksowana zamiast pełnego RGB565), albo ikony rysowane proceduralnie z prymitywów — to ostatnie jest praktycznie darmowe we flashu, ale kosztuje czas rysowania i wygląda inaczej.

Mapy i sprite'y są generowane skryptami — można wygenerować dowolny inny wycinek, skalę czy styl.

---

## 8. Dane — katalog tego, co urządzenie ma do pokazania

Wszystkie pozycje odczytane ze struktur danych w kodzie.

### 8.1 Pogoda (Open-Meteo, co 15 min)
**Teraz:** temperatura, odczuwalna, wilgotność, zachmurzenie, wiatr (prędkość + kierunek), ciśnienie, opad, prawdopodobieństwo opadu, indeks UV (surowy i skorygowany o chmury), dzień/noc, kod pogody.
**Godzinowa:** 24 h. **Dzienna:** 5 dni — min/max, opad, UV, wschód i zachód słońca.

### 8.2 Fotowoltaika (falownik Huawei, Modbus TCP, co 30 s)
Moc DC, moc AC, bilans sieci (+oddawanie / −pobór), **wyliczony pobór domu**, energia dzisiaj, energia całkowita, napięcie, temperatura falownika, sprawność, status.
**Historia dnia:** profil produkcji i poboru co 5 min.

### 8.3 Ogrzewanie (Viessmann, chmura ViCare)
Temperatura CWU i zasilania, stan palnika, modulacja, godziny pracy palnika, liczba startów, zużycie gazu dzisiaj, historia palnika. Osobno: ręczne odczyty licznika gazu z porównaniem do danych z pieca.

### 8.4 Czujniki w domu (Bluetooth, do 8 sztuk)
Temperatura, wilgotność, bateria, siła sygnału, **źródło odbioru** (bezpośrednio czy przez bramkę). Historia 24 h z 6 pomieszczeń.

### 8.5 Radar opadów (RainViewer)
13 klatek co 10 min (2 h wstecz), mapa 300 km, 6 poziomów intensywności, animacja z interpolacją ruchu.

### 8.6 Samoloty (co 15 s)
Znak wywoławczy, typ, trasa, pozycja, wysokość, prędkość, kurs; rozróżnienie ląduje / startuje / przelot.

### 8.7 Jakość powietrza (miejska sieć Gdyni, co 15 min)
PM10, PM2.5, indeks 6-klasowy (liczony lokalnie), nazwa stacji, **wiek pomiaru**, temperatura/wilgotność/ciśnienie ze stacji.

### 8.8 Czujniki własne
**Światło:** histogram dobowy, czas na każdym poziomie podświetlenia, zdarzenia „zostawione światło". **Ruch:** rytm doby (24 słupki), rozkłady długości impulsów i przerw. Zbierane tygodniami, przeżywają restarty.

### 8.9 Stan urządzenia
Czas pracy, restarty i przyczyny, temperatura procesora, pamięć (SRAM z fragmentacją, PSRAM, flash z partycjami, RTC, ROM, stosy), jakość Wi-Fi, statystyki MQTT, liczniki awarii źródeł danych, wydajność rysowania.

---

## 9. Ekrany istniejące — **wygląd V1** (zostaje bez zmian)

13 ekranów w rotacji:

`RETRO` · `TERAZ` · `GODZINY` · `RADAR` · `5 DNI` · `W DOMU` · `PIEC` · `FOTOWOLTAIKA` · `SAMOLOTY` · `POWIETRZE` · `PAMIĘĆ` · `RUCH` · `STATYSTYKI`

Cztery są **pomijane automatycznie**, gdy nie mają czego pokazać: RADAR (brak opadu), W DOMU (brak czujników), PIEC (brak autoryzacji), POWIETRZE (brak danych ze stacji). Pominięte ekrany są zaznaczone przygaszonym segmentem na pasku postępu.

**Wygląd V1 nie podlega przeprojektowaniu.**

---

## 10. Zadanie — **wygląd V2**

Zaprojektować **alternatywny, kompletny wygląd** tych samych danych, w stylistyce **retro** (konsole i komputery 8/16-bitowe, przełom lat 80. i 90.).

Punkt odniesienia: grafika z gry platformowej w stylu Mario — ceglane platformy, pikselowe chmury, duże słońce, ograniczona paleta, twarde krawędzie, wyraźny podział na pasy interfejsu i scenę pomiędzy nimi. Jeden taki ekran (RETRO) już istnieje w firmware jako próba stylu i można go obejrzeć na żywo.

### Oczekiwany rezultat

1. **Pytania do właściciela** — zadać przed rysowaniem. To jest wprost oczekiwane.
2. **Jeden lub trzy mockupy** nowego wyglądu, **dokładnie 320×240 px**, bez wygładzania krawędzi.
3. Zaczynamy od **jednego ekranu** jako próbki stylu.

### Wymagania techniczne

- 320×240, RGB565
- Granica y=205 (treść) / y=206+ (stopka)
- Fonty istniejące albo nowy bitmapowy (do zgłoszenia)
- Bez przezroczystości, cieni rozmytych i wygładzania
- Czytelność z 3 m przy podświetleniu 45
- ~19 ms na narysowanie treści
- Warto zaprojektować **reakcję na dotknięcie** — dziś jej nie ma

---

## 11. Przełączanie V1 / V2

W panelu WWW mają pojawić się **przyciski „Wygląd V1" i „Wygląd V2"**. Wybór zapamiętywany w pamięci nieulotnej — ma przeżywać restart i aktualizację.

**Konsekwencja:** dwa wyglądy to dwa komplety kodu rysującego w jednym firmware. Przy 260 kB wolnego flasha jest to wykonalne, ale im bardziej V2 opiera się na istniejących mechanizmach (te same fonty, te same prymitywy), tym taniej. Największe ryzyko to **nowy komplet ikon pogody** — patrz punkt 7.

---

## 12. Czego nie robić — lekcje z tego projektu

Wszystkie punkty pochodzą z realnych błędów, które trafiły na ekran:

1. **Nie zakładaj, że font ma znak.** Brakujący glif znika po cichu.
2. **Nie rysuj elementów przez granicę y=205.**
3. **Nie używaj losowości bez stałego ziarna.** Element losowany co klatkę **miga** 20 razy na sekundę.
4. **Nie odczytuj czasu wewnątrz rysowania.** Zrzut ekranu dla panelu renderuje obraz w paskach — osobne odczyty zegara rozjeżdżają obraz.
5. **Nie pokazuj zera zamiast braku danych.** Nieruchome „0" czyta się jak prawdziwy pomiar.
6. **Nie ukrywaj źródła.** Gdy dane mogą pochodzić z dwóch miejsc (stacja główna/zapasowa, czujnik wprost/przez bramkę), musi być widać, z którego.
7. **Kolor ma opisywać tę liczbę, którą pokazuje.** Mieliśmy kartę kolorowaną według innej wartości niż wyświetlana.

---

## 13. Jak dostarczyć projekt

Mockupy jako PNG **1:1 (320×240)**, bez skalowania i wygładzania; mile widziana wersja powiększona ×2/×3 metodą „najbliższy sąsiad".

Do każdego ekranu: krótki opis, **które dane** są pokazane i **skąd** (odwołanie do punktu 8), oraz co się dzieje, **gdy danych brakuje**.

---

## 14. Materiały

- Kod źródłowy: repozytorium podane na górze (publiczne)
- Bieżący obraz ekranu: urządzenie udostępnia go przez HTTP
- Istniejący ekran RETRO jako punkt wyjścia stylistyczny
- Stan opisany w tym dokumencie: firmware v117
