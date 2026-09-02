#pragma once

#include <cstdint>

// Wraparound-safe "czy to jeszcze świeże" dla znaczników czasu millis(). Wyjęte tu
// z WeatherUiV3.cpp (freshMs, v158) i skopiowanych ad-hoc odpowiedników w
// OledPanel.cpp (step()/graphTick()) — jeden wzór zamiast trzech osobnych kopii tej
// samej, nieoczywistej arytmetyki. Powód, dla którego to się opłaca: MqttClient.cpp
// miał WŁASNY, INNY wzór na "czy już czas" (harmonogram na ABSOLUTNYM "kolejnym
// terminie" zamiast na UPŁYWIE czasu od ostatniej próby) i ten wzór był zwyczajnie
// zły — blokował reconnect na dobę 24,85-49,7 pracy urządzenia (patrz P1-1, poprawka
// w MqttClient.cpp). Konsolidacja tej — poprawnej — arytmetyki w jednym miejscu
// zmniejsza szansę, że taka sama pomyłka powtórzy się gdzie indziej.
//
// [oryginalny komentarz z WeatherUiV3.cpp, zachowany w całości]
// Wiek liczymy ZE ZNAKIEM na int32: znaczniki to millis() pisany przez netTask,
// a uint32 przekreca sie po ~49 dniach pracy. Ujemna roznica (chwila po przekrece
// albo znacznik z przyszlosci przy wyscigu odczytu) ma znaczyc "swieze", a nie
// "starsze niz wszechswiat" — ten sam idiom, co ago() w Portal.cpp.
//
// okAt == 0 znaczy "nigdy" (konwencja tego projektu, patrz np. gGraphHighMs w
// OledPanel.cpp: "0 znaczy nigdy") i zawsze daje false, niezależnie od `now`.
//
// `now` jest parametrem, nie millis() wołanym w środku: OledPanel.cpp łapie jedno
// `now` na cały obieg step() i liczy względem NIEGO, żeby kilka sprawdzeń świeżości
// w tym samym obiegu widziało dokładnie tę samą chwilę.
inline bool isFresh(uint32_t now, uint32_t okAt, uint32_t staleMs) {
  if (okAt == 0) return false;
  return static_cast<int32_t>(now - okAt) < static_cast<int32_t>(staleMs);
}
