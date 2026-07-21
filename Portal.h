#pragma once

#include <WiFiClient.h>

#include <cstdint>

// Panel konfiguracyjny WWW.
// Bez konfiguracji: urzadzenie wystawia wlasny AP i pokazuje dane na ekranie.
// Po polaczeniu z WiFi: panel zostaje dostepny w sieci domowej pod IP urzadzenia.
namespace portal {

void beginAp();      // tryb konfiguracji (SoftAP)
void beginSta();     // panel w sieci domowej
void loop();         // wolane cyklicznie z zadania web

bool apActive();
const char* apSsid();
const char* apPass();
const char* apIp();

// Ustawione przez formularz — main ma zrestartowac lub przelaczyc sie na STA.
bool wifiJustSaved();
void clearWifiSavedFlag();

// Panel wlasnie testuje nowe dane WiFi (apiWifi w zadaniu web).
// Zadanie sieci MUSI wtedy odpuscic swoje WiFi.begin() ze starymi danymi z NVS —
// inaczej nadpisze probe panelu, a panel uzna powrot na STARA siec za dowod, ze
// nowe haslo dziala. Flaga SAMA WYGASA po kilkunastu sekundach, wiec nawet blad
// w panelu nie odetnie urzadzenia od sieci na stale.
bool wifiConfigBusy();

// Wyniki skanu WiFi to JEDEN globalny bufor w rdzeniu (WiFiScanClass::_scanResult),
// a scanNetworks() zaczyna od jego zwolnienia. Dwa zadania skanujace naraz = odczyt
// ze zwolnionej pamieci = panic. Kto skanuje, bierze te blokade na CALY cykl:
// scanNetworks() + odczyt wynikow (SSID/RSSI/BSSID) + scanDelete().
// NIE brac jej pod gLock — kolejnosc blokad musi zostac jednokierunkowa.
bool scanLock(uint32_t waitMs);
void scanUnlock();

// Prowizoryczna konfiguracja z portu szeregowego (do wgrania bez telefonu).
void serialConsole();

// Zrzut ekranu urzadzenia (BMP) — podpina go szkic glowny.
void setScreenshotHandler(void (*fn)(WiFiClient&));

// Sterowanie ekranem z panelu: set(idx) — idx<0 wraca do rotacji; get(cur, pin).
void setViewHandler(void (*setFn)(int), void (*getFn)(int&, int&));

// Podswietlenie: test sprzetu (wymus jasnosc na czas) + podglad, co kod realnie
// wystawia na PWM. Sluzy do rozstrzygniecia, czy pin podswietlenia jest w ogole
// sterowany z GPIO — patrz komentarz przy WeatherUi::backlightCurrent().
void setBacklightHandler(void (*testFn)(uint8_t, uint32_t), void (*getFn)(uint8_t&, uint8_t&));
// Wizualny test rampy jasnosci — ekran pokazuje wtedy sama liczbe PWM (v124).
void setBacklightSweepHandler(void (*fn)(uint32_t));

}  // namespace portal
