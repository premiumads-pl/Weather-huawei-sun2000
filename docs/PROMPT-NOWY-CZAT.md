# Prompt otwarcia nowego czatu

Skopiuj poniższy tekst (wszystko między liniami) jako pierwszą wiadomość w nowym czacie.

---

Pracujemy lokalnie na plikach w katalogu projektu `pogoda-gdynia` (ESP32-S3 + ST7789,
wyświetlacz pogodowo-domowy w łazience, motyw V3 „Pasmowy"). Rozmawiamy po polsku.

**Zacznij od przeglądu — nie pisz jeszcze kodu:**

1. Przeczytaj `docs/SESJA-2026-07-25.md` — pełne przekazanie stanu (v144 → v146:
   weryfikacja v144, zadanie #19 etapy 0 i 1, decyzje do zakwestionowania, co zostało).
   Poprzednie przekazanie, `docs/SESJA-2026-07-23.md`, dalej jest aktualne w części
   o pułapkach (font zegara, tryb nocny, Viessmann, AirHistory).
2. Przejrzyj `ARCHITEKTURA.md` i `docs/KATALOG-DANYCH.md` oraz kod, którego dotyczy
   zadanie, które weźmiemy.
3. Sprawdź stan repo (`git log`, `git status`, `Version.h`) i stan urządzenia przez
   panel (`/api/state`, w razie potrzeby `/api/diag`).
4. **Jeśli urządzenie nie odpowiada — najpierw zapytaj mnie o adres.** Mac potrafi
   siedzieć w innej podsieci niż wyświetlacz, a adres z historii poleceń bywa
   nieaktualny. To już dwa razy wyglądało jak awaria firmware'u, a nią nie było.

**Potem powiedz mi krótko, czy wszystko się zgadza.** Rzeczy do wzięcia, w kolejności:

- **#19 etap 2 (opcjonalny):** fallback na mniejszą liczbę klatek radaru, gdy pełny
  komplet 13 nie wchodzi do PSRAM. Uwaga: `FRAMES` jest `constexpr` i siedzi w `fetch()`,
  `setDemo()`, animacji i osi czasu w `WeatherUi` — zamiana na limit runtime'owy
  rozlewa się poza `RadarMap.cpp`. Osobne wydanie, nie doklejane do niczego.
- **Loty:** `flights.ok_ago_s` rośnie przy `total: 0` — nie pobierają się. Do zbadania.
- Drobne z poprzednich sesji: sesje w łazience (sklejanie impulsów PIR w „wizyty"),
  log zdarzeń, „noc przy świetle" (makieta 11).

**Zasady, które obowiązują zawsze:**
- Repo jest **publiczne** — zero kluczy, haseł, tokenów, adresów IP, SSID w kodzie
  i dokumentach. Sekrety żyją wyłącznie w NVS urządzenia i w panelu WWW.
- **Nigdy nie wklejaj mi tokenów, haseł ani kluczy do czatu.**
- Urządzenie jest **na stałe odłączone od komputera — tylko OTA, bez USB**. Firmware
  psujący rozruch albo sieć jest nie do odratowania bez rozkręcania.
- Twarde bariery przed publikacją (`tools/release.sh`): **statyczny RAM < 76000 B**
  (teraz 73856) oraz adresy RTC `gPir @ 0x50000200`, `gLdr @ 0x500002c0` bez zmian.
- Do samego sprawdzenia buduj do `build_verify/`; `tools/release.sh` **od razu publikuje**.
- Przed wydaniem warto **zrestartować urządzenie**: po dobie–dwóch największy ciągły
  blok sterty schodzi poniżej 40 kB, a tyle chce TLS przy OTA.
- Po wydaniu `POST /api/update` zleca sprawdzenie od razu — bez czekania 15 minut.
- Tryb pracy: kod piszą osobni agenci z briefem → Ty recenzujesz → build i bramy →
  publikacja → weryfikacja na żywo (zrzut ekranu). Komentarze w kodzie po polsku,
  wyjaśniające *dlaczego*.
- Pułapka fontowa: `plex::f24()` to font zegara (14 glifów, bez przecinka i liter) —
  do liczb z przecinkiem i tekstu używaj f20/f13/f11/f10/f52.
- **Nie łam zasady z `RadarMap.cpp`:** opublikowany bufor PSRAM nie jest zwalniany
  nigdy — na tym stoi prawo rysowania do czytania `raster()` bez mutexa.

---
