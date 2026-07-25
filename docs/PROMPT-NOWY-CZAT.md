# Prompt otwarcia nowego czatu

Skopiuj poniższy tekst (wszystko między liniami) jako pierwszą wiadomość w nowym czacie.

---

Pracujemy lokalnie na plikach w katalogu projektu `pogoda-gdynia` (ESP32-S3 + ST7789,
wyświetlacz pogodowo-domowy w łazience, motyw V3 „Pasmowy"). Rozmawiamy po polsku.

**Zacznij od przeglądu — nie pisz jeszcze kodu:**

1. Przeczytaj `docs/SESJA-2026-07-23.md` — to pełne przekazanie stanu z poprzedniej sesji
   (v139 → v144: co zrobione, co zweryfikowane, pułapki, co zostało).
2. Przejrzyj `ARCHITEKTURA.md`, `BACKLOG.md`, `docs/KATALOG-DANYCH.md`
   oraz kod, którego dotyczy zadanie: `RadarMap.cpp`, `RadarClient.cpp`, `pogoda-gdynia.ino`.
3. Sprawdź stan repo (`git log`, `git status`, `Version.h`) i — jeśli masz łączność —
   stan urządzenia przez panel (`/api/state`, w razie potrzeby `/api/diag`).
4. **Zweryfikuj v144**, bo nie została sprawdzona wizualnie (padło łącze):
   ekran główny (pasek PV nie przelewa się, jest metryka „dom X kW") oraz panel
   (sekcje „Klimat pokoi" i „Powietrze — 7 dni", obecność `air_hist` w `/api/diag`).

**Potem powiedz mi krótko, czy wszystko się zgadza.** Jeśli tak — bierzemy zadanie #19:

**#19 — utwardzenie alokacji PSRAM radaru przy starcie.**
Dziś, gdy przy starcie nie uda się zarezerwować buforów mapy w PSRAM, radar leci przez całą
sesję w trybie zastępczym „pomiar punktowy" (zaprojektowany tryb awaryjny 7d — działa poprawnie,
pomaga zwykły restart). Chcemy, żeby pełna mapa wchodziła pewniej.
Punkt wyjścia: `RadarMap.cpp` ~305–325 (`psramFound()`, `ps_malloc` na klatki i kafelek, `gErr`).
Kierunki do rozważenia: ponawianie alokacji zamiast jednorazowej próby przy starcie, alokacja
wcześniej w `setup()` (zanim PSRAM się pofragmentuje), mniejszy bufor jako fallback,
jawny stan w panelu. Najpierw przedstaw mi plan i ryzyka, dopiero po akceptacji koduj.

**Zasady, które obowiązują zawsze:**
- Repo jest **publiczne** — zero kluczy, haseł, tokenów, adresów IP, SSID w kodzie i dokumentach.
  Sekrety żyją wyłącznie w NVS urządzenia i w panelu WWW.
- **Nigdy nie wklejaj mi tokenów, haseł ani kluczy do czatu.**
- Urządzenie jest **na stałe odłączone od komputera — tylko OTA, bez USB**. Firmware psujący
  rozruch albo sieć jest nie do odratowania bez rozkręcania. To zadanie dotyka pamięci przy
  starcie, więc jest z natury ryzykowne — działaj ostrożnie i weryfikuj po restarcie.
- Twarde bariery przed publikacją (`tools/release.sh`): **statyczny RAM < 76000 B**
  (teraz 73840) oraz adresy RTC `gPir @ 0x50000200`, `gLdr @ 0x500002c0` bez zmian.
- Do samego sprawdzenia buduj do `build_verify/`; `tools/release.sh` **od razu publikuje** wydanie.
- Tryb pracy: kod piszą osobni agenci z briefem → Ty recenzujesz → build i bramy → publikacja
  → weryfikacja na żywo (zrzut ekranu). Komentarze w kodzie po polsku, wyjaśniające *dlaczego*.
- Pułapka fontowa: `plex::f24()` to font zegara (14 glifów, bez przecinka i liter) — do liczb
  z przecinkiem i tekstu używaj f20/f13/f11/f10/f52.
- Gdy urządzenie „nie odpowiada" — najpierw sprawdź sieć **mojego Maca** (potrafi zgubić Wi-Fi),
  a nie zakładaj awarii firmware.

---
