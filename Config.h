#pragma once

#include <cstdint>

// UWAGA: w tym pliku NIE MA żadnych sekretów.
// SSID, hasło, IP falownika i lokalizacja siedzą w pamięci NVS (patrz Settings.h)
// i konfiguruje się je przez panel WWW urządzenia.

namespace cfg {

// ---------- Aktualizacje OTA (publiczne repo, bez tokenu) ----------
constexpr const char* OTA_VERSION_URL =
    "https://github.com/premiumads-pl/Weather-huawei-sun2000/releases/latest/download/"
    "version.json";
constexpr const char* OTA_FIRMWARE_URL =
    "https://github.com/premiumads-pl/Weather-huawei-sun2000/releases/latest/download/"
    "firmware.bin";
constexpr uint32_t OTA_CHECK_MS = 15UL * 60UL * 1000UL;

// ---------- Dioda RGB (bilans z siecią) ----------
constexpr uint8_t LED_DAY = 90;         // jasność w dzień
constexpr uint8_t LED_NIGHT = 12;       // w nocy — ma nie oślepiać
constexpr int32_t LED_BALANCE_W = 300;  // +/- 300 W = "równowaga" (niebieski)
constexpr uint32_t LED_TEST_MS = 500;   // autotest kolorow przy starcie (na kolor)

// ---------- Lotnisko / loty ----------
constexpr float EPGD_LAT = 54.3823f;
constexpr float EPGD_LON = 18.4654f;
constexpr int FLIGHT_RADIUS_NM = 40;

// ---------- Ekran ----------
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr uint8_t TFT_ROTATION = 1;
constexpr bool TFT_INVERT_DISPLAY = false;
constexpr bool COLOR_TEST_MODE = false;

constexpr int PIN_TFT_CS = 10;
constexpr int PIN_TFT_DC = 8;
constexpr int PIN_TFT_RST = 9;
constexpr int PIN_TFT_MOSI = 11;
constexpr int PIN_TFT_SCLK = 12;
constexpr int PIN_TFT_BL = 14;

// ---------- Czujniki (nowe, v100) ----------
// LDR MUSI byc na ADC1 (GPIO 1-10), nie ADC2 — ADC2 nie dziala przy wlaczonym WiFi
// (radio zabiera przetwornik). GPIO 1 = ADC1_CH0, wolny (TFT ma 8-12/14, dotyk 7).
// Dzielnik: 3,3V -[LDR]- GPIO1 -[7,93k]- GND. Jasno => R_LDR male => napiecie WYZSZE.
constexpr int PIN_LDR = 1;
// AM312 (PIR): modul 3,3 V, OUT 3,3 V. GPIO 13 wolny, nie strapujacy. Odczyt CYFROWY,
// wiec fakt, ze GPIO 13 to ADC2, nie ma znaczenia (nie uzywamy tam ADC — i nie probuj,
// bo ADC2 przy wlaczonym WiFi nie dziala).
// Stalo tu "SR505 (PIR): VCC 5V" — nieprawda w obu polowach. Wlutowany jest AM312 i on
// jest zasilany 3,3 V; potwierdzil wlasciciel 16.07.2026. To nie jest kosmetyka: AM312 ma
// impuls ~2 s i okno martwe ~2 s, a SR505 ~8 s, wiec ta nazwa uzasadniala (poprawny)
// okres odpytywania w loop() zupelnie nie ta liczba. Realne zachowanie TEGO egzemplarza
// mierzymy dopiero teraz — pir_* w /api/diag.
constexpr int PIN_PIR = 13;

// ---------- Siatka layoutu ----------
constexpr int HEADER_H = 28;
constexpr int PROG_Y = 29;
constexpr int PROG_H = 3;
constexpr int CONTENT_Y = 34;
constexpr int CONTENT_H = 172;
// FOOTER_Y/FOOTER_H tu NIE MA i niech tak zostanie. Byly, nie definiowaly niczego
// (zero uzyc w calym repo) i do tego podawaly zle liczby: twierdzily 208/32, gdy
// stopka realnie stoi na 206 i ma 34 px. Ktos, kto w dobrej wierze zmienilby te
// stala, nie zobaczylby ZADNEGO efektu. Jedyne zrodlo prawdy o stopce to
// WeatherUi::VIEW_H (= CONTENT_Y + CONTENT_H) i wysokosc liczona w drawFooterTo().

// ---------- Podświetlenie — steruje nim OPTOREZYSTOR, nie zegar ----------
constexpr uint32_t BL_PWM_FREQ = 5000;
constexpr uint8_t BL_PWM_BITS = 8;
constexpr uint8_t BL_DAY = 255;   // światło zapalone
constexpr uint8_t BL_DIM = 130;   // półmrok
constexpr uint8_t BL_NIGHT = 45;  // ciemno

// NIGHT_FROM_H/NIGHT_TO_H (22/6) tu NIE MA i niech tak zostanie — patrz notatka
// przy FOOTER_Y wyżej, to ta sama pułapka. Miały DOKŁADNIE JEDNEGO odbiorcę:
// linię podświetlenia w loop(), którą zastąpił LDR. Nocne milczenie falownika ich
// NIE używa — pvMayBeAsleep() (WeatherData.h) liczy okno ze wschodu/zachodu
// z prognozy, więc ta zmiana go nie dotyka. Zostawienie ich „na zapas" dałoby
// gałąź wykonywaną praktycznie nigdy, czyli kod nietestowany i gnijący; historia
// gita pamięta te liczby lepiej niż martwa stała, która wygląda na żywą.
//
// ---------- Progi jasności (LDR na GPIO1) ----------
// Dzielnik: 3,3V -[LDR]- GPIO1 -[7,93 kΩ]- GND. Jasno => R_LDR małe => napięcie WYŻSZE.
// ZMIERZONE W TEJ ŁAZIENCE 16.07.2026, nie wzięte z noty katalogowej LDR-a:
//   PRAWDZIWA ciemność (23:30, zgaszone światło, pusto)  **17-26 mV**  (~1,3 MΩ)
//   zmierzch (19:30, jeszcze widno za oknem)               251 mV     (~96 kΩ)
//   półmrok (20:50)                                       1050 mV     (~16 kΩ)
//   światło zapalone                                      3164 mV     (~0,3 kΩ)
//
// UWAGA — te 251 mV były przez chwilę uznane za „ciemność" i to był BŁĄD, który
// kosztował wydanie (v103): zmierzono je o 19:30, czyli w zmierzchu. Prawdziwa
// ciemność jest **dziesięć razy niższa**, a LDR ma wtedy ~1,3 MΩ, czyli SIEDEM RAZY
// więcej niż katalogowe „dark 190 kΩ" (nota mierzy „ciemność" przy kilku luksach,
// nie w ciemności). Nie ufać nocie i nie ufać pomiarowi zrobionemu o złej porze.
//
// Progi poniżej ZOSTAJĄ mimo tej korekty: 20 mV jest 20x poniżej LDR_DIM_DOWN_MV,
// więc ciemność trafia w poziom 0 z ogromnym zapasem. Zmieniło się tylko to, że
// pasmo 400-650 nie stoi już w geometrycznym środku przerwy (ten wypadłby na ~145 mV)
// — stoi bliżej półmroku. Skutek: zmierzch 251 mV dostaje poziom „ciemno" (45),
// co jest obronne, ale warte sprawdzenia na pełnej dobie danych ldr_mv.
//
// Każda granica ma DWA progi (histereza). Bez tego odczyt drgający wokół pojedynczego
// progu przerzucałby poziom w kółko przez cały zmierzch i świt — a rampa w WeatherUi
// dochodzi do celu krokami, więc ekran nie mrugałby, tylko pulsował w tę i we w tę.
//
// Dobrane PARAMI tak, żeby środek PASMA histerezy trafiał w środek przerwy między
// zmierzonymi stanami. Środek liczony GEOMETRYCZNIE, bo LDR jest logarytmiczny —
// arytmetyczny (650 i 2107) siedziałby nieszczerze blisko stanu ciemniejszego:
//   pasmo 400-650   -> sqrt(400*650)  = 510  wobec przerwy sqrt(251*1050)  = 513
//   pasmo 1500-2200 -> sqrt(1500*2200)= 1817 wobec przerwy sqrt(1050*3164) = 1823
// Szerokość pasm ~1,6x i ~1,5x; do najbliższego ZMIERZONEGO stanu zostaje z każdej
// strony ~1,4-1,6x zapasu, czyli żaden próg nie stoi blisko czegokolwiek realnego.
constexpr uint16_t LDR_DIM_UP_MV = 650;     // ciemno  -> półmrok
constexpr uint16_t LDR_DIM_DOWN_MV = 400;   // półmrok -> ciemno
constexpr uint16_t LDR_DAY_UP_MV = 2200;    // półmrok -> światło
constexpr uint16_t LDR_DAY_DOWN_MV = 1500;  // światło -> półmrok

// LDR_BROKEN_MV/LDR_BROKEN_MS tu NIE MA — wykrywanie awarii czujnika zostało
// USUNIĘTE w v104, bo było oparte na błędnym pomiarze i psuło normalną pracę.
// Pełne uzasadnienie stoi przy logice podświetlenia w pogoda-gdynia.ino; w skrócie:
// próg 50 mV wziął się z założenia „ciemność = 251 mV", a to 251 mV zmierzono
// o 19:30, czyli w ZMIERZCHU. Prawdziwa ciemność o 23:30 to **17-26 mV**, więc
// czujnik działający poprawnie był rozpoznawany jako zepsuty.
// Progu, który odróżnia „odłączony" (~0 mV) od „ciemno" (~20 mV), po prostu nie ma —
// dzieli je tyle, ile wynosi nieliniowość ADC przy dnie skali.

// ---------- Czasy ----------
constexpr uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t PV_REFRESH_MS = 30UL * 1000UL;
// Noc: falownik śpi (Modbus TCP wyłączony), więc nie ma po co dobijać się co 30 s.
// Wracamy do 30 s natychmiast, gdy tylko falownik znów odpowie.
constexpr uint32_t PV_REFRESH_NIGHT_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t PV_STORE_MS = 5UL * 60UL * 1000UL;  // zapis profilu do NVS
constexpr uint32_t WIFI_RETRY_MS = 8000;
constexpr uint32_t RADAR_REFRESH_MS = 5UL * 60UL * 1000UL;  // klatki radaru co ~10 min
// Jakosc powietrza to srednie GODZINOWE — nowa probka raz na godzine, wiec 15 minut
// to i tak trzy-cztery odpyty na kazda swieza probke. To CUDZY serwer (ARMAAG/
// sensorbox), wiec nie ma po co pytac czesciej — patrz AirClient.cpp.
constexpr uint32_t AIR_REFRESH_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t FLIGHT_REFRESH_MS = 15000;
constexpr uint32_t FLIGHT_PREFETCH_MS = 6000;
constexpr uint32_t VIEW_HOLD_MS = 9000;
constexpr uint32_t TRANSITION_MS = 340;
constexpr uint32_t ENTER_ANIM_MS = 550;
constexpr uint32_t ALERT_SHOW_MS = 6500;
constexpr uint32_t ALERT_COOLDOWN_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t FRAME_ACTIVE_MS = 33;
constexpr uint32_t FRAME_IDLE_MS = 50;   // 20 fps na statycznym ekranie (pasek odliczania)

// Pomiar czasu klatki (rysowanie / wypchnięcie na SPI) + stan sterty, co 2 s na
// Serial. Przydatne po zmianie na dwa pasy — domyślnie wyłączone, bo to tylko log.
constexpr bool PROFILE_FRAME = false;

constexpr int VIEW_COUNT = 13;  // RETRO / TERAZ / GODZINY / RADAR / 5 DNI / W DOMU / PIEC / PV / SAMOLOTY / POWIETRZE / PAMIEC / RUCH / STATYSTYKI
// v114: RETRO wszedl PIERWSZY w rotacji (wyrazne zyczenie wlasciciela — ma je
// widziec zaraz po starcie, przed TERAZ). To przesuwa WSZYSTKIE pozostale numery
// widokow o +1 wzgledem v113 (VIEW_NOW byl 0, teraz jest 1, itd.). Zrodlem prawdy
// dla numeru widoku jest WYLACZNIE ta stala (cfg::VIEW_*) — dawniej (przed
// wprowadzeniem VIEW_NOW/VIEW_HOURS) switch w drawView() mial gole "case 0:" /
// "case 1:" i przezyl niezauwazony przez kilka wersji. Kazde nowe uzycie numeru
// widoku ma isc przez cfg::VIEW_*, nigdy przez literal.
constexpr int VIEW_RETRO = 0;   // ekran ozdobny w stylu gry platformowej 8/16-bit (Mario) — WeatherUi::drawViewRetro
constexpr int VIEW_NOW = 1;
constexpr int VIEW_HOURS = 2;
constexpr int VIEW_RADAR = 3;   // animowana mapa opadow (pomijany, gdy nie pada)
constexpr int VIEW_DAYS = 4;
constexpr int VIEW_HOME = 5;    // czujniki BLE — pomijany, gdy zadnego nie ma
constexpr int VIEW_BOILER = 6;  // piec — pomijany, gdy nieautoryzowany
constexpr int VIEW_PV = 7;
constexpr int VIEW_FLIGHTS = 8;
// v117: POWIETRZE wszedl ZARAZ PO SAMOLOTY (9) — a to przesunelo PAMIEC/RUCH/
// STATYSTYKI o +1 wzgledem v116 (byly 9/10/11, teraz 10/11/12). Ten sam kontrakt,
// co przy v111 nizej: static_assert w WeatherUi.cpp::drawView() wymaga
// VIEW_STATS == VIEW_COUNT - 1, wiec nowy ekran NIE moze wejsc na koncu — musi
// wejsc PRZED serwisowa trojka, zeby STATS zostal ostatni.
constexpr int VIEW_AIR = 9;     // POWIETRZE: PM10/PM2.5 + indeks ARMAAG (GA17, zapas GA24) — pomijany, gdy brak danych z obu stacji
// v111: dwa nowe ekrany serwisowe (eksploracyjne — PAMIEC/RUCH) WESZLY PRZED
// STATS, nie po nim. Powod: static_assert w WeatherUi.cpp::drawView() wymaga
// VIEW_STATS == VIEW_COUNT - 1 (inaczej rotacja widokow trafia w "default" i przez
// caly czas trzymania tego widoku ekran zostaje czarny — patrz komentarz przy
// tym switchu). Wygodniej przesunac STATS na koniec niz rozluzniac ten kontrakt.
constexpr int VIEW_MEM = 10;    // PAMIEC: wszystkie rodzaje (SRAM/PSRAM/flash/partycje/RTC/ROM/stos)
constexpr int VIEW_MOTION = 11; // RUCH: PIR (rytm doby) + LDR (jasnosc) + wydajnosc rysowania (fps)
constexpr int VIEW_STATS = 12;  // ekran serwisowy — MUSI zostac VIEW_COUNT-1 (patrz wyzej)

// --- progi zdrowia urządzenia (wskaźniki na ekranie statystyk) ---
// Temperatura: czujnik w ESP32-S3 mierzy strukturę (die), nie otoczenie.
// Nota katalogowa: zalecane otoczenie do +85 °C, maksymalna temperatura złącza
// (Tj) 125 °C — i to jest koniec skali, nie punkt pracy.
constexpr float CPU_T_MIN = 20.f;   // początek skali
constexpr float CPU_T_OK = 70.f;    // do tego miejsca: spokojnie (zielony)
constexpr float CPU_T_WARN = 90.f;  // powyżej: gorąco (żółty -> czerwony)
constexpr float CPU_T_SPEC = 85.f;  // granica z noty katalogowej — kreska na skali
constexpr float CPU_T_MAX = 125.f;  // Tj max — koniec skali

// Wolna sterta: poniżej DANGER radar nie ma jak zdekodować PNG, a TLS się dławi.
constexpr uint32_t HEAP_DANGER = 25000;
constexpr uint32_t HEAP_WARN = 45000;
constexpr uint32_t HEAP_FULL = 160000;  // pełna skala wskaźnika
constexpr uint32_t VIEW_HOLD_FLIGHTS_MS = 15000;
constexpr uint32_t VIEW_HOLD_STATS_MS = VIEW_HOLD_MS;   // tyle samo co reszta
// v111: PAMIEC i RUCH sa rownie geste jak STATS (kilka-kilkanascie liczb na
// ekranie) — dostaja wiecej czasu niz domyslne 9 s, zeby dalo sie to przeczytac.
// Osobne stale, NIE VIEW_HOLD_STATS_MS: ekran STATYSTYKI ma zostac nietkniety,
// wiec czas jego trzymania tez sie nie zmienia.
constexpr uint32_t VIEW_HOLD_MEM_MS = 14000;
constexpr uint32_t VIEW_HOLD_MOTION_MS = 14000;
// Pelny cykl animacji radaru to (n+2)*RADAR_FRAME_MS: n klatek + 2 "przystanki"
// pauzy na najnowszej (patrz WeatherUi::drawViewRadar). Przy 13 klatkach (v109,
// bylo 7, co 20 min) to (13+2)*650 = 9750 ms, wiec dwa pelne cykle to 19,5 s.
// Stara wartosc 16000 byla dobrana pod 7 klatek ((7+2)*650=5850 ms, tam "2x" to
// 11,7 s, z zapasem) — po przejsciu 7->13 obcinalaby animacje w ~64% DRUGIEJ
// petli, czyli widz nie zobaczylby juz dwoch pelnych przejsc. 20000 daje ~2,05
// cyklu — wraca do tego, co obiecuje komentarz ponizej.
constexpr uint32_t VIEW_HOLD_RADAR_MS = 20000;  // tyle, zeby animacja zdazyla przejsc 2x
constexpr uint32_t RADAR_MAP_REFRESH_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t RADAR_FRAME_MS = 650;       // wolniej = oko nadaza za frontem

// Fronty opadowe plyna z wiatrem na wysokosci, na ktorej "widzi" je radar
// (echo z chmur rzedu 700 hPa, ~3 km) — nie z wiatrem PRZYZIEMNYM (10 m), jedynym,
// jaki mamy z Open-Meteo. Ten na wysokosci bywa typowo 1,5-2,5x szybszy, bo znika
// tarcie o teren/zabudowe. RADAR_FLOW_GAIN mnozy windKmh z API, zeby wektor ruchu
// echa na ekranie (WeatherUi::drawViewRadar) lepiej zgadzal sie z tym, co realnie
// widac na kolejnych klatkach.
// To JAWNE PRZYBLIZENIE, nie pomiar — nie udawajmy inaczej. Wartosc do kalibracji,
// gdy zbierzemy realny ruch echa (kilka frontow, porownanie przesuniecia klatka
// do klatki z tym, co ten wspolczynnik przewiduje) — 2.0 to punkt startowy
// z literatury meteorologicznej, nie zmierzony na tym konkretnym niebie.
constexpr float RADAR_FLOW_GAIN = 2.0f;

}  // namespace cfg
