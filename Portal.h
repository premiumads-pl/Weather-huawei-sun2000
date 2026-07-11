#pragma once

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

// Prowizoryczna konfiguracja z portu szeregowego (do wgrania bez telefonu).
void serialConsole();

}  // namespace portal
