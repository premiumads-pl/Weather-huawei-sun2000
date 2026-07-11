# Backlog

Urządzenie jest odłączone od komputera na stałe. Wszystko — diagnostyka, aktualizacje,
konfiguracja — musi iść przez sieć.

---

## 1. Zdalna diagnostyka (PRIORYTET — blokuje resztę)

**Problem:** cały `Serial.printf(...)` w firmwarze trafia teraz w próżnię. Bez USB nie da się
sprawdzić, dlaczego coś nie działa (falownik, radar, OTA, loty). Każdy kolejny błąd byłby
zgadywanką.

**Do zrobienia — musi wejść w najbliższej aktualizacji:**

- **Bufor kołowy logów w RAM** (np. 8 kB, ~120 ostatnich linii). Podmienić `Serial.printf`
  na `LOG(...)`, które pisze i do Seriala, i do bufora.
- **`GET /api/log`** — zwraca bufor jako tekst. Pozwala zobaczyć to samo, co dotąd szło po USB.
- **`GET /api/diag`** — migawka stanu w JSON:
  - `fw`, uptime, wolny heap, min. wolny heap, temperatura rdzenia
  - WiFi: SSID, RSSI, IP, liczba reconnectów
  - PV: `online`, ostatni błąd, wiek ostatniego udanego odczytu, status falownika
  - Radar: poziom, wiek klatki, ostatni błąd
  - Loty: liczba w zasięgu, ostatni błąd
  - Pogoda: wiek ostatniego pobrania, ostatni błąd
  - OTA: wersja zdalna, ostatni wynik sprawdzenia
- **`POST /api/reboot`** — restart bez kasowania konfiguracji (dziś restartuje tylko
  `/api/forget`, który przy okazji czyści WiFi — to za dużo).
- Podpiąć `/api/log` i `/api/diag` do panelu WWW (zakładka „Diagnostyka").

**Uwaga na bezpieczeństwo:** logi nie mogą zawierać hasła WiFi.

---

## 2. Do poprawienia przez użytkownika w panelu

- **Lokalizacja** — ustawiona na centrum Gdyni (54,5189 / 18,5319), a nie ul. Częstochowską
  (54,4870 / 18,5216). Różnica ~4 km. Radar próbkuje ~3,5 km, więc dla ulewy nad domem
  warto poprawić.
- **Moc instalacji** — w panelu `7000 W`, a deklarowane 8 kWp. Wpływa na wskaźnik
  „OBCIĄŻENIE" na ekranie PV.

---

## 3. Pomysły / do rozważenia

- **Mini-radar na mapie lotów** — nałożyć kafelek radarowy na kontur Zatoki. Dane już są
  pobierane, brakuje tylko renderowania.
- **Trend radaru** — pobierać 2–3 ostatnie klatki i pokazywać, czy opad nadchodzi, czy odchodzi
  („ulewa za ~20 min" / „przechodzi").
- **Historia produkcji PV z wielu dni** — dziś profil kasuje się o północy. Można trzymać
  7 dni w NVS i pokazywać słupki dzień po dniu.
- **Alert radarowy** — przerwanie rotacji, gdy radar wykryje poziom ≥ 4 nad domem.
- **Nocą pomijać ekran PV** — odrzucone przy pierwszej rozmowie, ale w praktyce ekran PV
  w nocy pokazuje same zera.
- **Weryfikacja PSRAM** — wolny heap bywa niski (~17 kB przy otwartym TLS). Jeśli płytka ma
  PSRAM, włączenie go w FQBN przeniosłoby bufor ekranu (150 kB) do PSRAM. Wymaga sprawdzenia,
  czy `ESP.getPsramSize() > 0` — czyli znów: potrzebna zdalna diagnostyka (pkt 1).

---

## 4. Znane ograniczenia

- **RainViewer obsługuje tylko zoom ≤ 7.** Od 8 w górę zwraca kafelek z napisem
  „Zoom Level Not Supported" — antyaliasowany tekst wygląda w danych jak echo opadu.
  Nie podnosić zoomu bez ponownej weryfikacji.
- **Falownik Huawei przyjmuje tylko jedną sesję Modbus TCP.** Równoległy klient (np. skrypt
  na komputerze) rozłączy wyświetlacz.
- **Open-Meteo to model, nie radar** — potrafi kompletnie przegapić lokalną ulewę. Dlatego
  ekran 1 ma priorytet: radar > prognoza.
