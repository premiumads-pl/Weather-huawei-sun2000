# WYŚWIETLACZ ŁAZIENKOWY 320×240 — SPECYFIKACJA WDROŻENIOWA
Faza 1 · projekt "Pasmowy" · 21.07.2026

## Siatka
- Kolumna kontekstu (ciemna) 136 px + kolumna danych (jasna) 184 px; marginesy 10/12 px.
- Moduły rozdzielane linią 1 px (#D9DCD6), nigdy ramkami. Maks. 3 moduły na kolumnę danych.
- Pasek techniczny 22 px doklejany od dołu wyłącznie w sekwencji startowej.

## Typografia — IBM Plex Sans Condensed (licencja OFL, wolno osadzać)
Rozmiary bitmap: 10 (osie), 11 (etykiety, wersaliki +0.06em), 13 (tekst), 20 (wartości), 24 (zegar), 52 (wyróżnik).
Wagi: 500/600/700. Zestaw znaków do zbitmapowania:
ASCII + ąćęłńóśźż ĄĆĘŁŃÓŚŹŻ ° µ ³ − → · ▮ ▯ ✓
Szacunek: ~16 kB łącznie.

## Paleta (hex → użycie)
#F4F4F0 tło jasne · #1A1C1E panel/tekst · #6C6F6A drugorzędny · #9A9C96 wyciszony · #D9DCD6 linie
#2563C4 opad (odcienie: #5B8DD0 #9DB8DD #B8C6DD) · #4D9A4D produkcja/OK · #3D78C4 zużycie własne · #C04A3A z sieci/alarm
#F2B13A słońce · #E0A92E modulacja/PV · #B8901F + tło #F3E4C2 nieświeże/uwaga · #7FA8E0 akcenty na ciemnym
Radar: morze #0E141F · ląd #2B3444 · brzeg #5A6A82 · granice #57647C (kreska 3/2)
Zasady RGB565: płaskie wypełnienia, zero gradientów; przy 18% podświetlenia nie używać kolorów ciemniejszych niż #3A4150 na czarnym.

## Świeżość danych (system trójstanowy)
- świeże: pełna kropka #4D9A4D 7 px
- nieświeże: pusta kropka #B8901F + wiek słownie ("sprzed 5 h"); liczba szarzeje do #8D867A
- nie wiem: myślnik, nigdy zero. PV: 0 W + flaga nieważne = "—", falownik w oknie nocy = "śpi" (neutralne).

## Nawigacja (1 elektroda)
1× (opóźnienie 550 ms): GŁÓWNY→RADAR→5 DNI→PRĄD→POKOJE→OGRZEWANIE→POWIETRZE→SAMOLOTY→GŁÓWNY
2× (natychmiast): DIAGNOSTYKA 1/2 (1× przełącza, 2× wyjście). Timeout 60 s → GŁÓWNY.
Plansza zdarzenia: 6,5 s, ten sam rodzaj maks. co 10 min.

## Zasoby
- Fonty bitmapowe: ~16 kB
- Glify pogody (8 rodzin × warianty), księżyc, wykresy: prymitywy, 0 kB
- Kontur mapy + granice: polilinie ~6 kB (coastline-polyline-320x172.txt)
- RAZEM ~23 kB z ~440 kB dostępnych.

## Ekrany (PNG 1:1 w screens/)
01 główny · 02 noc-ciemno · 03 radar · 04 pięć dni · 05 prąd · 06 styczniowy wieczór · 07 start
08 diagnostyka źródła · 09 jesień pobór+smog · 10 diagnostyka stan · 11 noc przy świetle · 12 samoloty
13 zdarzenie burza · 14 pokoje · 15 ogrzewanie · 16 powietrze · 17 minimalna instalacja
18 zdarzenie mróz · 19 zdarzenie awaria · 20 radar bez PSRAM · 21 pogoda niepobrana
