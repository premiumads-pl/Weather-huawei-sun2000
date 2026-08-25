#pragma once

// Publikacja danych na brokera MQTT z autodiscovery Home Assistanta.
//
// Zasady, ktore trzymaja to w ryzach na plytce bez PSRAM:
//   * caly klient (WiFiClient + PubSubClient + bufor 512 B) siedzi na stercie
//     i powstaje DOPIERO, gdy MQTT jest wlaczony w ustawieniach — wylaczony
//     MQTT kosztuje kilkadziesiat bajtow RAM-u statycznego i zero sterty,
//   * bufor musi pomiescic nasz najwiekszy pakiet WYCHODZACY (retained config
//     encji, 430 B razem z naglowkiem) — i on wyznacza rozmiar. (v174) Doszedl
//     JEDEN temat przychodzacy (<prefix>/auto/stan, ~190 B razem z naglowkiem),
//     wiec bufora NIE trzeba bylo ruszac; rachunek stoi przy kBufSize nizej,
//   * wszystko chodzi z netTaska; loop() (rysowanie) nie dotyka MQTT.
//
// Brak brokera nie moze wywrocic urzadzenia: proby laczenia maja krotkie
// timeouty i wykladniczy backoff (5 s -> 5 min), a bledy ladują w diag().

#include "AutoData.h"
#include "PvData.h"
#include "WeatherData.h"

namespace mqttha {

// (v174) Kopiuje ostatni odebrany stan auta (<prefix>/auto/stan) do `out`.
//
// KOPIA, NIE WSKAZNIK — i to nie jest ostroznosc na wyrost. Strukture pisze CALLBACK
// PubSubClienta, czyli netTask (rdzen 0), a czyta petla rysujaca (rdzen 1) i zrzut
// ekranu z webTaska. AutoModel ma 44 B, wiec odczyt przez wskaznik nie jest atomowy:
// bez blokady da sie zlapac nowe `soc` przy starym `kw`. Kopiowanie idzie pod
// wlasnym, malym mutexem tego modulu — NIE pod gLock z pogoda-gdynia.ino, bo callback
// nie ma prawa czekac na mutex trzymany przez rysowanie (ten sam zakaz, co przy
// BleGateway i Viessmannie: kazdy modul pilnuje swoich danych sam).
//
// Zwraca false, gdy MQTT jest wylaczony albo nie przyszla jeszcze ani jedna
// wiadomosc — wtedy `out` zostaje NIETKNIETE.
bool autoSnapshot(AutoModel& out);

// (v175) WYSYLKA TRYBU LADOWANIA — <prefix>/auto/tryb/set, ladunek to DOKLADNIE
// jedna z wartosci "OFF" / "PV" / "PV+MIN" / "MAX" (patrz autoModeMqtt w AutoData.h).
//
// DLACZEGO NIE `gCli->publish()` WPROST Z PANELU: PubSubClient nie jest bezpieczny
// watkowo, a JEDYNYM zadaniem, ktore go dotyka, jest netTask (rdzen 0) — to on wola
// gCli->loop() i w srodku tego wywolania biegnie callback odbioru. Panel OLED zyje
// w petli rysowania (rdzen 1). Publikacja stamtad weszlaby w to samo gniazdo i ten
// sam bufor 512 B, ktory netTask akurat czyta. Dlatego panel tylko SKLADA ZAMOWIENIE,
// a wysyla je mqttha::loop() u siebie — ta sama sciezka co przy pozostalych danych.
//
// requestAutoMode() nadpisuje zamowienie, ktore jeszcze nie poszlo: liczy sie
// OSTATNI wybor wlasciciela, a nie kolejka jego wahania.
void requestAutoMode(const char* mode);

// Stan tego zamowienia — panel na tym opiera komunikat, a NIE na wlasnym zalozeniu,
// ze skoro wyslal, to zadzialalo:
//   0 = nic nie zamawiano (albo panel juz odczytal koniec sprawy),
//   1 = czeka na wyslanie (netTask jeszcze nie doszedl albo nie ma polaczenia),
//   2 = pakiet POSZEDL do brokera (to NIE jest potwierdzenie zmiany trybu!),
//   3 = nie udalo sie wyslac (MQTT wylaczony, brak polaczenia, broker odrzucil).
// Potwierdzeniem ZMIANY jest wylacznie pole `tryb` wracajace w auto/stan.
uint8_t autoModeReqState();

// Utrzymuje polaczenie (albo je zrywa, gdy MQTT wylaczono), obsluguje keepalive
// i cyklicznie publikuje telemetrie urzadzenia. Wolac z netTaska w kazdej petli.
void loop();

// Wywolywac po UDANYM odczycie (ok == true) i po nieudanym (ok == false).
// Przy bledzie moce ida na 0, a liczniki energii trzymaja ostatnia znana wartosc —
// inaczej HA zobaczylby zjazd total_increasing do zera i policzyl falszywy reset.
// (v179) Liczniki miernika (pola `gin`/`gout`) traktowane sa jeszcze ostrozniej:
// przy wartowniku -1 z PvData.h ZNIKAJA z ladunku zamiast isc jako 0 — pelne
// uzasadnienie przy publishPv() w MqttClient.cpp.
void publishPv(const PvModel& pv, bool ok);

// Tylko po udanym pobraniu prognozy.
void publishBle();
void publishWeather(const WeatherModel& w);

// Panel WWW / konsola zmienily konfiguracje — zerwij polaczenie i zestaw od nowa
// (discovery poleci ponownie, bo prefix albo broker mogly sie zmienic).
void configChanged();

}  // namespace mqttha
