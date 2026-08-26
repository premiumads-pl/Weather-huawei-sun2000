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

// ---------- (v175) PANEL OLED + CZTERY PRZYCISKI — WYBOR TRYBU LADOWANIA AUTA ----
// To OSOBNE URZADZENIE na tej samej plytce, a nie nowy widok glownego ekranu: ma
// wlasny sterownik, wlasny rytm rysowania i wlasne przyciski. Numerow widokow
// (VIEW_*) nie dotyka — patrz nota przy VIEW_COUNT nizej.
//
// PINY POTWIERDZONE NA SPRZECIE (25.08.2026), nie wziete z propozycji w notatce:
// dokumentacja projektu proponowala K1..K4 na 15/16/17/18, wlasciciel polutowal
// 6/15/16/17. Zrodlem prawdy jest ten plik.
constexpr int PIN_OLED_SDA = 5;
constexpr int PIN_OLED_SCL = 6;
// 400 kHz, bo cala klatka to 1024 B: przy 100 kHz jedna strona (128 B) szlaby ~12 ms,
// czyli dluzej niz CALA klatka glownego ekranu. Modul ma wlasne podciagniecia,
// wiec szybki tryb jest w jego zasiegu.
constexpr uint32_t OLED_I2C_HZ = 400000;
// (v176) OBROT OBRAZU. Modul wisi PRZYKRECONY DO GORY NOGAMI — piny na dole,
// przyciski po lewej — wiec obraz trzeba obrocic wzgledem tego, co uznaje za
// normalne sam sterownik. false = ustawienie fabryczne modulu (0xA1,0xC8),
// true = obrot o 180 stopni (0xA0,0xC0). To jest wylacznie kwestia MONTAZU,
// nie sprzetu: przy przykreceniu na drugi sposob wraca sie na false.
constexpr bool OLED_FLIP180 = true;
// Adresu NIE zakladamy: te moduly jada 0x3C albo 0x3D (zworka SA0 na spodzie).
// begin() sprawdza po kolei — ACK rozstrzyga, nie nadruk na plytce.
constexpr uint8_t OLED_ADDR_A = 0x3C;
constexpr uint8_t OLED_ADDR_B = 0x3D;

// Przyciski ZWIERAJA PIN DO MASY (zmierzone miernikiem), wiec INPUT_PULLUP i logika
// odwrocona: LOW = wcisniety. Zaden z tych pinow nie jest strapujacy ani zajety
// (TFT ma 8-12/14, dotyk 7, PIR 13, LDR 1, USB CDC 19/20).
constexpr int PIN_BTN_1 = 18;   // K1 na module
constexpr int PIN_BTN_2 = 17;   // K2
constexpr int PIN_BTN_3 = 16;   // K3
constexpr int PIN_BTN_4 = 15;   // K4

// MAPOWANIE ROL — JEDNO MIEJSCE I TYLKO TO JEDNO.
// Nadruk na listwie idzie K4 K3 K2 K1 OD LEWEJ, a same przyciski sa opisane
// strzalkami i symbolami OD GORY — czyli nie wiadomo, ktory fizyczny guzik siedzi
// na ktorym pinie, dopoki wlasciciel nie zobaczy ekranu TEST PRZYCISKOW. Do tego
// czasu stala tu kolejnosc naturalna. Poprawka po tescie to zmiana TYCH CZTERECH
// LICZB i nic wiecej — dlatego sa indeksami w tablicy pinow (0 = PIN_BTN_1), a nie
// numerami GPIO: kod obslugi nigdzie indziej nie zna zwiazku "rola -> pin".
//
// (v176) TE LICZBY SA JUZ PO POMIARZE, nie sa zgadniete: ekran TEST PRZYCISKOW na
// zywo 25.08.2026 plus zgloszenie wlasciciela ("strzalki dzialaja odwrotnie, gora
// przewija w dol"). Przyczyna jest ta sama, co przy OLED_FLIP180 wyzej — modul jest
// PRZYKRECONY DO GORY NOGAMI, wiec kolejnosc klawiszy z nadruku wypada u wlasciciela
// ODWROTNIE: guzik lezacy pod palcem jako "w gore" siedzi na PIN_BTN_2, a para
// zatwierdz/powrot zamienia sie miejscami. Stad 1/0/3/2 zamiast 0/1/2/3.
// PRZY PRZYKRECENIU MODULU NA DRUGI SPOSOB te cztery liczby wracaja na 0/1/2/3
// (dokladnie tak, jak OLED_FLIP180 wraca wtedy na false) — to kwestia MONTAZU,
// a nie sprzetu, wiec nie "poprawiaj" ich z powrotem na kolejnosc naturalna.
constexpr uint8_t BTN_UP = 1;    // przewijanie w gore  (K2, GPIO17)
constexpr uint8_t BTN_DOWN = 0;  // przewijanie w dol   (K1, GPIO18)
constexpr uint8_t BTN_OK = 3;    // zatwierdz — wysyla tryb do Home Assistanta (K4, GPIO15)
constexpr uint8_t BTN_BACK = 2;  // wyjscie bez zmiany  (K3, GPIO16)

// Debounce PRZEZ HOLDOFF, a nie przez probkowanie: panel jest odpytywany raz na
// obieg petli rysowania (33-50 ms), czyli rzadziej niz trwaja drgania styku (~5 ms).
// Probkowanie potwierdzajace kosztowaloby caly obieg opoznienia na kazde zbocze;
// holdoff nie kosztuje nic i tak samo dziala dotyk GPIO7 (Touch.cpp).
constexpr uint32_t OLED_BTN_HOLDOFF_MS = 120;
// Menu wraca do ekranu spoczynkowego po tylu ms bez zadnego zbocza.
constexpr uint32_t OLED_MENU_IDLE_MS = 15000;
// (v178) Wejscie w ekran testu: cfg::BTN_OK i cfg::BTN_BACK wcisniete JEDNOCZESNIE
// przez tyle ms. DO v177 wystarczylo przytrzymanie DOWOLNEGO przycisku i to bylo
// zle, bo robilo z tamtego ekranu PULAPKE: w tescie zadne nacisniecie nic nie robi,
// wiec wlasciciel — przekonany, ze przyciski sa zepsute — naciskal DLUZEJ, wpadal
// w test i tam tez nic nie dostawal, a kazde kolejne nacisniecie zerowalo licznik
// wyjscia po bezczynnosci. Wejscie mogloby nie zalezec od konkretnych guzikow tylko
// dopoty, dopoki ich mapowania nie znalismy; po pomiarze z v176 (patrz BTN_* wyzej)
// juz je znamy. OK i WSTECZ to dwa SASIADUJACE klawisze na gorze modulu, wiec chwyt
// jest wygodny celowo, a przypadkiem nie do zrobienia.
constexpr uint32_t OLED_TEST_HOLD_MS = 3000;
// Wyjscie z testu PO BEZCZYNNOSCI — DRUGA furtka, na wypadek gdyby wlasciciel po
// prostu odszedl od panelu. Pierwsza i wazniejsza jest natychmiastowa: krotkie
// nacisniecie i puszczenie dowolnego przycisku (patrz pollButtons w OledPanel.cpp).
constexpr uint32_t OLED_TEST_EXIT_MS = 10000;
// Ile czekamy na POTWIERDZENIE wyslanego trybu w <prefix>/auto/stan. Po tym czasie
// menu pisze wprost, ze potwierdzenia nie ma — panel NIGDY nie przesuwa kropki sam
// z siebie. 10 s to ~2/3 kadencji Home Assistanta (15 s), czyli jedna pominieta
// publikacja jeszcze sie miesci, dwie juz nie.
constexpr uint32_t OLED_CONFIRM_MS = 10000;

// ---------- Siatka layoutu ----------
// Wysokosc gornej belki. UWAGA: to NIE jest juz stala motywu V1 — po usunieciu V1/V2
// (v160) jej jedynymi czytelnikami sa ekrany SYSTEMOWE, wspolne dla calego firmware:
// drawSetup() (tryb AP, "krok 1 z 2") i drawNetInfo() ("POLACZONO Z SIECIA"). Motyw V3
// tej belki nie rysuje — ma wlasny naglowek wg siatki z ThemeV3.h. Nie kasowac.
constexpr int HEADER_H = 28;
// (v160) PROG_Y/PROG_H zniknely razem z motywami V1/V2: opisywaly segmentowy pasek
// postepu, ktorego jedynym czytelnikiem bylo drawProgress(). Motyw V3 "Pasmowy" ma
// wlasny pasek (2 px na y=0..1, rysowany w drawV3) — te dwie stale nie definiowaly
// juz niczego.
constexpr int CONTENT_Y = 34;
constexpr int CONTENT_H = 172;
// FOOTER_Y/FOOTER_H tu NIE MA i niech tak zostanie. Byly, nie definiowaly niczego
// (zero uzyc w calym repo) i do tego podawaly zle liczby: twierdzily 208/32, gdy
// stopka realnie stoi na 206 i ma 34 px. Ktos, kto w dobrej wierze zmienilby te
// stala, nie zobaczylby ZADNEGO efektu. Jedyne zrodlo prawdy o stopce to
// WeatherUi::VIEW_H (= CONTENT_Y + CONTENT_H) i dolny pas rysowany w drawV3Bottom().

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
// (v169) Utrwalenie bazy licznikow miernika ("mtr2", 32 B, 3 wpisy NVS). Zapis leci
// TAKZE po zdarzeniu zmiany bazy; ten zegar dokłada regularne odswiezanie pola
// `lastEpoch` (ostatni udany odczyt), bez ktorego restart w okolicy polnocy kasuje
// jedyny odczyt SPRZED polnocy — a to on decyduje, czy ekran PRAD pokaze "dzis"
// z licznikow, czy zejdzie do calki. 15 minut to najgorszy przypadek 15 minut
// przeterminowania przy progu PV_BASE_FULL_MIN = 30 min, czyli polowa zapasu.
constexpr uint32_t PV_METER_STORE_MS = 15UL * 60UL * 1000UL;

// ---------- (v169) ROZSUNIECIE ZAPISOW DO NVS W CZASIE ----------------------
// KAZDY zapis blobu potrzebuje wolnych WPISOW NVS (2 narzutu + 1 na kazde rozpoczete
// 32 B) i NVS zapisuje NOWA kopie, ZANIM zwolni stara. Dopoki wszystkie zapisy
// wypadaly w tym samym takcie, zapotrzebowanie sumowalo sie w jednej chwili: co
// 30 minut zbiegaly sie profil PV, palnik, pokoje, powietrze i statystyki, czyli
// 110 z 111 dostepnych wpisow (pomiar z urzadzenia, v168). Wystarczylo, ze
// kompaktowanie strony nie zdazylo — i najwiekszy blob nie wchodzil.
//
// Te przesuniecia startowe rozkladaja zapisy tak, zeby ZADNE dwa nie trafialy w ten
// sam moment. Wszystkie okresy sa wielokrotnoscia 5 minut, wiec raz nadana faza jest
// zachowana na zawsze — nie ma dryfu, ktory po dobie znowu je zsumuje.
//   profil PV + palnik : 0 s   (co  5 min)  -> 12 + 7 = 19 wpisow
//   pokoje + powietrze : 90 s  (co 10 min)  -> 30 + 4 = 34 wpisy
//   statystyki PIR/LDR : 210 s (co 15 min)  -> 16 wpisow
//   baza licznikow     : 330 s (co 15 min)  -> 3 wpisy
// Najwiekszy szczyt w jednej chwili to 34 wpisy zamiast 110.
constexpr uint32_t NVS_PHASE_ROOMS_MS = 90UL * 1000UL;
constexpr uint32_t NVS_PHASE_SENS_MS = 210UL * 1000UL;
constexpr uint32_t NVS_PHASE_METER_MS = 330UL * 1000UL;
// (v166) Kadencja zapisu trwalej kopii statystyk PIR/LDR (klucz NVS "sen1", 424 B).
// LICZBY, a nie przeczucie:
//  * CO TRACIMY. To histogramy okna TYGODNIOWEGO. Przy 15 minutach zanik zasilania
//    cofa pomiar najwyzej o 900 s, czyli 0,15% siedmiodniowego zbioru (604800 s).
//    Zmierzone dotad okno to 14481 s — nawet w nim 900 s to 6%, a przy docelowym
//    tygodniu robi sie z tego szum. Kolejnego prysznica i tak nie zgubimy.
//  * CO KOSZTUJE. 424 B co 15 min = 96 zapisow na dobe = ~41 kB/dobe. Obok tego, co
//    ten sam netTask juz pisze: (v169) prof2 (292 B) i burn2 (148 B) co 5 min = 288
//    zapisow i ~127 kB/dobe oraz rh3 (872 B) co 10 min = ~126 kB/dobe. Dokladamy
//    wiec ~16% do istniejacego ruchu do flasha (dieta blobow z v169 zmniejszyla ten
//    ruch o polowe, wiec ten sam zapis wazy teraz procentowo wiecej), czyli nadal
//    nie zmieniamy rzedu wielkosci zuzycia.
//  * DLACZEGO NIE CZESCIEJ. Przy 5 minutach (kadencja profilu PV) byloby 288 zapisow
//    na dobe i ~122 kB, czyli trzykrotnie wiecej zapisow za uratowanie 10 minut
//    histogramu — a partycja NVS ma tu tylko 0x5000 B (20 kB) i jest wear-levelowana
//    w swoim wlasnym, malym obszarze.
//  * DLACZEGO NIE RZADZIEJ. Godzina znaczylaby, ze przypadkowy zanik pradu kasuje
//    caly wieczor, w ktorym mozna bylo zebrac zdarzenie "zostawione swiatlo".
constexpr uint32_t SENS_STORE_MS = 15UL * 60UL * 1000UL;
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

// ---------- PROGI SWIEZOSCI DANYCH (v158) — JEDNO miejsce dla calego projektu ----
// Do v157 kazdy ekran i panel mial WLASNA liczbe, wpisana na miejscu: pogoda 2x
// WEATHER_REFRESH_MS (WeatherUiV3.cpp), radar 1200 s, loty 60 s, pokoje 900 s,
// panel 900 s dla WSZYSTKIEGO w liscie "swiezosc zrodel" i 1800 s dla falownika
// w dwoch innych miejscach. Efekt byl dokladnie taki, jak zglosil wlasciciel: piec
// (kadencja 3 min) wygladal na swiezy jeszcze 15 minut po zamilknieciu, a loty
// (kadencja 15 s) migaly na "nieaktualne" po jednym poslizgu.
//
// REGULA (jedna, jawna): prog = 2,5 x pelna kadencja odpytywania danego zrodla,
// zaokraglone W GORE do czytelnej wartosci. Skad 2,5: JEDNA nieudana proba ma sie
// jeszcze zmiescic w progu (zrodlo nie miga na czerwono przy kazdym poslizgu
// cudzego serwera), DWIE pod rzad juz nie. Wyjatki od reguly sa opisane przy
// swoich stalych — NIE naginamy reguly, tylko piszemy wprost, ze przypadek jest
// inny i dlaczego.
//
// UWAGA, ktora zmienia arytmetyke: po BLEDZIE netTask ponawia SZYBCIEJ niz wynosi
// kadencja (pogoda/powietrze 30 s, radar 60 s, mapa 120 s, piec 120 s, loty 20 s —
// patrz netTask w pogoda-gdynia.ino). Prog "2,5 kadencji" w praktyce oznacza wiec
// nie "dwie proby", tylko "kadencja + kilka nieudanych ponowien", czyli trwaly
// brak lacznosci z danym API, a nie pojedyncza czkawka. Liczby ponizej licza sie
// od OSTATNIEGO UDANEGO pobrania (diag().*OkAt), wiec ponowienia nie przesuwaja
// zegara — przesuwa go dopiero sukces.
constexpr uint32_t WEATHER_STALE_MS = 40UL * 60UL * 1000UL;
// ^ kadencja 15 min (WEATHER_REFRESH_MS) -> 2,5 x 15 = 37,5 min -> 40 min.
//   Bylo: 2 x WEATHER_REFRESH_MS = 30 min, wpisane wprost w wxFresh() (WeatherUiV3.cpp).
constexpr uint32_t AIR_FETCH_STALE_MS = 40UL * 60UL * 1000UL;
// ^ kadencja 15 min (AIR_REFRESH_MS) -> ta sama liczba, co pogoda. To wiek NASZEGO
//   pobrania, NIE wiek probki ze stacji — te dwie rzeczy sa rozne i obie sa widoczne
//   (/api/diag: ok_ago_s vs sample_age_s).
constexpr uint32_t AIR_SAMPLE_STALE_S = 3UL * 3600UL;
// ^ INNA kadencja, ta sama regula: ARMAAG publikuje srednie GODZINOWE, wiec kadencja
//   ZRODLA to 60 min, a 2,5 x 60 = 150 min -> zaokraglone w gore do 3 h. Ta sama
//   liczba sluzy AirClientowi do decyzji GA17 -> GA24 (AirData.h::AIR_STALE_S).
constexpr uint32_t RADAR_STALE_MS = 15UL * 60UL * 1000UL;
// ^ kadencja 5 min (RADAR_REFRESH_MS) -> 2,5 x 5 = 12,5 min -> 15 min.
//   Bylo: 1200 s (20 min) wpisane liczba w naglowku ekranu RADAR.
constexpr uint32_t RADAR_MAP_STALE_S = 30UL * 60UL;
// ^ klatki RainViewera powstaja co 10 min (RADAR_MAP_REFRESH_MS jest pod to dobrane)
//   -> 2,5 x 10 = 25 min -> 30 min. Liczone od frameEpoch (czas KLATKI, nie pobrania):
//   swiezo pobrana mapa potrafi miec 10-minutowa klatke — zmierzone na urzadzeniu
//   16.08.2026: radar.ok_ago_s = 24 s przy frame_age_s = 610 s.
constexpr uint32_t PV_STALE_MS = 90UL * 1000UL;
// ^ kadencja 30 s (PV_REFRESH_MS) -> 2,5 x 30 = 75 s -> 90 s.
constexpr uint32_t PV_STALE_NIGHT_MS = 15UL * 60UL * 1000UL;
// ^ ta sama regula przy nocnej kadencji 5 min (PV_REFRESH_NIGHT_MS): 2,5 x 5 = 12,5
//   -> 15 min. Osobna stala, bo falownik spi po zachodzie i jednym progiem nie da sie
//   uczciwie opisac obu tempa.
constexpr uint32_t VI_STALE_MS = 8UL * 60UL * 1000UL;
// ^ kadencja 3 min (nextViAt = +180000 w netTask; limit Viessmanna 1450 zapytan/dobe
//   nie pozwala szybciej) -> 2,5 x 3 = 7,5 min -> 8 min. Ponowienie po bledzie to
//   120 s, wiec 8 min = trzecia nieudana proba z rzedu.
constexpr uint32_t FLIGHT_STALE_MS = 45UL * 1000UL;
// ^ kadencja 15 s (FLIGHT_REFRESH_MS) -> 2,5 x 15 = 37,5 s -> 45 s. WAZNE: loty sa
//   odpytywane TYLKO gdy ekran SAMOLOTY jest na wierzchu albo zaraz na niego wejdziemy
//   (gFlightsNeeded z WeatherUi::needsFlights), wiec MIEDZY pokazami wiek rosnie do
//   minut i to jest normalne. Prog opisuje wiec sensownie tylko czas SPEDZONY na tym
//   ekranie — i tak ma byc: samolot w 45 s przelatuje ~10 km, stara ramka klamie.
constexpr uint32_t FLIGHT_LIST_STALE_MS = 10UL * 60UL * 1000UL;
// ^ TO SAMO zrodlo, INNE pytanie, wiec INNA liczba — i to jest drugi swiadomy wyjatek.
//   FLIGHT_STALE_MS wyzej ocenia wiersze NA EKRANIE SAMOLOTY. Ta stala ocenia pozycje
//   "samoloty" na LISCIE ZRODEL (ekran diagnostyki, panel), gdzie 45 s bylo by alarmem
//   o niczym: poza ekranem SAMOLOTY netTask lotow w ogole nie odpytuje. Skad 10 min:
//   pelny obrot rotacji to 8 ekranow x dwellS (domyslnie 9 s, Settings.h) + dluzsze
//   przystanki radaru (20 s) i lotow (15 s) — okolo 1,5 min, wiec przy wlaczonej
//   rotacji ekran SAMOLOTY wraca mniej wiecej co poltorej minuty, czyli w 10 minutach
//   dostaje ze szesc-siedem podejsc. 10 min bez ANI JEDNEGO udanego pobrania znaczy wiec
//   albo wylaczona rotacje i brak dotkniec (sytuacja normalna, ale wtedy lista i tak
//   nie klamie: pisze wiek), albo trwaly blad API. Nie da sie tego wyprowadzic z
//   kadencji, bo kadencji tu po prostu nie ma — i tak to jest opisane, zamiast udawac.
constexpr uint32_t BLE_STALE_MS = 15UL * 60UL * 1000UL;
// ^ WYJATEK OD REGULY, celowy. Kadencja NASZEGO nasluchu to 4 s skanu co 20 s (netTask),
//   czyli 2,5 x 20 s = 50 s — i taki prog bylby klamstwem, bo o tym, kiedy przyjdzie
//   ramka, decyduje CZUJNIK, nie my. Zmierzone na zywo 16.08.2026 (/api/ble): wieki
//   22, 23, 24 i 78 s — czyli pojedynczy czujnik potrafi milczec przez kilka naszych
//   okien skanu przy zupelnie zdrowej baterii. 15 min to ~11x najgorszy zmierzony wiek:
//   pokoj gasnie dopiero wtedy, gdy czujnik naprawde zamilkl. Liczba jest ta sama,
//   co dotad wpisana w v3Home (900), tylko przeniesiona tutaj razem z uzasadnieniem.
constexpr uint32_t MQTT_STALE_MS = 3UL * 60UL * 1000UL;
// ^ kadencja 60 s: telemetria urzadzenia (kDevPublishMs w MqttClient.cpp) idzie co
//   minute NIEZALEZNIE od tego, czy cokolwiek sie zmienilo, wiec to ona wyznacza
//   rytm. 2,5 x 60 s = 150 s -> 3 min. Bylo: 900 s w panelu, czyli 15 pominietych
//   publikacji z rzedu wygladalo na zdrowe polaczenie.
constexpr uint32_t AUTO_STALE_MS = 45UL * 1000UL;
// ^ (v174) DANE AUTA. Kadencja 15 s: tyle wynosi okres, z jakim Home Assistant
//   publikuje <prefix>/auto/stan. 2,5 x 15 = 37,5 s -> 45 s, czyli ta sama liczba,
//   co FLIGHT_STALE_MS przy tej samej kadencji — regula z v158 dziala tu wprost,
//   bez wyjatku.
//
//   TA STALA ROBI WIECEJ NIZ WYSZARZENIE. Poza ocena swiezosci w naglowku decyduje
//   takze o POMIJANIU calego ekranu AUTO w rotacji (WeatherUi::viewSkipped), czyli
//   dziala jak `air->ready` dla POWIETRZA i `hasViessmann()` dla PIECA. Dlatego jest
//   celowo KROTKA: ekran ma zniknac z rotacji, gdy automatyka w garazu zamilkla, a
//   nie pokazywac przez kwadrans stanu baterii sprzed obiadu. Dane sa PCHANE, nie
//   ciagniete — po naszej stronie NIE MA ponowienia, ktore mogloby wydluzyc realny
//   czas do nastepnej udanej proby (uwaga o ponowieniach kilkanascie linii wyzej
//   dotyczy zrodel odpytywanych przez netTask i tutaj nie obowiazuje). Milczenie
//   dluzsze niz 45 s znaczy wiec dokladnie jedno: broker albo Home Assistant nie
//   dostarczaja, i nie ma czego przeczekiwac.
constexpr uint32_t COST_STALE_MS = 3UL * 60UL * 1000UL;
// ^ (v180) KOSZT ENERGII KUPIONEJ Z SIECI (<prefix>/dom/stan, CostData.h). Kadencja
//   60 s: tyle wynosi okres, z jakim publikuje ja automatyka "Dom -> MQTT na
//   wyswietlacz" w Home Assistancie. Regula z v158 zastosowana wprost, dokladnie jak
//   przy AUTO_STALE_MS wyzej (tam 2,5 x 15 s = 37,5 s -> 45 s): 2,5 x 60 s = 150 s ->
//   zaokraglone w gore do 3 min, czyli TRZY pominiete publikacje z rzedu. Wychodzi
//   ta sama liczba, co MQTT_STALE_MS przy tej samej kadencji 60 s — i tak ma byc,
//   bo regula jest jedna, a nie jedna na stala.
//
//   TA STALA NIE POMIJA ZADNEGO EKRANU, w odroznieniu od AUTO_STALE_MS. Koszt jest
//   JEDNA LINIA w module PRAD na ekranie glownym (mainPvModule), wiec jej jedynym
//   zadaniem jest rozdzielenie stanu (b) od (c): swieza kwota vs ostatnia znana
//   kwota podpisana wiekiem. Stan (a) — "nigdy nic nie przyszlo" — rozstrzyga samo
//   CostModel::atMs == 0 i wtedy linii NIE MA W OGOLE.

constexpr int VIEW_COUNT = 14;  // [0 wycofany] / TERAZ / [2 wycofany] / RADAR / 5 DNI / W DOMU / PIEC / PV / SAMOLOTY / POWIETRZE / PAMIEC / RUCH / AUTO / STATYSTYKI
// Zrodlem prawdy dla numeru widoku jest WYLACZNIE ta stala (cfg::VIEW_*) — dawniej
// switch w drawView() mial gole "case 0:" / "case 1:" i przezyl niezauwazony przez
// kilka wersji. Kazde nowe uzycie numeru widoku ma isc przez cfg::VIEW_*, nigdy
// przez literal.
//
// (v162) SLOTY 0 I 2 SA WYCOFANE I ZAREZERWOWANE — NIE WOLNO ICH UZYC PONOWNIE.
// Byly to ekrany RETRO (Mario, slot 0) i GODZINY (slot 2), skasowane w calosci w
// tej wersji. NUMERY POZOSTALYCH WIDOKOW CELOWO ZOSTALY BEZ ZMIAN, mimo ze robi to
// dwie dziury w numeracji. Powod jest jeden i twardy: numer widoku WYCHODZI NA
// ZEWNATRZ firmware'u przez HTTP — POST /api/view?i=N przyjmuje go, a GET /api/view
// oddaje go w polach "cur"/"pin". Panel WWW i tools/capture_screens.py jada razem z
// firmware i daloby sie je poprawic, ale wszystko, co siedzi POZA repozytorium
// (zakladka w przegladarce, rest_command w Home Assistant, skrypt wlasciciela), NIE
// jada. Przenumerowanie 0..10 zmienioby po cichu ZNACZENIE kazdego numeru >= 3 (np.
// i=7 przestaloby byc PV, a stalo sie POKOJAMI) — czyli stary wpis nadal by dzialal,
// tylko pokazywalby CUDZY ekran. Dokladnie ta klasa bledu juz raz uderzyla w ten
// projekt (docs/screens.gif skladany z ekranow podpisanych cudzymi nazwami, patrz
// naglowek tools/capture_screens.py), wiec nie powtarzamy jej dla kosmetyki.
// Sprawdzone przed ta decyzja: numer widoku NIE trafia do MQTT (ani do discovery
// Home Assistant, ani do nazw encji, ani do zadnego ladunku — MqttClient.cpp nie zna
// slowa "view"), NIE trafia do NVS (przypiecie zyje w WeatherUi::pinned_, polu w RAM,
// i ginie przy restarcie) i nie ma go w /api/state ani /api/diag. Zostaje wylacznie
// /api/view — i to on rozstrzyga.
//
// Co to znaczy w praktyce dla slotow 0 i 2: nie maja juz wlasnych stalych ani wlasnych
// galezi w dispatcherach. POST /api/view?i=0 (albo i=2) nadal przechodzi przez
// pinView() (bo 0 i 2 < VIEW_COUNT) i trafia do galezi `default:` w drawV3()/
// drawV3Bottom(), ktora rysuje ekran GLOWNY. To jest zachowanie ZAMIERZONE i jedyne
// bezpieczne: urzadzenie jest tylko-OTA, wiec numer widoku spoza listy ma pokazac
// cokolwiek czytelnego, a nie czern.
constexpr int VIEW_NOW = 1;
constexpr int VIEW_RADAR = 3;   // animowana mapa opadow (pomijany, gdy nie pada)
constexpr int VIEW_DAYS = 4;
constexpr int VIEW_HOME = 5;    // czujniki BLE — pomijany, gdy zadnego nie ma
constexpr int VIEW_BOILER = 6;  // piec — pomijany, gdy nieautoryzowany
constexpr int VIEW_PV = 7;
constexpr int VIEW_FLIGHTS = 8;
// v117: POWIETRZE wszedl ZARAZ PO SAMOLOTY (9) — a to przesunelo PAMIEC/RUCH/
// STATYSTYKI o +1 wzgledem v116 (byly 9/10/11, teraz 10/11/12). Ten sam kontrakt,
// co przy v111 nizej: static_assert (nizej, pod stalymi) wymaga
// VIEW_STATS == VIEW_COUNT - 1, wiec nowy ekran NIE moze wejsc na koncu — musi
// wejsc PRZED serwisowa trojka, zeby STATS zostal ostatni.
constexpr int VIEW_AIR = 9;     // POWIETRZE: PM10/PM2.5 + indeks ARMAAG (GA17, zapas GA24) — pomijany, gdy brak danych z obu stacji
// v111: dwa nowe ekrany serwisowe (eksploracyjne — PAMIEC/RUCH) WESZLY PRZED
// STATS, nie po nim. Powod: static_assert (nizej) wymaga
// VIEW_STATS == VIEW_COUNT - 1. Wygodniej przesunac STATS na koniec niz rozluzniac
// ten kontrakt.
constexpr int VIEW_MEM = 10;    // PAMIEC: wszystkie rodzaje (SRAM/PSRAM/flash/partycje/RTC/ROM/stos)
constexpr int VIEW_MOTION = 11; // RUCH: PIR (rytm doby) + LDR (jasnosc) + wydajnosc rysowania (fps)
// v174: AUTO (dane Tesli z MQTT) wszedl PRZED STATS — TA SAMA operacja, co przy AIR
// (v117) i przy parze MEM/RUCH (v111), i z tego samego powodu: static_assert nizej
// wymaga VIEW_STATS == VIEW_COUNT - 1, wiec na koniec listy nic nie wchodzi.
// CENA JEST REALNA I ZOSTALA ZAPLACONA SWIADOMIE: STATYSTYKI przesuwaja sie z 12 na
// 13, czyli /api/view?i=12 od tego wydania pokazuje AUTO, a nie STATYSTYKI. To jest
// zmiana kontraktu WYCHODZACEGO NA ZEWNATRZ (POST /api/view?i=N i GET /api/view),
// wiec zakladka w przegladarce albo rest_command w Home Assistancie przypinajacy 12
// pokaze CUDZY ekran. Alternatywa — dopisanie AUTO jako 13 i zostawienie STATS na 12
// — wymagalaby ZDJECIA static_assert, czyli oddania jedynego straznika, ktory pilnuje,
// ze ekran serwisowy stoi na koncu; przy trzech poprzednich okazjach projekt wybieral
// przesuniecie STATS i tak zostaje. Miejsca W REPOZYTORIUM, ktore znaja numery,
// zaktualizowano razem z ta zmiana: panel WWW (VDIAG w Portal.cpp), tools/
// capture_screens.py (VIEWS + SLUG_TO_CONST, ma wlasny straznik zgodnosci z tym
// plikiem) i kViewNames w WeatherUi.cpp.
constexpr int VIEW_AUTO = 12;   // AUTO: stan Tesli z MQTT — pomijany, gdy brak swiezej wiadomosci
constexpr int VIEW_STATS = 13;  // ekran serwisowy — MUSI zostac VIEW_COUNT-1 (patrz wyzej)

// (v162) TEN WARUNEK MIESZKA TERAZ TUTAJ, NIE W FUNKCJI. Do v159 stal w
// WeatherUi.cpp::drawView() — i zniknal razem z ta funkcja przy usuwaniu motywow
// V1/V2 (v160), po cichu, zostawiajac w tym pliku dwa komentarze powolujace sie na
// straznika, ktorego juz nie bylo. Obok stalych, ktorych pilnuje, nie da sie go
// zgubic przy kasowaniu kodu rysujacego.
static_assert(VIEW_STATS == VIEW_COUNT - 1,
              "VIEW_STATS musi byc ostatnim numerem widoku (VIEW_COUNT-1) — "
              "nowy ekran dopisuj PRZED serwisowa trojka STATS/MEM/RUCH");
// Sloty 0 i 2 sa wycofane (patrz wyzej), ale VIEW_COUNT ich NIE zwalnia: zostaja w
// zakresie, zeby pinView() dalej przyjmowal stare numery i oddawal je galezi
// domyslnej (ekran GLOWNY) zamiast odrzucac albo gasic ekran.
static_assert(VIEW_NOW == 1 && VIEW_RADAR == 3,
              "sloty 0 i 2 sa ZAREZERWOWANE po wycofanych ekranach RETRO/GODZINY — "
              "nie przenumerowuj widokow, /api/view wystawia te numery na zewnatrz");

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
// (v174) EKRAN AUTO CELOWO NIE MA TU WLASNEJ STALEJ — i to jest decyzja, nie
// przeoczenie. Wlasna liczbe maja WYLACZNIE ekrany, ktore albo czekaja na ANIMACJE
// (RADAR: dwa przejscia klatek), albo na POBRANIE (SAMOLOTY: prefetch), albo stoja
// POZA petla rotacji (STATS/PAMIEC/RUCH — do nich nie da sie dojechac rotacja, wiec
// dwellS ich nie dotyczy). AUTO nalezy do petli i jest ekranem tej samej klasy, co
// PRAD, POWIETRZE, POKOJE i OGRZEWANIE — a te wszystkie jada na settings().dwellS,
// czyli na liczbie, ktora wlasciciel ustawia w panelu (3..60 s). Dolozenie
// VIEW_HOLD_AUTO_MS ZABRALOBY mu ten suwak dla jednego ekranu: przy dwellS = 20 s
// AUTO i tak schodziloby po swoich kilkunastu, bez zadnego widocznego powodu.
// Gdyby kiedys okazalo sie, ze uklad AUTO wymaga wiecej czasu na przeczytanie,
// wlasciwym miejscem zmiany jest dwellS (dotyczy calej rotacji), a nie wyjatek tutaj.
// Pelny cykl animacji radaru to (n+2)*RADAR_FRAME_MS: n klatek + 2 "przystanki"
// pauzy na najnowszej (patrz v3Radar w WeatherUiV3.cpp). Przy 13 klatkach (v109,
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
// echa na ekranie (v3Radar) lepiej zgadzal sie z tym, co realnie
// widac na kolejnych klatkach.
// To JAWNE PRZYBLIZENIE, nie pomiar — nie udawajmy inaczej. Wartosc do kalibracji,
// gdy zbierzemy realny ruch echa (kilka frontow, porownanie przesuniecia klatka
// do klatki z tym, co ten wspolczynnik przewiduje) — 2.0 to punkt startowy
// z literatury meteorologicznej, nie zmierzony na tym konkretnym niebie.
constexpr float RADAR_FLOW_GAIN = 2.0f;

}  // namespace cfg
