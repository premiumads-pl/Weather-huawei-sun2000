# Raport nocny — 23 lipca (przegląd poranny)

## 1. Co zrobiłem i opublikowałem

### v140 → v141 (ekran POKOJE, wg Twojego feedbacku)
Przeprojektowałem ekran POKOJE na wykres z opisanymi osiami i dużymi wartościami, potem naniosłem Twoje dwie poprawki:

- **Duże wartości w dwóch kolumnach u góry** (nazwa + kolorowa kropka wiążąca z linią wykresu), a pod spodem **ściśnięty wykres** ze wspólną osią °C i osią czasu (godziny zegara + „teraz").
- **Wilgotność** przy każdej temperaturze.
- **Źródło sygnału**: „Shelly" (przez bramkę BLE) albo „ESP" (wprost do płytki) — dla każdego czujnika.

### Złapałem i naprawiłem błąd, którego nie było widać w makiecie
Duże temperatury szły **fontem zegara (f24)** — a on ma tylko 14 glifów (cyfry, `:`, `°`, `.`), **bez przecinka**. Efekt: „21,2°" wyświetlałoby się jako **„212°"**. Wykryłem to analizując dane fontów (nie na urządzeniu — Mac stracił w nocy WiFi). Naprawa: temperatury na **f20** (pełne 120 glifów, przecinek wraca).

Wersje: **v141** opublikowana na GitHub (release + firmware.bin). Urządzenie pobiera z „latest" co 15 min. RAM statyczny **73768 B** bez zmian (limit 76000), adresy RTC gPir/gLdr nienaruszone, kompilacja czysta.

---

## 2. ⚠️ Wymaga Twojej uwagi rano

1. **Tryb nocny jest nadal WYŁĄCZONY.** Wyłączyłem go na Twoją prośbę (żebyś mógł przeglądać ekrany), planowałem przywrócić po robocie — ale **Mac stracił WiFi** i nie mam jak dosięgnąć wyświetlacza, żeby cofnąć. Skutek: **wyświetlacz w łazience świeci jasno całą noc** (podświetlenie 170 zamiast 45, okno nocne 6–6 = wyłączone). Przywrócę automatycznie, gdy łączność wróci. Możesz też sam: panel → Ekran → noc **22:00–06:00**, jasność nocna **45**.

2. **v141 nie jest zweryfikowana na żywo** (w nocy nie miałem dostępu do LAN wyświetlacza). Ale: build zielony, bramy RAM/RTC przeszły, a układ i pokrycie fontów **policzyłem offline** (temperatura „22,1°" = 55 px, nie nachodzi na wilgotność). Rano rzuć okiem na ekran POKOJE — jeśli coś się nie zgadza, poprawię.

---

## 3. Panel — prototyp „Obecność · Światło · Ruch"

Zbudowałem **działający prototyp** sekcji panelu z Twoimi realnymi danymi z czujników PIR i LDR (plik `panel-obecnosc-prototyp.html` + podgląd w czacie). Co pokazuje:

- **Kiedy najczęściej ktoś jest w łazience** — wykres godzinowy. Twoje szczyty: **22:00, 7:00, 10:00, 11:00, 20:00** (rano 7–11 i wieczór 20–22).
- **Karty**: ~10 wejść/dobę, ~13 min aktywnego ruchu/dobę, najruchliwsza 22:00, światło jasne ~5,2 h/dobę.
- **Światło**: ciemno 77% / jasno 23% czasu (kandydat na „zostawione światło", gdy jasno > obecność).
- **Długość ruchu**: głównie krótkie impulsy 1–3 s (78%).

Dane liczone z okna obserwacji (`collected_s`), nie z uptime. Prototyp jest **gotowy do wpięcia do panelu urządzenia** — nie zrobiłem tego na ślepo w nocy, bo nie mógłbym zweryfikować, jak wygląda w panelu. Jak zaakceptujesz układ (i wróci łączność), wpinam to jako żywą sekcję (`v142`).

---

## 4. Propozycje dalszej rozbudowy panelu

Uszeregowane od „mamy dane, łatwo" do „wymaga dozbierania":

**A. Z danych, które już mamy (szybkie):**
- **Detektor „zostawione światło"**: alert, gdy jasno > N min bez ruchu (LDR wysoki + PIR cicho). Dane są, logika prosta.
- **Klimat pokoi — trend wilgotności**: wykres 24 h wilgotności per pokój; ostrzeżenie o **ryzyku pleśni** (wysoka wilgotność + niska temperatura, typowo łazienka po prysznicu).
- **Zdrowie urządzenia w czasie**: uptime, liczba restartów, skuteczność alokacji radaru w PSRAM przy starcie (dziś czasem „pomiar punktowy"), RSSI/roamy WiFi, trend pamięci.
- **Czujniki — bateria i łączność**: % baterii, ostatni odczyt, źródło (Shelly/ESP) w czasie, alert „czujnik milczy > 15 min".
- **Prąd/PV — historia doby**: produkcja vs pobór, % autokonsumpcji, najlepszy/najgorszy dzień, szacunek oszczędności.
- **Piec (Viessmann)**: historia CWU, czas pracy palnika/dobę, zużycie gazu, przypomnienie o odnowieniu tokenu.

**B. Wymaga trochę więcej (dozbieranie / nowe liczniki):**
- **Sesje w łazience**: sklejanie impulsów PIR w „wizyty" (długość, pora), zamiast surowych impulsów — dokładniejsze „ile czasu przebywają".
- **Powietrze — historia**: trend PM2.5/PM10, o której dobie najgorzej.
- **Reguły/alerty konfigurowalne**: progi (wilgotność > X, zimno < Y, światło zostawione > Z min) z powiadomieniem (MQTT/Home Assistant).
- **Eksport CSV**: pobranie historii (pokoje, PV, ruch) do arkusza.
- **Log zdarzeń pogodowych**: kiedy „nad domem padało" (ile razy odpalił pomiar punktowy radaru).

**C. Wygody UX panelu:**
- Ciemny motyw panelu (dziś jasny „Pasmowy") do wieczornego przeglądania.
- Zakładka „Na żywo" z auto-odświeżaniem co 5–10 s (obecność, ruch, prąd).
- Mini-podgląd ekranu wyświetlacza (JPEG) odświeżany automatycznie.

Powiedz, które z tego chcesz, a poukładam w kolejność i zrobię — sekcja po sekcji, z weryfikacją.

---

## 5. Zaległości / do decyzji (z całej rozmowy)

- **Integracja prototypu panelu do firmware** (`v142`) — czeka na Twój OK do układu + stabilną łączność.
- **Tryb nocny „dotyk budzi UI"**: żebyś w nocy nie był zablokowany na zegarze — dotyk pokazuje normalny ekran na chwilę, potem wraca. Dobry pomysł po dzisiejszej sytuacji; mogę dorobić.
- **Loty `ok_ago_s = -1`**: od startu nie pobierają się (endpoint/filtr). Nie ruszałem na ślepo w nocy — zbadam z dostępem do urządzenia.
- **Stan awaryjny 7e** (pogoda niepobrana = myślniki) i **noc przy świetle** (makieta 11) — drobne, do dokończenia.
- **Radar bez PSRAM przy starcie**: dziś widziałeś „pomiar punktowy". To działa jak zaprojektowany tryb awaryjny (7d), ale warto utwardzić alokację przy starcie, żeby mapa wchodziła pewniej.

---

## 6. Stan techniczny
- Firmware: **v141** (GitHub release, `origin/main` = HEAD, drzewo czyste).
- RAM statyczny: **73768 B** / 76000. Flash: ~1,79 MB. RTC: gPir @0x50000200, gLdr @0x500002c0 — nienaruszone.
- Do zrobienia po powrocie łączności: potwierdzić fw=141 zrzutem POKOJE + **przywrócić tryb nocny**.
