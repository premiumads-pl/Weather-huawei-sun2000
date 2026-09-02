#pragma once

#include <cstdint>

// Wraparound-safe "czy to jeszcze świeże" dla znaczników czasu millis(). Wyjęte tu
// z WeatherUiV3.cpp (freshMs, v158) i skopiowanych ad-hoc odpowiedników w
// OledPanel.cpp (step()/graphTick()) — jeden wzór zamiast trzech osobnych kopii tej
// samej, nieoczywistej arytmetyki. Powód, dla którego to się opłaca: MqttClient.cpp
// miał WŁASNY, INNY wzór na "czy już czas" (harmonogram na ABSOLUTNYM "kolejnym
// terminie" zamiast na UPŁYWIE czasu od ostatniej próby) i ten wzór był zwyczajnie
// zły — blokował reconnect na dobę 24,85-49,7 pracy urządzenia (patrz P1-1, poprawka
// w MqttClient.cpp). Konsolidacja tej arytmetyki w jednym miejscu zmniejsza szansę,
// że taka sama pomyłka powtórzy się gdzie indziej.
//
// (P1-1, poprawka po przeglądzie) ORYGINALNY komentarz z WeatherUiV3.cpp (freshMs,
// v158) uzasadniał rzutowanie na int32 tak: "Ujemna roznica (chwila po przekrece
// albo znacznik z przyszlosci przy wyscigu odczytu) ma znaczyc 'swieze', a nie
// 'starsze niz wszechswiat'". To lączyło DWA różne przypadki w jeden koszyk:
//   (a) `now` chwilę po przekręceniu millis(), `okAt` sprzed przekręcenia — zwykłe
//       odejmowanie bez znaku liczy to PRAWIDŁOWO samo z siebie, bez żadnego
//       rzutowania (np. now=5, okAt=UINT32_MAX-100 -> age=106, poprawne);
//   (b) `okAt` stemplowany przez INNY RDZEŃ ułamek milisekundy PO `now` (wyścig
//       odczytu) — wtedy `now - okAt` zawija się do wartości BLISKO UINT32_MAX,
//       co jest jedynym przypadkiem, który realnie wymaga specjalnego traktowania.
// Rzutowanie na int32 (próg dokładnie 2^31 ms = ~24,85 dnia) łapało (b), ale PRZY
// OKAZJI łapało też każdy wiek POWYŻEJ ~24,85 dnia jako "świeży" — czyli dane
// sprzed 30 dni wychodziły jako aktualne. Sprawdzone liczbowo: dla `now`=40 dni,
// `okAt`=10 dni (wiek=30 dni), stary wzór dawał `isFresh()==true` dla dowolnego
// sensownego `staleMs`. To NIE był zamierzony fail-safe, tylko efekt uboczny progu
// dobranego "z grubsza" (2^31) zamiast do rzeczywistej skali wyścigu (rząd
// pojedynczych rdzeni/pętli, nie dni).
//
// POPRAWKA: zamiast łapać WSZYSTKO powyżej 2^31 ms, łapiemy WYŁĄCZNIE wąskie okno
// tuż PRZED zawinięciem (`age` bliskie UINT32_MAX) — czyli dokładnie przypadek (b).
// 2000 ms to szeroki, bezpieczny zapas ponad realny wyścig (pojedyncze pętle/ms
// między rdzeniami), a jednocześnie znikomy wobec jakiegokolwiek sensownego
// `staleMs` w tym projekcie (dziesiątki sekund do dziesiątek minut) — nie zmienia
// więc żadnej normalnej granicy świeżości, tylko usuwa efekt uboczny.
//
// okAt == 0 znaczy "nigdy" (konwencja tego projektu, patrz np. gGraphHighMs w
// OledPanel.cpp: "0 znaczy nigdy") i zawsze daje false, niezależnie od `now`.
//
// `now` jest parametrem, nie millis() wołanym w środku: OledPanel.cpp łapie jedno
// `now` na cały obieg step() i liczy względem NIEGO, żeby kilka sprawdzeń świeżości
// w tym samym obiegu widziało dokładnie tę samą chwilę.
// Wspolna dla isFresh() i kazdego innego miejsca liczacego wiek stempla millis()
// (np. okAgeS() w WeatherUiV3.cpp) — jedna liczba, nie kopia w kazdym pliku.
constexpr uint32_t kFreshFutureSkewToleranceMs = 2000;

inline bool isFresh(uint32_t now, uint32_t okAt, uint32_t staleMs) {
  if (okAt == 0) return false;
  const uint32_t age = now - okAt;
  return age < staleMs || age > (0xFFFFFFFFu - kFreshFutureSkewToleranceMs);
}
