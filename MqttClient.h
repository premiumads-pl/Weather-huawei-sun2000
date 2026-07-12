#pragma once

// Publikacja danych na brokera MQTT z autodiscovery Home Assistanta.
//
// Zasady, ktore trzymaja to w ryzach na plytce bez PSRAM:
//   * caly klient (WiFiClient + PubSubClient + bufor 512 B) siedzi na stercie
//     i powstaje DOPIERO, gdy MQTT jest wlaczony w ustawieniach — wylaczony
//     MQTT kosztuje kilkadziesiat bajtow RAM-u statycznego i zero sterty,
//   * nic nie subskrybujemy, wiec bufor musi pomiescic tylko nasz najwiekszy
//     wychodzacy pakiet (retained config encji, 430 B razem z naglowkiem),
//   * wszystko chodzi z netTaska; loop() (rysowanie) nie dotyka MQTT.
//
// Brak brokera nie moze wywrocic urzadzenia: proby laczenia maja krotkie
// timeouty i wykladniczy backoff (5 s -> 5 min), a bledy ladują w diag().

#include "PvData.h"
#include "WeatherData.h"

namespace mqttha {

// Utrzymuje polaczenie (albo je zrywa, gdy MQTT wylaczono), obsluguje keepalive
// i cyklicznie publikuje telemetrie urzadzenia. Wolac z netTaska w kazdej petli.
void loop();

// Wywolywac po UDANYM odczycie (ok == true) i po nieudanym (ok == false).
// Przy bledzie moce ida na 0, a liczniki energii trzymaja ostatnia znana wartosc —
// inaczej HA zobaczylby zjazd total_increasing do zera i policzyl falszywy reset.
void publishPv(const PvModel& pv, bool ok);

// Tylko po udanym pobraniu prognozy.
void publishWeather(const WeatherModel& w);

// Panel WWW / konsola zmienily konfiguracje — zerwij polaczenie i zestaw od nowa
// (discovery poleci ponownie, bo prefix albo broker mogly sie zmienic).
void configChanged();

}  // namespace mqttha
