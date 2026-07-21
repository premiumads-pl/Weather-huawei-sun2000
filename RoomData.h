#pragma once

#include <cstdint>

#include "BleSensors.h"  // ble::MAX_SENSORS - jedno zrodlo prawdy o liczbie wierszy

// Model posredni ekranu W DOMU (v126). Po co osobna struktura, skoro dane siedza
// juz w ble::?
//
// Bo do v125 funkcja RYSUJACA (drawViewHome/drawViewHomeV2) sama podejmowala
// decyzje o DANYCH: czytala singleton ble:: na zywo, szukala nazwy pokoju przez
// settings().bleFind(), wolala millis() (mimo ze nowMs stal obok w sygnaturze
// paintFrame) i - najgorsze - sama arbitrazowala RSSI miedzy wlasnym radiem a
// bramka, z progiem swiezosci 90 s zaszytym w srodku petli rysujacej kafelki.
// To nie sa decyzje o pikselach. Skutek byl taki, ze wymiana wygladu (V1 -> V2)
// wymagala PRZEPISANIA tej logiki po raz drugi - i faktycznie zostala przepisana
// niedokladnie: V2 pokazuje czujniki bez wpisu w ustawieniach (podpisane MAC-iem),
// V1 je pomija. Dwie kopie tej samej reguly rozjechaly sie po cichu.
//
// Teraz warstwa danych (pogoda-gdynia.ino, buildRoomModel) wypelnia GOTOWE wiersze
// raz na klatke, a obie warstwy rysowania tylko je czytaja. Model jest CELOWO
// nadmiarowy wzgledem obu wygladow: niesie wszystko, czego potrzebuje ktorykolwiek
// z nich (V1 filtruje po `slot`, V2 bierze wszystko), zeby nie trzeba go bylo
// zmieniac przy kolejnej wymianie interfejsu.
//
// Rozmiar: 8 wierszy x 24 B = 192 B statycznego RAM-u. Bariera calego programu to
// 76 000 B (tools/release.sh), wiec to jest cena, ktora widac w budzecie - dlatego
// wiersz trzyma WSKAZNIK do nazwy, a nie jej kopie (24 B na wiersz wiecej), i
// dlatego rastry/historia zostaja tam, gdzie byly.

struct RoomRow {
  // Nazwa pokoju JUZ ROZWIAZANA przez warstwe danych (settings().bleFind ->
  // BleCfg::name; przy braku wpisu albo pustej nazwie - MAC czujnika).
  //
  // Wskaznik, nie bufor - i oba warianty celuja w pamiec, ktora zyje dluzej niz
  // klatka: albo w Settings (singleton w RAM, przezywa restart przez NVS), albo
  // w tablice gSensors[] w BleSensors.cpp (slot dostaje MAC raz, przy zalozeniu,
  // i nigdy go nie zmienia - patrz ble::macOf). Kopiowanie nazw kosztowaloby
  // 8 x 24 B statycznego RAM-u i nie kupowaloby nic.
  const char* name = nullptr;

  float tempC = 0.f;
  float humidity = 0.f;

  // Wiek probki w sekundach, policzony RAZ, z tego samego nowMs, co reszta klatki.
  // 9999 = czujnik nie odezwal sie ani razu (s.seenAt == 0). Rysowanie porownuje to
  // z progiem "brak lacznosci" (900 s) i nie ma prawa pytac zegara samo.
  uint32_t ageS = 0;

  // JUZ WYBRANY lepszy sygnal z dwoch zrodel (wlasne radio / bramka), 0 = zadne
  // zrodlo nie jest swieze. Arbitraz siedzi w buildRoomModel, nie tutaj i nie
  // w rysowaniu.
  int16_t rssi = 0;

  int8_t batteryPct = 0;   // 0..100 (przyciete), 0 = nieznana

  // Slot w Settings::ble[] = indeks koloru kafelka i wiersza w RoomHistory.
  // -1 = czujnik bez wpisu w ustawieniach albo wpisany poza RoomHistory::ROOMS:
  // nie ma dla niego ani koloru, ani historii. V1 takie wiersze POMIJA (tak jak
  // dotad), V2 rysuje je z MAC-iem zamiast nazwy (tak jak dotad).
  int8_t slot = -1;

  bool hasTemp = false;
  bool hasHum = false;
  bool viaGw = false;   // true = lepszy sygnal ma bramka (litera S), false = ESP (E)
  bool valid = false;
};

struct RoomModel {
  // Tyle, ile czujnikow potrafi w ogole zapamietac warstwa nasluchu. NIE tyle, ile
  // miesci ekran (6): model ma byc wierny danym, a przyciecie do szesciu kafelkow
  // jest decyzja UKLADU i zostaje w rysowaniu. Gdyby model mial 6 wierszy, dwa
  // pierwsze czujniki bez wpisu w ustawieniach wypchnelyby z niego pokoje, ktore
  // V1 chce narysowac.
  static constexpr int ROWS = ble::MAX_SENSORS;

  RoomRow rows[ROWS];

  // Ile wierszy jest wypelnionych, czyli ile czujnikow ma JAKIKOLWIEK odczyt.
  uint8_t count = 0;

  // Ile czujnikow widzi warstwa nasluchu W OGOLE, razem z tymi bez odczytu.
  // To NIE to samo co `count` i ta roznica jest widoczna na ekranie: naglowek V2
  // ("* N CZUJNIKOW") i podzial siatki licza sie z tej liczby, a karty rysuja sie
  // z `count`. Tak bylo przed refaktorem (ble::count() kontra petla pomijajaca
  // !s.valid) i tak zostaje - to jest odtworzenie zachowania, nie jego poprawa.
  uint8_t sensorCount = 0;
};
