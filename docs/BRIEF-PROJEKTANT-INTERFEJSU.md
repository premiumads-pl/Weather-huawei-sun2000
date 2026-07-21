# Brief: interfejs wyświetlacza domowego

**Wersja 2.0 — projekt od zera**
**Data: 21 lipca 2026**

---

## 0. Zanim zaczniesz — jak czytać ten dokument

Ten brief jest napisany tak, żebyś **nie zobaczył**, jak urządzenie wygląda dzisiaj. To celowe i jest to najważniejsza decyzja w całym zleceniu.

Poprzednia próba przeprojektowania interfejsu poszła inaczej: projektant dostał zrzuty istniejących ekranów „dla kontekstu". Efekt był przewidywalny — nowy projekt okazał się przemalowaniem starego. Te same podziały ekranu, te same rzeczy obok siebie, ta sama hierarchia. Zmieniły się kolory i krój pisma, nie sposób myślenia o problemie.

Dlatego tym razem dostajesz **dane, nie ekrany**. Twoim materiałem wyjściowym jest lista tego, co urządzenie wie o świecie, plus opis tego, komu i w jakich warunkach ma to pokazać. Układ, podział na widoki, hierarchia, typografia, ikonografia — wszystko to jest **twoją decyzją i przedmiotem zlecenia**.

Jeżeli w trakcie pracy zapytasz „a jak to jest teraz zrobione?", odpowiedź brzmi: nie powiem, i to nie jest złośliwość. Chcę zobaczyć, co wymyślisz, mając te same dane i żadnego kotwiczenia.

**Możesz zaproponować wszystko od nowa:** własne kroje pisma, własne bitmapy, własne ikony pogodowe, własne mapy, własną paletę, własną liczbę i kolejność ekranów, własny sposób nawigacji. Nie ma niczego, co trzeba zachować „bo tak było".

---

## 1. Czym jest to urządzenie

Mały wyświetlacz zawieszony na stałe na ścianie w **łazience** domu jednorodzinnego pod Gdynią. Działa bez przerwy, nie jest do niczego podłączony poza zasilaniem, nikt go nie obsługuje — po prostu wisi i pokazuje.

Zbiera dane z kilkunastu źródeł: prognoza pogody, radar opadów, miejska sieć pomiaru jakości powietrza, własna instalacja fotowoltaiczna, piec gazowy, czujniki temperatury w pokojach, samoloty przelatujące nad domem, oraz stan samego siebie.

Nie jest to ani panel sterowania (niczym nie steruje), ani dashboard (nikt się w niego nie wpatruje), ani zegar (godzina jest tylko jedną z informacji). Najbliższą analogią jest **okno**: patrzysz przez nie przelotnie i coś wiesz.

---

## 2. Kto na to patrzy i jak

To jest kluczowa część briefu, ważniejsza niż lista danych.

**Sytuacja obserwacji:**

- Odległość patrzenia: **około 2 metry**. To jest twardy wymóg, nie przybliżenie — wynika z geometrii pomieszczenia.
- Czas patrzenia: **kilka sekund**, zwykle mimochodem. Ktoś myje zęby i zerka. Ktoś wchodzi i wychodzi.
- Nikt nie czeka, aż urządzenie coś pokaże. Jeśli w momencie spojrzenia na ekranie jest akurat coś nieinteresującego, ta informacja jest po prostu stracona.
- Obserwator jest często **bez okularów**, czasem zaparowany, czasem w ruchu.
- Widzowie to cała rodzina, w tym dzieci.

**Z tego wynika zasada nadrzędna, która rozstrzyga każdy spór projektowy:**

> **Czytelność bije estetykę.**

Jeżeli w jakimkolwiek miejscu projektu trzeba wybrać między „ładniej" a „czytelniej z dwóch metrów bez okularów" — wybierasz czytelniej. Elegancki, subtelny, cienki, niskokontrastowy projekt jest w tym zastosowaniu projektem **złym**, niezależnie od tego, jak dobrze wygląda w prezentacji na monitorze.

**Praktyczny test, którym będę oceniał makiety:** wyświetlam je w skali 1:1, odchodzę na dwa metry, patrzę przez trzy sekundy i odwracam wzrok. Co zapamiętałem? Jeśli odpowiedź brzmi „że było ładne" — projekt nie przeszedł.

**Warunki fizyczne:**

- Łazienka bywa zaparowana. Kontrast musi to znieść.
- Oświetlenie zmienia się skrajnie: od pełnego światła po zupełną ciemność w nocy.
- Urządzenie samo ściemnia ekran (patrz punkt 4), więc projekt musi działać także wtedy, gdy jasność podświetlenia spada do ~18%. Kolory bliskie czerni zlewają się wtedy z tłem.

---

## 3. Sprzęt i twarde ograniczenia

Te liczby są zweryfikowane w kodzie i pomiarach na urządzeniu. Możesz na nich polegać.

| Parametr | Wartość | Uwaga |
|---|---|---|
| Rozdzielczość | **320 × 240 px**, poziomo | To wszystko. Nie ma trybu wysokiej rozdzielczości. |
| Głębia koloru | 16 bitów (65 536 kolorów) | RGB565: 5 bitów czerwony, **6 zielony**, 5 niebieski. Gradienty w niebieskim i czerwonym będą się pasmować. |
| Odświeżanie | **20 klatek/s** | Pomiar z urządzenia. Samo narysowanie i wysłanie klatki zajmuje ~41 ms, a odstęp między klatkami to 50 ms — czyli budżet jest wypełniony w ~80%. Animacja jest możliwa, ale zapasu jest niewiele. |
| Pamięć na zasoby | **~341 kB wolnego flasha** | Twardy sufit na wszystkie nowe czcionki, bitmapy i mapy **razem**. |
| Pamięć na duże bufory | **~1,1 MB wolnego** (PSRAM) | Tu mieszczą się bufory graficzne. Dla skali: animacja radaru zajmuje w niej 715 kB. |

**O budżecie flasha — liczba, którą warto rozumieć dokładnie.**

Wolnego miejsca jest **~341 kB**, ale to jest zapas *ponad* zasoby, które są tam dzisiaj. Ponieważ projektujesz od zera, obecne zasoby graficzne (ikony pogodowe, kroje pisma, dane map — razem ok. **98 kB**) zostaną wyrzucone. Twój realny budżet to więc **około 440 kB**.

Dla skali kosztów: komplet ikon pogodowych zajmuje 64 kB w formacie z paletą indeksowaną (przed optymalizacją zajmował 98 kB). Trzy rozmiary obecnego kroju pisma z polskimi znakami to razem tylko ~9 kB — czcionki bitmapowe są tanie, więc nie oszczędzaj na nich przesadnie. Zdjęcie tła w pełnej rozdzielczości (320×240×2 B = 150 kB) zmieści się, ale zje jedną trzecią budżetu.

Zaprojektuj więc z myślą o **oszczędności zasobów**: rysowanie prymitywami (linie, prostokąty, koła, wypełnienia) jest darmowe i nieograniczone, bitmapy kosztują. Jeśli efekt da się osiągnąć geometrią zamiast obrazkiem — zrób to geometrią. Jeśli potrzebujesz bitmap, projektuj je tak, by miały mało kolorów (paleta indeksowana 16 lub 256 kolorów zamiast pełnego koloru to oszczędność 2–4×).

**Typografia — uwaga praktyczna z doświadczenia:**

- Polskie znaki diakrytyczne są **obowiązkowe** (ą ć ę ł ń ó ś ź ż). Interfejs jest po polsku.
- Znak stopnia `°` jest niezbędny (temperatury).
- Uwaga na jednostkę **µg/m³** (jakość powietrza): znak `µ` i indeks górny `³` bywają pomijane w okrojonych zestawach znaków. Albo je uwzględnij, albo zaprojektuj zapis, który ich nie potrzebuje.
- Podaj, których dokładnie znaków wymaga twój projekt. Każdy dodatkowy zestaw znaków to flash.

---

## 4. Jak człowiek wchodzi w interakcję z urządzeniem

Urządzenie nie ma klawiatury, myszy, przycisków ani pokrętła. Ekran **nie jest dotykowy**. Wejścia są cztery:

**1. Dotyk** — **jedna elektroda pojemnościowa w konkretnym punkcie** obudowy (nie cały ekran; ekran nie jest dotykowy). Nie ma współrzędnych, nie ma pozycji, nie ma siły nacisku — jest tylko „dotknięto / nie dotknięto". Rozróżnia **pojedyncze** i **podwójne** stuknięcie.

Dwie rzeczy, które musisz uwzględnić:

- **Pojedyncze stuknięcie jest zgłaszane z opóźnieniem ~550 ms**, bo tyle trzeba czekać, żeby stwierdzić, że nie nadchodzi drugie. Reakcja na pojedynczy dotyk nigdy nie będzie natychmiastowa. Jeśli twój projekt wymaga natychmiastowej odpowiedzi, oprzyj ją na podwójnym stuknięciu albo powiedz, że rezygnujemy z rozróżniania i wtedy pojedyncze działa od razu.
- To jedyna świadoma interakcja użytkownika. Masz nad nią pełną władzę projektową; dziś przypisane są dwie funkcje, ale możesz zaproponować inne. Jeśli dwa gesty to za mało dla twojego projektu — powiedz to i uzasadnij, bo zmienia to wymagania sprzętowe.

**2. Ruch w pomieszczeniu** — czujnik wykrywa **ruch, a nie obecność**. To istotna różnica: ktoś, kto stoi nieruchomo przy lustrze, przestaje być wykrywany. „Brak ruchu" nie znaczy „nikogo nie ma".

Dziś jest to wyłącznie pomiar — czujnik **nie steruje** niczym, tylko zapisuje statystykę rytmu doby. Możesz zaproponować, żeby interfejs z tego korzystał (inne zachowanie tuż po wykryciu ruchu niż po godzinie ciszy), ale projektuj z założeniem, że sygnał bywa fałszywie negatywny.

**3. Jasność otoczenia** — czujnik światła mierzy, jak jasno jest w pomieszczeniu, i rozróżnia trzy stany: **światło zapalone**, **półmrok**, **ciemno**. Urządzenie automatycznie ustawia jasność podświetlenia (odpowiednio 100%, 51%, 18%).

Warto zaprojektować **osobne warianty wizualne dla tych stanów**, a nie tylko zdać się na przyciemnienie podświetlenia. W nocy, przy 18% jasności, projekt oparty na ciemnych szarościach zniknie zupełnie; sensowniejszy może być wtedy zupełnie inny, minimalny obraz — mniej informacji, większy kontrast, może tylko to, co naprawdę potrzebne o trzeciej w nocy.

Uczciwe zastrzeżenie: **nie wiemy, czy stan środkowy („półmrok") w ogóle bywa osiągany** w tej konkretnej łazience — mierzymy to dopiero teraz. Zaprojektuj przede wszystkim dwa skrajne stany (pełne światło / ciemno), a wariant pośredni potraktuj jako opcjonalny.

**4. Panel WWW** — jest jeszcze czwarte wejście, tyle że nie dla domowników: panel w przeglądarce, z którego można przypiąć konkretny widok, przełączyć wygląd i wymusić jasność. Używam go ja, do konfiguracji i diagnostyki. Projektujesz go w fazie 2 (punkt 8).

---

## 5. Cykl życia urządzenia — w tym uruchamianie

**To jest część zlecenia, o której łatwo zapomnieć, a jest widoczna codziennie.**

Po włączeniu zasilania (albo po automatycznej aktualizacji, która zdarza się co kilka dni) urządzenie startuje. Poniżej **realne, zmierzone czasy** z logu uruchomienia — nie szacunki:

| Czas od włączenia | Co się dzieje | Co urządzenie już wie |
|---|---|---|
| 0,0–0,7 s | Start układu | nic |
| 0,7–1,3 s | Pamięć, czujniki lokalne, wczytanie zapisanych profili | historia produkcji prądu, pokoi, gazu |
| 1,3–1,9 s | Nasłuch czujników bezprzewodowych, dotyk, przygotowanie mapy | — |
| **5,6 s** | **Połączenie z siecią Wi-Fi** | adres w sieci |
| 5,6–6,5 s | Połączenie z serwerem domowym | — |
| **6,4 s** | **Pierwsza prognoza pogody** | pogoda bieżąca i prognozy |
| 6,5 s | Jakość powietrza | pyły zawieszone, indeks |
| **9,7–10,6 s** | **Połączenie z falownikiem** | produkcja prądu, pobór |
| 11,4 s | Pierwsza klatka radaru | opady |
| 13,2 s | Czujniki w pokojach | temperatury w domu |
| **do 15,6 s** | **Ekran z adresem urządzenia w sieci — wisi 10 sekund** | — |
| 15,6 s | Pierwsze ustawienie jasności podświetlenia | poziom światła |
| **34,2 s** | **Komplet 13 klatek radaru** | animacja opadów |
| 36,6–37,8 s | Autoryzacja i dane pieca | ogrzewanie, gaz |

**Wniosek dla projektu:** przez pierwsze pół minuty urządzenie zapełnia się danymi **stopniowo**, w tej kolejności. Nie ma momentu „teraz jest gotowe" — jest ciągłe narastanie.

Dziś sekwencja startowa to kilka kolejnych komunikatów tekstowych („Łączenie z WiFi…", „Synchronizacja czasu…", „Pobieram prognozę…") oraz — co ważne — **pełnoekranowa plansza z adresem urządzenia w sieci, nazwą sieci i siłą sygnału, wisząca równe 10 sekund przy każdym starcie**. Ta plansza jest potrzebna mnie (bez niej nie wiem, pod jakim adresem szukać panelu po zmianie sieci), ale zajmuje jedną trzecią całego czasu uruchamiania i widzi ją cała rodzina.

To jest zmarnowana okazja: pierwsze trzydzieści sekund po każdej aktualizacji to jedyny moment, kiedy urządzenie ma **coś do opowiedzenia o sobie**.

**Zaprojektuj sekwencję uruchamiania.** Masz swobodę: może to być ekran powitalny, może animacja zapełniania, może progresywne pojawianie się kolejnych kafelków w miarę, jak dane spływają, może coś zupełnie innego. Wymagania:

- Musi być zrozumiałe dla kogoś, kto nie wie, co to jest ESP32 — członek rodziny ma widzieć „urządzenie się budzi", a nie komunikat techniczny.
- Nie może sugerować awarii. Powolny start ma wyglądać na normalny, bo jest normalny.
- Musi znieść przypadek, w którym któreś źródło **nigdy nie odpowie** (np. brak internetu) — sekwencja nie może się wtedy zawiesić na wieczność w stanie „ładowanie".
- Musi pomieścić **informację techniczną dla mnie** (adres w sieci, nazwa sieci, siła sygnału). Nie musi to być osobna plansza na 10 sekund — chętnie zobaczę pomysł, jak podać to elegancko, nie kradnąc jednej trzeciej startu. Może to być np. element widoczny tylko dopóki dane nie napłyną.

---

## 6. Dane, którymi dysponujesz

Pełny katalog jest w osobnym pliku: **`KATALOG-DANYCH.md`** — około **290 pól** w dwunastu grupach, z typami, jednostkami, zakresami, częstotliwością odświeżania i zachowaniem przy awarii źródła.

Grupy: pogoda bieżąca · prognoza godzinowa (12 punktów: od +1 h do +12 h) · prognoza dzienna (5 dni: **od jutra**, bez dzisiaj) · radar opadów · jakość powietrza · fotowoltaika · ogrzewanie i gaz · czujniki w pomieszczeniach · samoloty · diagnostyka urządzenia · czujniki lokalne · alarmy.

Przeczytaj ten katalog **przed** rozpoczęciem szkicowania. Nie musisz wykorzystać wszystkiego — 290 pól na 320×240 pikseli to fizyczna niemożliwość i **selekcja jest częścią twojej pracy**. Chcę zobaczyć twoją hierarchię ważności, nie kompletność.

Jedna wskazówka co do selekcji: część tych danych jest ciekawa dla mnie jako właściciela urządzenia (diagnostyka, statystyki pamięci, liczniki awarii), a część dla rodziny (czy padać będzie, czy zimno, ile prądu produkujemy). To nie muszą być te same ekrany i nie muszą być tak samo dostępne.

---

## 7. Trzy problemy, na których ten projekt się rozstrzygnie

To są realne właściwości danych, odkryte przy inwentaryzacji. Projekt, który je zignoruje, będzie ładny i będzie kłamał.

### 7.1. Prawie każda wartość ma trzeci stan: „jest, ale nieświeża"

Urządzenie **nigdzie nie wygasza danych automatycznie**. Prognoza sprzed dwóch dni wyświetla się identycznie jak sprzed pięciu minut. Jeśli internet padnie w nocy, rano ekran pokaże wczorajszą pogodę z pełnym przekonaniem.

Interfejs zaprojektowany na dwa stany („jest wartość / brak wartości") będzie kłamał dokładnie w tych sytuacjach, w których uczciwość ma największą wartość.

**Zaprojektuj trzeci stan.** Jak wygląda liczba, która jest prawdziwa, ale sprzed dwóch godzin? A sprzed dwóch dni? Czy to samo rozwiązanie działa dla temperatury (zmienia się wolno) i dla produkcji prądu (zmienia się co sekundę)? Różne dane starzeją się w różnym tempie i to jest projektowo interesujące.

Najgorszy przypadek: **samoloty nie mają żadnego znacznika wieku**. Nie da się odróżnić samolotu, który leci teraz, od takiego, który przeleciał kwadrans temu.

### 7.2. Zero bywa prawdą i bywa awarią

Fotowoltaika jest jedynym źródłem, które przy awarii **zeruje widoczne wartości** zamiast zostawić ostatnie znane. Wartość „0 W" znaczy jednocześnie „jest noc, nic nie produkujemy" i „padło połączenie z falownikiem". Rozróżnia je wyłącznie flaga poprawności danych.

Dodatkowo trzy pola (napięcie, temperatura falownika, sprawność) **nie mają w ogóle flagi obecności** i przy nieudanym odczycie trafiają na ekran jako wiarygodnie wyglądające zero.

Zaprojektuj to tak, żeby „nic nie produkujemy, bo noc" i „nie wiem, co się dzieje" wyglądały **inaczej**. To nie jest ten sam komunikat.

### 7.3. Całe grupy danych znikają w normalnej pracy

To nie są awarie, tylko normalne działanie:

- **Falownik śpi** od 30 minut po zachodzie do 30 minut **po** wschodzie. Zimą to **16 godzin na dobę** — czyli stan dominujący, nie wyjątek.
- **Temperatura, wilgotność i ciśnienie z sieci miejskiej znikają w komplecie**, gdy stacja główna milknie i włącza się zapasowa (ta ma mniej czujników).
- **Autoryzacja pieca wygasa** co około 180 dni i nie odnawia się sama.
- Czujniki bezprzewodowe w pokojach mogą być **niezainstalowane** — może ich nie być w ogóle.

Układ zaprojektowany na komplet danych będzie miał dziury przez większość zimowej doby. **Nie projektuj ekranów, które wyglądają dobrze tylko wtedy, gdy wszystko działa.** Pokaż mi, jak wygląda styczniowy wieczór, kiedy nie ma słońca, piec stracił autoryzację, a jedna stacja pomiarowa milczy.

Osobno: **żaden ekran nie powinien być pusty**. Jeżeli dane, które miał pokazać, nie istnieją, ma pokazać coś sensownego zamiast pustego prostokąta.

---

## 8. Czego oczekuję jako produktu

### Faza 1 — interfejs wyświetlacza (właściwe zlecenie)

**a) Makiety wszystkich zaprojektowanych ekranów**
Skala 1:1, dokładnie 320 × 240 px, PNG lub Figma. Dla każdego ekranu potrzebuję też **wariantów stanu**: komplet danych, dane częściowe, dane nieświeże, brak danych. To nie jest dodatek — na podstawie punktu 7 to jest sedno.

**b) System wizualny**
Siatka, marginesy, skala typograficzna, paleta z podaniem wartości kolorów, biblioteka komponentów (jak wygląda „wartość z jednostką", „nagłówek", „stan ostrzegawczy", „element nieaktywny"). Zasady, nie tylko obrazki — dzięki nim zaprojektuję samodzielnie te ekrany, których nie narysujesz, i będą spójne z resztą.

**c) Pliki źródłowe zasobów**
Kroje pisma (z licencją pozwalającą na osadzenie w urządzeniu), bitmapy, ikony — w formacie źródłowym, nie tylko jako zrzuty. Bez nich makieta pozostaje obrazkiem, którego nie odtworzę wiernie. Przy każdym zasobie podaj przewidywany rozmiar, żebyśmy pilnowali budżetu 341 kB.

**d) Krótkie uzasadnienie decyzji**
Kilka akapitów: dlaczego taki podział informacji, dlaczego taka hierarchia, co świadomie odrzuciłeś. Interesuje mnie sposób myślenia, bo przy wdrożeniu pojawią się przypadki, których nie przewidzieliśmy, i będę musiał rozstrzygać je w twoim duchu.

### Faza 2 — panel administracyjny WWW (po zaakceptowaniu fazy 1)

Urządzenie ma panel konfiguracyjny w przeglądarce, dostępny w sieci domowej. Służy do ustawień (sieć, lokalizacja, czujniki, integracje), podglądu stanu, zrzutu ekranu i przełączania widoków.

Ten panel projektujesz **po** interfejsie wyświetlacza i **w jego języku wizualnym** — ma być rozpoznawalnie tą samą rodziną. Używany jest z telefonu i z komputera, przez jedną osobę (mnie), rzadko. Szczegółowy zakres uzgodnimy po fazie 1; nie zaczynaj od niego.

---

## 9. Jak będę oceniał

W kolejności ważności:

1. **Czytelność z 2 metrów w 3 sekundy.** Test opisany w punkcie 2.
2. **Uczciwość wobec danych.** Czy widać różnicę między „wiem, że zero" a „nie wiem"? Czy widać, że dane są stare?
3. **Zachowanie przy niekompletnych danych.** Czy styczniowy wieczór wygląda na przemyślany, czy na zepsuty?
4. **Mieszczenie się w budżecie zasobów.** Projekt przekraczający flash jest projektem niewdrożonym.
5. **Oryginalność podejścia.** Po to jest cały ten brief. Zaskocz mnie.

---

## 10. Co dostaniesz ode mnie

- **`KATALOG-DANYCH.md`** — pełna specyfikacja ~290 pól danych.
- Odpowiedzi na pytania o dane, sprzęt i możliwości — pytaj śmiało i szczegółowo.
- Dostęp do konsultacji technicznej: jeśli wymyślisz coś, co wydaje się niewykonalne, zapytaj, zanim odrzucisz. Sporo rzeczy jest wykonalnych taniej, niż się wydaje (i odwrotnie).

**Czego nie dostaniesz:** zrzutów obecnego interfejsu, opisu obecnych ekranów, obecnej palety ani obecnych ikon. Powody w punkcie 0.

---

## 11. Podsumowanie w trzech zdaniach

Zaprojektuj od zera interfejs małego wyświetlacza 320 × 240, który wisi w łazience i przez kilka sekund dziennie ma komuś powiedzieć coś prawdziwego o pogodzie, domu i prądzie.

Masz do dyspozycji około 290 pól danych, 341 kB na zasoby, 20 klatek na sekundę, dotyk, czujnik obecności i czujnik światła.

Najważniejsze: ma być czytelne z dwóch metrów i ma nie kłamać, gdy dane są stare albo ich nie ma.
