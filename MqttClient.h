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

// Utrzymuje polaczenie (albo je zrywa, gdy MQTT wylaczono), obsluguje keepalive
// i cyklicznie publikuje telemetrie urzadzenia. Wolac z netTaska w kazdej petli.
void loop();

// Wywolywac po UDANYM odczycie (ok == true) i po nieudanym (ok == false).
// Przy bledzie moce ida na 0, a liczniki energii trzymaja ostatnia znana wartosc —
// inaczej HA zobaczylby zjazd total_increasing do zera i policzyl falszywy reset.
void publishPv(const PvModel& pv, bool ok);

// Tylko po udanym pobraniu prognozy.
void publishBle();
void publishWeather(const WeatherModel& w);

// Panel WWW / konsola zmienily konfiguracje — zerwij polaczenie i zestaw od nowa
// (discovery poleci ponownie, bo prefix albo broker mogly sie zmienic).
void configChanged();

}  // namespace mqttha
