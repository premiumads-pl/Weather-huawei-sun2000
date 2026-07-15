#pragma once

#include <cstdint>

#include "Settings.h"  // Settings::BLE_GW - jedno zrodlo prawdy o liczbie slotow

// Bramki BLE: pozostale pary uszu.
//
// PO CO: Bluetooth nie ma sieci kratowej - kazdy czujnik musi dosiegnac jednego,
// konkretnego odbiornika. Zmierzone u uzytkownika (wlasne radio / bramka, dBm):
//
//   Lazienka  -72 / -70      Schody -90 / -56      Salon -98 / -79
//   Sypialnia -84 / -94      Biuro (parter) -98 / -98
//
// Te liczby mowia jedno: odbiorniki sa KOMPLEMENTARNE, nie redundantne. Kazdy
// czujnik ma dokladnie jednego opiekuna, a Biuro na parterze nie ma zadnego - jest
// dwa stropy od wszystkiego. Zadne przekladanie plytki tego nie naprawi (wyswietlacz
// wisi w lazience za lustrem z metalowa powloka i slyszy praktycznie tylko swoj
// pokoj), a jedna bramka na pietrze nie siega parteru. Stad LISTA bramek.
//
// PODZIAL PRACY: bramka przekazuje SUROWA ramke - szyfrogram MiBeacon (fe95) albo
// jawna ramke Qingping (fdcd). Bindkeys zostaja w NVS wyswietlacza. Gdyby ktos
// zajrzal w skrypt bramki, nie dowie sie nic. Ta zasada nie ma wyjatkow.
//
// Wyswietlacz NIE przestaje sluchac sam - bierze to, co przyjdzie. Padnie bramka,
// blizsze czujniki dzialaja dalej.
//
// WYBOR ZRODLA (bo "ostatni zapis wygrywa" klamal): ta sama ramka potrafi przyjsc
// z kilku bramek naraz, a temperatura w niej jest za kazdym razem IDENTYCZNA -
// rozni sie tylko RSSI. Kto pisze ostatni, ten ustawia rssiGw, wiec bez arbitrazu
// czujnik na Schodach pokazywalby raz -56, raz -98, zaleznie od kolejnosci
// odpytania. Dlatego kazdy czujnik ma tu OPIEKUNA: bramke, ktora slyszy go
// najlepiej sposrod zywych, z histereza 6 dB (Lazienka: -70 vs -72 - bez histerezy
// opiekun zmienialby sie co ramke). Do ble:: trafia wylacznie ramka od opiekuna,
// wiec Sensor::rssiGw znaczy "tyle ma czujnik u swojej bramki", a nie "tyle mial
// u tej, ktora odezwala sie ostatnia".
//
// Pomiar pozostalych bramek NIE jest przez to tracony - siedzi w tablicy tutaj i
// wychodzi przez rssiOf()/ownerOf(). To wazne: gdyby slabsze bramki przestaly
// mierzyc, panel porownywalby bramke sama ze soba i nie dalo by sie zobaczyc, ze
// nowa bramka na parterze slyszy Biuro lepiej niz stara.

namespace blegw {

// Liczba slotow rzadzi Settings - tam leza hosty i tam idzie zapis do NVS.
constexpr int SLOTS = Settings::BLE_GW;

// Wolane z netTask. Odpytuje wszystkie obsadzone sloty, ktore sa "na chodzie":
// bramka, ktora nie odpowiada, trafia na backoff i nie kradnie czasu pozostalym.
void poll();

// --- diagnostyka per bramka (0..SLOTS-1) ---
const char* host(int i);       // "" = slot pusty
uint32_t lastOkAt(int i);      // millis() ostatniej udanej odpowiedzi, 0 = nigdy
int lastCount(int i);          // ramek w ostatniej odpowiedzi (takze tych, ktore
                               // oddalismy innej bramce - to miara ZYCIA bramki,
                               // nie tego, ile z niej finalnie weszlo)
const char* lastError(int i);  // "" = OK, "wylaczona" = slot pusty

// --- zbiorczo, dla ekranu statystyk (jeden wiersz "Bramka" na dwie kolumny) ---
int configured();              // ile slotow ma adres
int online();                  // ile odpowiada
uint32_t lastOkAt();           // najswiezsze OK z calej listy
int lastCount();               // suma ramek
const char* lastError();       // blad PIERWSZEJ padnietej bramki, "" gdy wszystkie
                               // zyja. Przy jednej bramce to doslownie to samo, co
                               // zwracala wersja sprzed listy.

// --- ktora bramka opiekuje sie czujnikiem ---
// Odpowiedz na pytanie, ktorego Sensor::rssiGw nie potrafi zadac przy wielu
// bramkach. Wolane takze z webTask (panel), wiec chronione mutexem.
int ownerOf(const char* mac);           // indeks bramki-opiekuna, -1 = zadna
int rssiOf(const char* mac, int i);     // co slyszy bramka i, 0 = nie slyszy
uint32_t heardAt(const char* mac, int i);  // millis() ostatniej ramki, 0 = nigdy

// Panel zapisal nowa liste: kasuje backoff i pomiary (adresy mogly sie przesunac,
// wiec stary pomiar moze dotyczyc juz innego urzadzenia).
void retryNow();

}  // namespace blegw
