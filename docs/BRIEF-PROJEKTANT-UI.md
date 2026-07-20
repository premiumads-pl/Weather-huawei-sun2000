# Brief dla projektanta interfejsu — wyświetlacz informacyjny ESP32-S3

**Wersja dokumentu:** 1.0 · lipiec 2026 · firmware v117
**Repozytorium:** https://github.com/premiumads-pl/Weather-huawei-sun2000

---

## 1. Czym jest to urządzenie

Mały wyświetlacz informacyjny **wiszący na ścianie w łazience**. Nie ma klawiatury, myszy ani dotyku. Nikt przy nim nie siedzi — patrzy się na niego **z odległości 1,5–3 m, przelotnie, często kątem oka**, zwykle w trakcie robienia czegoś innego.

Urządzenie **samo przełącza ekrany w rotacji** (co ~9–20 s, zależnie od ekranu). Użytkownik nie steruje nim w codziennym użyciu — jedyne sterowanie to panel WWW w przeglądarce, używany sporadycznie.

To zmienia wszystko w projektowaniu:

- **Nie ma interakcji.** Żadnych przycisków, zakładek, hoverów, scrolla. Każdy ekran musi być zrozumiały bez kliknięcia.
- **Nie ma drugiej szansy.** Ekran znika po kilkunastu sekundach. Jeśli informacja wymaga dwóch spojrzeń, jest źle zaprojektowana.
- **Czytelność bije estetykę.** Liczba, której nie da się odczytać z 3 m, jest bezużyteczna niezależnie od tego, jak ładnie wygląda na monitorze projektanta.
- **Ekran bywa przygaszony.** Podświetlenie zmienia się automatycznie (3 poziomy: 45 / 130 / 255 na 255) w zależności od światła w pomieszczeniu. Projekt musi działać także przy 45 — czyli kontrast nie może opierać się na subtelnych różnicach jasności.

---

## 2. Sprzęt — twarde ograniczenia

| Zasób | Ile jest | Ile zostało | Uwaga |
|---|---|---|---|
| **SRAM statyczny** | 512 kB (dostępne ~327 kB) | **2 520 B** do limitu | Limit 76 000 B jest **twardo egzekwowany** przez skrypt wydawniczy — wersja przekraczająca go **nie zostanie wydana** |
| **PSRAM** | 2 MB | ~1,11 MB | Tu mieszka bufor ekranu i 13 klatek radaru |
| **Flash (partycja aplikacji)** | 1,97 MB | ~260 kB | Dwie partycje OTA po 1,97 MB |
| **RTC (przeżywa restart)** | 7 680 B | ~7,2 kB | Używane na statystyki długoterminowe |

**Procesor:** dwurdzeniowy Xtensa LX7, 240 MHz. Rdzeń 1 rysuje, rdzeń 0 obsługuje sieć i panel WWW.

### Co z tego wynika dla projektu

Ograniczeniem **nie jest** liczba kolorów ani rozdzielczość — te są komfortowe. Ograniczeniem jest:

1. **Statyczny RAM** — każda nowa tablica, bufor czy stała globalna zjada z 2,5 kB zapasu. Grafiki rastrowe (bitmapy) trafiają do flasha, ale duże bufory robocze do RAM-u.
2. **Czas rysowania klatki** — patrz punkt 4.

---

## 3. Wyświetlacz i geometria

- **Panel:** ST7789, 240×320, używany **poziomo → 320×240 px**
- **Kolor:** RGB565 (16-bitowy: 5 bitów czerwony, 6 zielony, 5 niebieski). Czyli **65 536 kolorów**, ale odcieni czerwieni i błękitu jest po 32, a zieleni 64 — delikatne gradienty potrafią się „poprążkować"
- **Magistrala:** SPI 80 MHz

### Podział ekranu (piksele, y liczone od góry)

```
y =   0 .. 27    NAGŁÓWEK        (28 px) — nazwa ekranu, data, godzina, kropka stanu
y =  29 .. 31    PASEK POSTĘPU   (3 px)  — segmenty: który ekran z 13, które pomijane
y =  32 .. 205   TREŚĆ WIDOKU            — tu projektant ma pełną swobodę
y = 206 .. 239   STOPKA PV       (34 px) — produkcja, dzisiejsza energia, bilans sieci
```

**Ważne technicznie:** obszar `0..205` jest rysowany do bufora w PSRAM i wypychany jednym transferem. Stopka `206..239` jest rysowana **osobno, bezpośrednio na wyświetlacz**, poza tym buforem, i ma własny mechanizm odświeżania tylko przy zmianie wartości.

**Konsekwencja dla projektanta:** element graficzny **nie może przechodzić przez granicę y=205**. Napis czy ramka rozpięta na 200–215 zostanie rozcięta na dwa niezależne przebiegi rysowania i rozjedzie się. To nie jest zalecenie stylistyczne, tylko ograniczenie architektury.

---

## 4. Budżet czasu — ile wolno rysować

Aktualnie: **~20 klatek na sekundę**, sufit ~26.

```
rysowanie treści do bufora   ~18 ms
wypchnięcie bufora na SPI    ~20 ms   (132 kB przez magistralę)
                             ------
razem                        ~42 ms na klatkę
```

Wypchnięcie to stała opłata, której nie da się zmniejszyć bez zmniejszenia obszaru rysowania. Zostaje **~18–21 ms na całą treść ekranu**.

### Co jest tanie, a co drogie

| Operacja | Koszt | Uwaga |
|---|---|---|
| Wypełniony prostokąt, linia pozioma/pionowa | **bardzo tanie** | Podstawowe narzędzie — używać do wszystkiego |
| Tekst | tani | Kilkadziesiąt znaków na ekran bez problemu |
| Rysowanie „ciągami" (linia pozioma jednego koloru) | tanie | Tak rysujemy mapy i opad |
| **Pojedyncze piksele po całym ekranie** | **zabójcze** | 320×206 = 66 tys. pikseli — samo to przekroczy budżet |
| Przezroczystość, cienie rozmyte, wygładzanie krawędzi | **niedostępne** | Biblioteka tego nie ma; wszystko jest twardo krawędziowe |
| Gradient | drogi, jeśli per piksel | Robimy je pasami poziomymi — to jest tanie i wygląda dobrze |

**Zasada:** jeśli efekt wymaga policzenia czegoś dla każdego piksela osobno, prawdopodobnie się nie zmieści.

---

## 5. Typografia — najważniejsza sekcja tego briefu

Nie ma tu „wybierz font z Google Fonts". Fonty są **bitmapowe, wkompilowane w firmware**, każdy zajmuje flash. Dostępne są cztery.

| Font | Wysokość liter | Polskie znaki | ° | µ | ³ | Zastosowanie |
|---|---|---|---|---|---|---|
| **PlFont10** | ~10 px | ✅ wszystkie | ✅ | ❌ | ✅ | podpisy, etykiety, opisy |
| **PlFont14** | ~13 px | ✅ wszystkie | ✅ | ❌ | ❌ | wartości, nagłówki sekcji |
| **PlFont18** | ~18 px | ✅ wszystkie | ✅ | ❌ | ❌ | duże wartości |
| **FreeSansBold 24pt** | ~34 px | ❌ **brak** | ❌ | ❌ | ❌ | tylko wielkie liczby (temperatura) |
| **RetroFont 8×8** | 8 px × skala | ❌ **brak** | ✅ (jako `*`) | ❌ | ❌ | ekran retro; skalowalny 1×–6× |

### Pułapki, które już raz kosztowały nas błędy na ekranie

1. **Znak mikro `µ` nie istnieje w żadnym foncie.** Dlatego jednostka pyłu zawieszonego jest pisana `ug/m3`, a nie `µg/m³`. Brakujący glif nie wywala programu — jest **po cichu pomijany**, więc „µg/m³" wyświetli się jako „g/m". To wygląda jak literówka, nie jak błąd techniczny.

2. **Wielkie liczby (FreeSansBold) nie znają polskich znaków ani stopnia.** Temperatura „24" jest rysowana tym fontem, ale „°C" obok — już innym.

3. **Dwa różne układy współrzędnych.** Funkcje `gl…` kotwiczą tekst **górną krawędzią**, funkcje `pl…` — **linią bazową** (dolną krawędzią liter, bez ogonków). Pomylenie ich przesuwa tekst o wysokość fontu. Projektant powinien podawać pozycje jako **prostokąty, w których tekst ma się zmieścić**, a nie punkty.

4. **Wielokropek `…`, myślnik `—`, punktor `•`, cudzysłowy typograficzne — nie istnieją.** Używać `...`, `-`, `:`.

**Rekomendacja dla projektanta:** jeśli V2 ma mieć inny charakter typograficzny, można dorobić nowy font bitmapowy (tak zrobiliśmy dla ekranu retro — 8×8, koszt ~3 kB flasha). Trzeba to zgłosić na etapie projektu, bo wymaga wygenerowania i wkompilowania.

---

## 6. Kolory

W projekcie jest **52 zdefiniowanych kolorów** pogrupowanych semantycznie: tła i karty, teksty (3 stopnie jasności), pogoda (słońce, chmura, deszcz, śnieg, burza, wiatr, wilgotność, ciśnienie), temperatura (5 progów od mrozu do upału), fotowoltaika (produkcja, oddawanie, pobór, dom), stany (OK / uwaga / awaria), mapa (morze, ląd, wybrzeże), loty, jakość powietrza (6 klas).

Tło jest **ciemne** (prawie czarny granat). To celowe: urządzenie wisi w łazience, także w nocy.

Projektant może zaproponować **własną paletę dla V2** — nie ma tu ograniczenia technicznego poza RGB565. Trzeba tylko pamiętać, że przy najniższym podświetleniu (45/255) ciemne kolory zlewają się ze sobą.

---

## 7. Grafika, którą już mamy

- **Mapa Zatoki Gdańskiej** — dwie wersje, generowane skryptem z danych Natural Earth: wąska (111 km, do ekranu samolotów) i szeroka (300 km, do radaru opadów). Format: linie poziome lądu, bardzo tani w rysowaniu. Można wygenerować dowolny inny wycinek/skalę.
- **Ikony pogody** — komplet, rysowane wektorowo z kształtów (nie bitmapy), skalowane, źródło 64 px.
- **Sprite Mario 16×16** — 4 klatki, na ekran retro.
- **Fazy księżyca** — rysowane proceduralnie.

Wszystko powyżej można **przeprojektować lub wygenerować od nowa** — pipeline istnieje.

---

## 8. Dane — pełny katalog tego, co urządzenie ma do pokazania

To jest odpowiedź na pytanie „czym dysponuję". Wszystkie dane są dostępne dla warstwy rysującej jako gotowe struktury.

### 8.1 Pogoda (Open-Meteo, odświeżanie co 15 min)
Teraz: temperatura, odczuwalna, wilgotność, zachmurzenie, wiatr (prędkość + kierunek), ciśnienie, opad, prawdopodobieństwo opadu, indeks UV (surowy i skorygowany o chmury), dzień/noc, kod pogody.
Prognoza godzinowa: 24 h. Prognoza dzienna: 5 dni (min/max, opad, UV, wschód/zachód słońca).

### 8.2 Fotowoltaika (falownik Huawei, Modbus TCP, co 30 s)
Moc DC, moc AC, bilans sieci (+oddawanie / −pobór), **wyliczony pobór domu**, energia dzisiaj, energia całkowita, napięcie, temperatura falownika, sprawność, status.
Historia dnia: profil produkcji i poboru co 5 min (288 punktów).

### 8.3 Ogrzewanie (Viessmann, chmura ViCare)
Temperatura CWU i zasilania, stan palnika, modulacja, **godziny pracy palnika**, liczba startów, zużycie gazu dzisiaj. Historia palnika. Osobno: ręcznie wprowadzane odczyty licznika gazu z porównaniem do danych z pieca.

### 8.4 Czujniki w domu (Bluetooth, do 8 sztuk)
Temperatura, wilgotność, poziom baterii, siła sygnału, źródło odbioru (bezpośrednio czy przez bramkę). Historia 24 h z 6 pomieszczeń.

### 8.5 Radar opadów (RainViewer)
13 klatek co 10 min (2 h wstecz), mapa 300 km, 6 poziomów intensywności opadu, animacja z interpolacją ruchu.

### 8.6 Samoloty (co 15 s)
Znak wywoławczy, typ, trasa, pozycja, wysokość, prędkość, kurs. Rozróżnienie: ląduje w Gdańsku / startuje / przelot.

### 8.7 Jakość powietrza (miejska sieć Gdyni, co 15 min)
PM10, PM2.5, indeks jakości (6 klas, liczony lokalnie), nazwa stacji, wiek pomiaru, temperatura/wilgotność/ciśnienie ze stacji.

### 8.8 Czujniki własne
Natężenie światła (fotorezystor) — histogram dobowy, czas na każdym poziomie podświetlenia, wykryte zdarzenia „zostawione światło". Czujnik ruchu — rytm doby (24 słupki), rozkłady długości impulsów i przerw. Oba zbierane tygodniami, przeżywają restarty.

### 8.9 Stan samego urządzenia
Czas pracy, restarty i ich przyczyny, temperatura procesora, pamięć (wszystkie rodzaje: SRAM z fragmentacją, PSRAM, flash z partycjami, RTC, ROM, stosy zadań), jakość Wi-Fi, statystyki MQTT, liczniki awarii poszczególnych źródeł danych, wydajność rysowania.

---

## 9. Ekrany istniejące — **wygląd V1** (zostaje bez zmian)

Rotacja obejmuje 13 ekranów:

`RETRO` · `TERAZ` · `GODZINY` · `RADAR` · `5 DNI` · `W DOMU` · `PIEC` · `FOTOWOLTAIKA` · `SAMOLOTY` · `POWIETRZE` · `PAMIĘĆ` · `RUCH` · `STATYSTYKI`

Niektóre są **pomijane automatycznie**, gdy nie mają czego pokazać: radar bez opadu, „w domu" bez czujników, piec bez autoryzacji, powietrze bez danych ze stacji.

**Wygląd V1 nie podlega przeprojektowaniu.** Ma zostać dokładnie taki, jaki jest.

---

## 10. Zadanie dla projektanta — **wygląd V2**

Zaprojektować **alternatywny, kompletny wygląd** tego samego zestawu danych, w stylistyce **retro** (konsole i komputery 8/16-bitowe, przełom lat 80. i 90.).

Punkt odniesienia: właściciel pokazał grafikę z gry platformowej w stylu Mario — ceglane platformy, pikselowe chmury, duże słońce, ograniczona paleta, twarde krawędzie, wyraźny podział na pasy interfejsu (górny i dolny) i scenę pomiędzy nimi. Zrobiliśmy już jeden taki ekran (RETRO) jako próbę — jest w firmware i można go obejrzeć na żywo.

### Oczekiwany rezultat

1. **Pytania do właściciela** — projektant ma je zadać przed rysowaniem. To jest wprost oczekiwane, nie opcjonalne.
2. **Jeden lub trzy mockupy** (do decyzji projektanta) nowego wyglądu, w rozdzielczości **dokładnie 320×240 px**, bez wygładzania krawędzi.
3. Zaczynamy od **jednego ekranu** jako próbki stylu — potem reszta.

### Wymagania techniczne dla V2

- Rozdzielczość 320×240, kolor RGB565
- Respektowanie granicy y=205 (treść) / y=206+ (stopka)
- Fonty: istniejące albo nowy bitmapowy (do zgłoszenia)
- Bez przezroczystości i cieni rozmytych
- Czytelność z 3 m przy najniższym podświetleniu
- Budżet: ~18 ms na narysowanie treści ekranu

---

## 11. Przełączanie V1 / V2 — wymaganie funkcjonalne

W panelu administracyjnym WWW mają pojawić się **dwa przyciski: „Wygląd V1" i „Wygląd V2"**. Wybór ma być zapamiętywany w pamięci nieulotnej urządzenia (przeżywać restart i aktualizację).

**Konsekwencja techniczna, którą projektant powinien znać:** dwa wyglądy oznaczają dwa komplety kodu rysującego w tym samym firmware. Przy 260 kB wolnego flasha jest to wykonalne, ale nie jest darmowe — im bardziej V2 opiera się na tych samych mechanizmach co V1 (te same fonty, te same prymitywy rysowania), tym taniej wychodzi. Zupełnie nowy zestaw fontów i grafik dla 13 ekranów może nie zmieścić się w budżecie i wtedy trzeba będzie coś poświęcić.

---

## 12. Czego nie robić — lekcje z tego projektu

Te punkty pochodzą z realnych błędów, które trafiły na ekran i musiały być naprawiane:

1. **Nie zakładaj, że font ma znak.** Brakujący glif znika po cichu.
2. **Nie rysuj elementów przez granicę y=205.**
3. **Nie używaj losowości bez stałego ziarna.** Element rysowany losowo co klatkę **miga** 20 razy na sekundę. Miasto w tle ekranu retro musiało dostać deterministyczny generator.
4. **Nie licz czasu wewnątrz rysowania.** Zrzut ekranu dla panelu WWW renderuje obraz w paskach — osobne odczyty zegara rozjeżdżają obraz. Czas musi być pobrany raz na klatkę i przekazany.
5. **Nie pokazuj zera zamiast braku danych.** Nieruchome „0" czyta się jak prawdziwy pomiar. Brak danych musi wyglądać jak brak danych.
6. **Nie ukrywaj źródła.** Jeśli dane mogą pochodzić z dwóch miejsc (np. stacja główna albo zapasowa, czujnik bezpośrednio albo przez bramkę), na ekranie musi być widać, z którego.
7. **Kolor ma opisywać tę liczbę, którą pokazuje.** Mieliśmy kartę kolorowaną według innej wartości niż wyświetlana — świeciła na czerwono, pokazując zdrową wartość.

---

## 13. Jak dostarczyć projekt

Mockupy jako pliki PNG **1:1 (320×240)**, bez skalowania i wygładzania. Dodatkowo mile widziana wersja powiększona (×2 lub ×3, metodą „najbliższy sąsiad") do oglądania.

Do każdego ekranu: krótki opis, **które dane** są pokazane i **skąd** (odwołanie do punktu 8), oraz co się dzieje, gdy danych brakuje.

---

## 14. Materiały pomocnicze

- Kod źródłowy: repozytorium podane na górze (publiczne)
- Zrzuty ekranów na żywo: urządzenie udostępnia bieżący obraz przez HTTP
- Istniejący ekran RETRO jako punkt wyjścia stylistyczny
- Ten dokument opisuje stan na firmware v117
