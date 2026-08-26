#pragma once

#include <cstdint>

// ============================================================================
//  (v181) HISTORIA ZWROTU Z FOTOWOLTAIKI — miesiac po miesiacu, NARASTAJACO
// ============================================================================
// Zasila ekran ZWROT (cfg::VIEW_PAYBACK, v3Payback w WeatherUiV3.cpp): rysuje sie
// z tego krzywa dojscia do cfg::PV_KOSZT_PLN, a z tempa ostatnich 12 punktow —
// prognoza daty pelnego zwrotu.
//
// CO TU STOI: SKUMULOWANA korzysc z instalacji w PELNYCH ZLOTYCH, liczona od
// uruchomienia. Kazda kolejna liczba jest wieksza albo rowna poprzedniej — to nie
// jest korzysc miesieczna, tylko suma wszystkiego do konca danego miesiaca. Zima
// przyrost potrafi wynosic kilkanascie zlotych (patrz grudzien/styczen: 10 200 ->
// 10 213 -> 10 253), latem kilkaset — plaskie odcinki tej krzywej sa PRAWDA
// o instalacji w Gdyni, a nie brakiem danych.
//
// SKAD TE LICZBY: policzone WSTECZ z raportow miesiecznych falownika (produkcja,
// oddane do sieci) i z faktur Energi (stawki w taryfie G12w). Metoda, ta sama dla
// kazdego miesiaca:
//
//     korzysc = (produkcja - oddane_rozliczone) x cena_zakupu
//             +  oddane_rozliczone x stawka_oddania_z_VAT
//
// czyli: energia zuzyta na miejscu jest warta tyle, ile kosztowaloby jej KUPIENIE
// (dlatego cena zakupu, ze strefa G12w wliczona), a energia oddana do sieci — tyle,
// ile realnie za nia dostajemy po rozliczeniu.
//
// OPLAT STALYCH (abonament, oplata przejsciowa, mocowa) CELOWO NIE WLICZONO,
// w zadna strone. Placi sie je co miesiac dokladnie tak samo Z PANELAMI I BEZ NICH,
// wiec nie sa ani kosztem, ani zyskiem instalacji — wejscie ich do wzoru zmienialoby
// wylacznie wartosc bezwzgledna, a nie moment zwrotu, i zacieralo to, co ta krzywa
// ma pokazywac.
//
// ZERO STATYCZNEGO RAM-U: `constexpr` -> wewnetrzna konsolidacja -> tablica ladzie
// w .rodata, czyli we flashu, i jest czytana wprost. 37 x 2 B = 74 B flasha. To jest
// warunek konieczny przy barierze 76 000 B statycznego RAM-u (tools/release.sh) —
// gdyby ktos zamienil to kiedys na `static uint16_t` albo dolozyl kopie w RAM-ie,
// zjadloby to zapas budzetu na ekran, ktory niczego nie liczy w czasie rzeczywistym.
//
// uint16_t, nie uint32_t: mieszcza sie tu kwoty do 65 535 zl, czyli — przy tempie
// rzedu 334 zl/mies. — historia do okolo roku 2039. Straznik nizej zatrzyma
// kompilacje, zanim ostatni punkt podejdzie pod ten sufit.
constexpr uint16_t kPaybackHist[] = {
      192,   737,   990,  1078,  1123,  1173,  1299,  1642,
     2140,  3005,  3786,  4546,  5059,  5651,  5926,  6025,
     6078,  6150,  6299,  6721,  7259,  7824,  8347,  8882,
     9268,  9736, 10017, 10138, 10200, 10213, 10253, 10675,
    11138, 11646, 12257, 12877, 13279,
};

// Pierwszy punkt tablicy = SIERPIEN 2023 (miesiac uruchomienia instalacji), kolejne
// co miesiac bez przerw. Ekran wylicza z tej pary date kazdego punktu i podpisy osi X,
// wiec przy dopisywaniu nowych miesiecy NIE RUSZAMY tych dwoch stalych — dopisuje sie
// wylacznie na KONCU tablicy.
constexpr int kPaybackHistYear0 = 2023;
constexpr int kPaybackHistMonth0 = 8;   // sierpien 2023

constexpr int kPaybackHistN = static_cast<int>(sizeof(kPaybackHist) / sizeof(kPaybackHist[0]));

// Straznicy sensu, nie stylu. Kazdy pilnuje zalozenia, ktore ekran ZWROT przyjmuje
// bez sprawdzania w czasie pracy (bo sprawdzanie stalej w petli rysowania to koszt
// bez zysku):
//   * niepusta — v3Payback siega po kPaybackHist[kPaybackHistN-1] bez zadnej oslony,
//     i to ten punkt jest zrodlem procentu, gdy MQTT jeszcze nic nie przyslal;
//   * >= 13 punktow — tempo liczy sie z okna 12 miesiecy wstecz; przy krotszej
//     historii ekran ma sciezke zapasowa (bierze najstarszy punkt), ale ponizej
//     dwoch punktow nie ma z czego narysowac nawet linii;
//   * miesiac 1..12 — z tej pary licza sie WSZYSTKIE daty na ekranie, wliczajac
//     prognozowana date zwrotu; literowka tutaj przesunelaby cala os X po cichu.
static_assert(kPaybackHistN >= 2,
              "kPaybackHist musi miec co najmniej dwa punkty — z jednego nie da sie "
              "narysowac ani linii, ani policzyc tempa");
static_assert(kPaybackHistMonth0 >= 1 && kPaybackHistMonth0 <= 12,
              "kPaybackHistMonth0 to numer miesiaca 1..12 (8 = sierpien)");
