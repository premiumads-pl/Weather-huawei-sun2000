// Panel OLED wyboru trybu ladowania — sterownik SSD1306 w trybie STRONICOWYM
// plus obsluga czterech przyciskow. Uzasadnienie calosci stoi w OledPanel.h.

#include "OledPanel.h"

#include <Arduino.h>
#include <pgmspace.h>

// STEROWNIK I2C PROSTO Z ESP-IDF, A NIE BIBLIOTEKA Wire — I TO JEST DECYZJA
// O PAMIECI, ZMIERZONA, A NIE O GUSCIE. Pierwsza wersja tego pliku jechala na Wire
// i kompilacja pokazala, ile to naprawde kosztuje w statycznym RAM-ie (nm + mapa
// linkera, build_verify):
//     .bss.bus            (esp32-hal-i2c-ng.c)   1064 B
//     Wire + Wire1        (Wire.cpp)              200 B   <- Wire1 wchodzi GRATIS,
//                                                            bo siedzi w tym samym
//                                                            pliku obiektowym
//     .data._i2c_bus_array (esp32-hal-i2c-slave.c)  96 B  <- tryb SLAVE, ktorego
//                                                            nigdy nie uzyjemy
//     razem                                       1360 B
// Przy zapasie 1816 B do bariery 76 000 B (tools/release.sh) samo dolaczenie Wire
// zjadaloby 75% wydania — WIECEJ niz caly panel, ktory ma 213 B wlasnych danych.
// Sterownik IDF zostawia w statyku 28 B (s_platform + s_i2c_platform), a uchwyty
// magistrali i urzadzenia bierze ze STERTY, ktorej jest tu ~85 kB.
// EFEKT ZMIERZONY NA CALYM PROGRAMIE: 75 792 B (wersja na Wire) -> 74 448 B, czyli
// 1344 B mniej — reszta do 1360 B z sumy symboli to te kilkanascie bajtow, ktore
// wersja na IDF dodaje od siebie (dwa uchwyty i licznik bledow).
//
// PULAPKA PRZY WERYFIKACJI, ZEBY NIKT NA NIA NIE WPADL DRUGI RAZ: samo usuniecie
// #include <Wire.h> NIE zmienilo liczby ani o bajt. arduino-cli trzyma raz zbudowane
// biblioteki w cache'u szkicu i dalej linkuje Wire.cpp.o, mimo ze nic go juz nie
// wlacza. Dopiero kompilacja od zera (skasowany katalog w ~/Library/Caches/arduino/
// sketches/) pokazala prawdziwy wynik. Pomiar RAM-u po przyrostowym budowaniu
// potrafi wiec KLAMAC w obie strony.
#include <driver/i2c_master.h>
#include <esp_heap_caps.h>   // heap_caps_malloc — kopia obrazu dla WWW ma isc do PSRAM

#include <atomic>
#include <cstdio>
#include <cstring>

#include <ctime>         // (v194) time() — znacznik zegarowy utrwalanego wykresu

#include "Config.h"
#include "Format.h"      // fmt1() — polski przecinek dziesietny, wspolny z ekranem TFT
#include "Freshness.h"   // isFresh() — wzor swiezosci, wspolny z WeatherUiV3.cpp
#include "GraphBlob.h"   // (v194) blob wykresu mocy w NVS ("graf1")
#include "Log.h"
#include "MqttClient.h"  // requestAutoMode() / autoModeReqState()
#include "PlText.h"      // pltxt:: — silnik fontow projektu (dekoder UTF-8, metryki)
#include "PlexText.h"    // plex::f10/f11/f13 — te same fonty, co glowny ekran
#include "Settings.h"    // (v187) settings() — menu ustawien edytuje TE pola i tylko te
// (v187) WYLACZNIE dla kViewNames — nazw ekranow do podekranu "Ekran". Drugiej listy
// nazw w tym pliku NIE MA i byc nie moze; uzasadnienie stoi przy deklaracji tablicy
// w WeatherUi.h. Naglowek nie doklada tu nic ciezkiego: TFT_eSPI i tak wchodzi juz
// przez PlexText.h -> Plex10.h, wiec kosztem jest kilka struktur modeli danych.
#include "WeatherUi.h"

namespace oled {
namespace {

// ============================ STAN — CALY BUDZET RAM-U =======================
// Wszystko ponizej to razem 213 B (policzone nm-em po symbolach oled::), z czego
// 129 B to gPage — JEDNA strona obrazu zamiast pelnego kilobajta. Doliczajac
// 9 B zamowienia trybu w MqttClient.cpp i 28 B sterownika I2C z IDF, caly panel
// kosztuje 264 B statycznego RAM-u przy zapasie 1816 B do bariery.
// (v176) Podglad i sterowanie z panelu WWW dolozylo do tego 9 B: wskaznik na kopie
// obrazu (4 B — sama kopia siedzi w PSRAM), maska wirtualnych nacisniec (4 B)
// i indeks potwierdzonego trybu (1 B). Kilobajt kopii NIE wchodzi do tego rachunku
// i nie ma prawa wejsc — to jest cala idea tego rozwiazania.
// (v187) Cale menu ustawien — piec ekranow, edycja z podgladem i przewijana lista
// widokow — kosztuje 18 B: szesc bajtow stanu menu (dwa kursory, gorny wiersz listy,
// flaga edycji, wartosc do cofniecia) i trzy wskazniki do glownego ekranu.
// Zmierzone na calym programie (arduino-cli, build_verify): 74 520 B -> 74 536 B,
// czyli +16 B po upakowaniu w dziury wyrownania, przy barierze 76 000 B. Tak malo,
// bo menu NIE KOPIUJE ustawien — czyta i pisze wprost do settings(), a nazwy ekranow
// bierze z kViewNames. Kazda "wygodna" kopia zaczynalaby sie tutaj od kilkudziesieciu
// bajtow i konczyla pytaniem, ktora z dwoch wersji jest prawdziwa.
bool gPresent = false;
uint8_t gAddr = 0;
i2c_master_bus_handle_t gBus = nullptr;   // oba uchwyty siedza na STERCIE
i2c_master_dev_handle_t gDev = nullptr;

// Bufor JEDNEJ strony Z MIEJSCEM NA BAJT STERUJACY Z PRZODU: gPage[0] to na stale
// 0x40 ("dalej ida dane"), a piksele zajmuja gPage[1..128] — bajt na kolumne, bit
// na wiersz w obrebie strony. Dzieki temu cala strona idzie JEDNYM wywolaniem
// i2c_master_transmit(), bez przepisywania do drugiego bufora.
// TO JEST CALY BUFOR OBRAZU TEGO PANELU: 129 B zamiast 1024 B pelnej klatki.
uint8_t gPage[1 + 128];
uint8_t gPageIdx = 8;    // ktora strona czeka na wyslanie; 8 = klatka gotowa, nic nie robimy
uint32_t gI2cErr = 0;    // ile transakcji sie nie udalo (zerwany przewod, zly styk)

// (v176) KOPIA CALEGO OBRAZU DLA PODGLADU WWW — 1024 B, ale W PSRAM, nie w .bss.
// W statycznym RAM-ie zostaje po niej TEN WSKAZNIK, czyli 4 B: pelna klatka w .bss
// zjadlaby 56% zapasu do bariery 76 000 B i jest dokladnie tym, czego ten panel od
// poczatku unika (patrz punkt 1 w OledPanel.h). Uklad bajtow jest taki sam, jak
// w pamieci kontrolera, wiec zapis to zwykle memcpy strony — bez przepakowywania.
// nullptr = podgladu nie ma; panel dziala wtedy bez zmiany.
uint8_t* gShadow = nullptr;

// (v188) BUFOR WYKRESU MOCY — 128 B, TAKZE W PSRAM, z tego samego powodu i tym
// samym sposobem, co kopia obrazu wyzej. W statycznym RAM-ie zostaje SAM WSKAZNIK,
// czyli 4 B: 128 B w .bss to prawie dziesiata czesc calego zapasu do bariery
// 76 000 B, oddana za rzecz, ktora widac tylko na jednym z osmiu ekranow.
// nullptr = wykresu po prostu NIE MA, reszta ekranu spoczynkowego dziala bez zmiany
// (patrz drawGraph i graphTick — obie wychodza pierwsza linia). To NIE jest blad.
//
// JEDEN BAJT = JEDNA PROBKA, w jednostkach 0,1 kW (0..25,5 kW). Rozdzielczosc jest
// DOKLADNIE taka, jak napisu "2,0 kW" nad wykresem — mniejsza rysowalaby roznice,
// ktorych nie widac w liczbie, wieksza nie zmiescilaby sie w bajcie.
uint8_t* gGraph = nullptr;
uint8_t gGraphCnt = 0;    // ile probek jest w buforze (0..kGraphN)
uint8_t gGraphMax = 0;    // najwieksza probka SESJI — trzymana, zeby nie skanowac
                          // 128 B PSRAM-u osiem razy na klatke i raz na obieg petli
// (v189) LICZNIK DOPISAN — 1 B, i to jest bajt, bez ktorego wykres ZAMARZA. Rosnie
// przy KAZDEJ dopisanej probce, takze wtedy, gdy bufor jest juz pelny i gGraphCnt
// stoi na kGraphN. Od v189 pelny bufor sie PRZEWIJA (patrz graphTick), wiec po 6,4 h
// sesji ani gGraphCnt, ani gGraphMax nie zmieniaja sie juz przy nowej probce — a
// obraz owszem, bo caly przebieg jedzie o kolumne w lewo. signature() nie mieszalby
// wtedy niczego nowego i panel przestalby przerysowywac wykres, choc dane by plynely.
// To DOKLADNIE ten sam blad, ktory zlapano tu juz dwa razy: pole widoczne na szkle
// musi byc w podpisie. Przekrecenie po 256 dopisaniach (12,8 h) nie szkodzi — podpis
// ma WYKRYWAC ZMIANE, a nie liczyc probki: kolejne wartosci zawsze sie roznia.
uint8_t gGraphSeq = 0;
bool gCharging = false;   // trwa sesja (dopisujemy) / skonczyla sie (przygaszamy)
uint32_t gGraphHighMs = 0;  // millis() ostatniej chwili z moca >= kGraphOnKw
uint32_t gGraphSampMs = 0;  // millis() ostatniej dopisanej probki

// (v194) ODLOZONA DECYZJA PO ODTWORZENIU WYKRESU Z NVS — patrz GraphBlob.h.
// Bufor wczytujemy przy inicjalizacji panelu, ale w tej chwili NTP jeszcze nie
// doszedl i przerwy w czasie NIE DA SIE policzyc. Te trzy zmienne przenosza tamten
// stan do pierwszego przebiegu graphTick, w ktorym zegar bedzie juz wazny.
bool gRestorePending = false;    // czekamy na zegar, zeby rozstrzygnac los sesji
bool gRestoreCharging = false;   // czy w chwili zapisu sesja trwala
uint32_t gRestoreEpoch = 0;      // czas zegarowy ostatniej zapisanej probki

// (v176) KOLEJKA WIRTUALNYCH NACISNIEC — maska ROL do obsluzenia (bit 0..3).
// DWA ZADANIA, JEDNA SCIEZKA AKCJI: injectPress() wola zadanie serwera WWW, a
// onKey() musi sie wykonac w zadaniu petli rysowania, bo dotyka calego stanu ekranu
// (gScr, gCursor, gMsg, wysylka MQTT). Dlatego strona WWW tylko ZAPALA BIT, a step()
// go zdejmuje i wykonuje TE SAMA funkcje, co puszczenie fizycznego przycisku.
//
// atomic, a nie samo volatile: "gInject |= bit" i "gInject &= ~m" to obie operacje
// odczyt-modyfikacja-zapis, wiec przy dwoch zadaniach (i dwoch rdzeniach) zwykly
// volatile potrafilby zgubic bit zapalony dokladnie miedzy odczytem a zapisem.
// uint32_t, a nie uint8_t: 32-bitowe atomiki sa na Xtensa BEZ ZAMKA (instrukcja
// S32C1I), wersja bajtowa szlaby przez funkcje biblioteczna. Koszt roznicy to 3 B.
//
// MASKA SKLEJA POWTORZENIA: dwa klikniecia TEJ SAMEJ roli, ktore trafia w jeden
// obieg petli, zostana wykonane RAZ. Obieg trwa 33-50 ms, a klikniecia z przegladarki
// ida przez HTTP i siec, wiec w to okno musialyby wpasc dwa zadania serwera naraz.
// Cena pomylki jest przy tym asymetryczna i to ona rozstrzyga: zgubiony powtorzony
// ruch kursora to jeden wiersz menu mniej (widoczne od razu, klikniesz jeszcze raz),
// a bufor cykliczny kupilby te skrajnosc kosztem wiekszej ilosci stanu i drugiego
// miejsca, w ktorym moze sie przepelnic.
std::atomic<uint32_t> gInject{0};

// Tryb POTWIERDZONY przez auto/stan, jako indeks (-1 = brak swiezych danych albo
// napis spoza listy). Indeks, a nie kopia napisu: 1 B zamiast 8 B, a activeMode()
// i tak oddaje literal z flasha przez autoModeMqtt(). To DOKLADNIE to samo zrodlo,
// z ktorego bierze sie kropka na ekranie — panel i strona nie moga sie tu roznic.
int8_t gActive = -1;
int gRow0 = 0;           // pierwszy wiersz ekranu nalezacy do rysowanej strony (= 8 * gPageIdx)
uint32_t gSig = 0;       // podpis tresci — zmiana podpisu jest JEDYNYM powodem przerysowania
uint32_t gPagesSent = 0;
uint32_t gStepUs = 0;

// (v187) EKRANY USTAWIEN DOPISANE NA KONCU, A NIE WSTAWIONE W SRODEK: SCR_IDLE/MENU/
// TEST musza zachowac swoje wartosci, bo wchodza do signature() jako pierwsza rzecz
// mieszana i przesuniecie ktorejkolwiek zmienialoby podpis calego ekranu bez zadnej
// zmiany tresci. Kolejnosc SCR_VIEW..SCR_NIGHT ODPOWIADA kolejnosci wierszy w menu
// glownym ustawien — pilnuje tego static_assert przy enterSub().
enum Screen : uint8_t {
  SCR_IDLE = 0,
  SCR_MENU = 1,   // wybor trybu ladowania auta
  SCR_TEST = 2,
  SCR_SET = 3,    // menu ustawien: Ekran / Jasność / Rotacja / Noc
  SCR_VIEW = 4,   // Ekran — pilot do duzego wyswietlacza (przypiecie widoku)
  SCR_BRI = 5,    // Jasność — blDay / blDim / blNight
  SCR_ROT = 6,    // Rotacja — autoRotate / dwellS
  SCR_NIGHT = 7,  // Noc — nightStartH / nightEndH
};
uint8_t gScr = SCR_IDLE;
// "Jestesmy gdziekolwiek w ustawieniach" — jeden warunek zamiast wyliczanki pieciu
// stalych w czterech miejscach (podpis, bezczynnosc, obsluga klawiszy, nazwa ekranu).
// Dziala, bo ustawienia zajmuja CIAGLY ogon enuma i nic nie moze wejsc za nie.
inline bool inSettings() { return gScr >= SCR_SET; }
uint8_t gCursor = 0;      // podswietlony wiersz menu (0..3) — to NIE jest tryb aktywny
uint32_t gLastKeyMs = 0;  // ostatnie zbocze na dowolnym przycisku

// ---- (v187) STAN MENU USTAWIEN — 6 BAJTOW, I TAKI MA ZOSTAC -----------------
// Panel nie ma wlasnego bufora klatki i nie bedzie mial (punkt 1 w OledPanel.h), wiec
// tym bardziej nie ma miejsca na "wygodna" strukture stanu menu. Trzymamy tu WYLACZNIE
// to, czego nie da sie policzyc na miejscu: dwa kursory, gorny wiersz przewijanej
// listy, flage edycji i JEDNA wartosc do cofniecia. Same ustawienia mieszkaja w
// settings() i to one sa zrodlem prawdy — menu ich nie kopiuje.
uint8_t gSetCur = 0;      // wiersz menu glownego ustawien (0..3)
uint8_t gRowCur = 0;      // wiersz w podekranie (na liscie widokow: 0..viewRows()-1)
uint8_t gTop = 0;         // pierwszy WIDOCZNY wiersz listy widokow (przewijanie)
bool gEditing = false;    // true = ∧∨ zmieniaja WARTOSC, a nie wiersz
// Wartosc SPRZED wejscia w edycje — jedyne, co trzeba pamietac, zeby "#" i wyjscie
// po bezczynnosci potrafily cofnac zmiane. JEDNA, a nie komplet siedmiu pol: edytuje
// sie zawsze dokladnie jeden wiersz, a zatwierdzenie zamyka sprawe (wartosc idzie do
// NVS i nie ma juz czego cofac). uint16_t, bo najszersze pole to dwellS; bool
// autoRotate jedzie tu jako 0/1.
uint16_t gEditOld = 0;

// (v187) Wskazniki do glownego ekranu — patrz setUiHooks() w OledPanel.h.
// 12 B statycznego RAM-u za brak zaleznosci OledPanel -> WeatherUi.
void (*gPinFn)(int) = nullptr;
int (*gPinnedFn)() = nullptr;
void (*gBlFn)(uint8_t, uint32_t) = nullptr;

// Komunikat w naglowku menu. 0 = zwykly tytul.
enum Msg : uint8_t { MSG_NONE = 0, MSG_SENDING = 1, MSG_NOACK = 2, MSG_FAILED = 3 };
uint8_t gMsg = MSG_NONE;
uint32_t gSentAtMs = 0;   // 0 = nic nie czeka na potwierdzenie
char gSentMode[8] = {};

// Przyciski. Maski zamiast tablic bool: cztery bity zamiast czterech bajtow.
const int kPins[4] = {cfg::PIN_BTN_1, cfg::PIN_BTN_2, cfg::PIN_BTN_3, cfg::PIN_BTN_4};
uint8_t gDown = 0;        // stan po debounce (bit i = przycisk i wcisniety)
uint8_t gSwallow = 0;     // puszczenie tego przycisku ma NIE wykonac akcji
uint32_t gDownAt[4] = {}; // millis() wcisniecia — stad liczy sie przytrzymanie
uint32_t gEdgeAt[4] = {}; // millis() ostatniego przyjetego zbocza — holdoff drgan

// ============================ SIATKA UKLADU ==================================
// Liczby POLICZONE z metryk fontow (xAdvance kazdego glifu), a nie przymierzone na
// oko. (v190) NAJCIASNIEJSZE MIEJSCE CALEGO PANELU PRZENIOSLO SIE Z EKRANU TEST NA
// WIERSZ WARTOSCI EKRANU SPOCZYNKOWEGO: przy pelnej mocy ladowarki "11,0 kW" (45 px
// w f13) konczy sie na x=47, a wysrodkowany blok ikon zrodla zaczyna sie na x=54 —
// zostaje 6 px przerwy. Ekran TEST jest drugi: "GPIO15" konczy sie na x=56, a
// prawostronnie wyrownany napis "WCIŚNIĘTY" (53 px) zaczyna sie na x=72, czyli
// 15 px przerwy. Pozostale zapasy: nazwa trybu na ekranie spoczynkowym
// ("CAŁA NAPRZÓD", 93 px w f13) ma 32 px do prawej krawedzi, wiersz menu
// ("SŁOŃCE + MIN.", 85 px w f11) ma 26 px do kropki trybu aktywnego, a blok ikon
// zrodla ma 15 px do wyrownanego w prawo "100 %" (36 px w f13).
//
// SZESC PIKSELI TO MALO I TAK MA BYC. Gdyby ta przerwa miala urosnac, musialby
// przesunac sie blok ikon — a on stoi POSRODKU EKRANU i to jest jedyna rzecz,
// ktora go tam trzyma. Przypadek 11 kW to maksimum ladowarki (jednorazowo, przy
// ladowaniu z sieci); typowa nadwyzka fotowoltaiczna to "2,0 kW" = 38 px, czyli
// 13 px przerwy.
constexpr int kW = 128;
constexpr int kMarginX = 3;

// (v188) NAGLOWEK USTAWIEN I MENU — linia bazowa, ktora do v187 nosila takze napis
// "TRYB" na ekranie spoczynkowym. Sam napis zniknal (patrz drawIdle nizej), nazwa
// zostala: tytuly menu i ustawien stoja na niej dalej i nic ich nie rusza.
constexpr int kIdleTagY = 8;     // tytul menu / ekranu TEST, f10

// (v188) EKRAN SPOCZYNKOWY PO PRZEBUDOWIE — linie bazowe (dolna krawedz liter,
// standard GFX). Wyleciaty stad DWA WIERSZE TEKSTU: "TRYB" z gory (nazwa trybu mowi
// sama za siebie, a etykieta nad nia byla powtorka) i zdanie na dole ("ładuję ze
// słońca"), ktore zastapily dwie ikony zrodla przy wartosci mocy.
//
// (v190) WYLECIAL TRZECI WIERSZ — ETYKIETY "MOC" / "BATERIA" (dawne kIdleLabY = 27).
// Napisy "kW" i "%" stoja przy samych liczbach i mowia dokladnie to samo, co te
// etykiety, wiec byly TRZECIM juz powtorzeniem na tym ekranie — po "TRYB" i po
// zdaniu na dole. Zwolniony wiersz nie poszedl jednak na powietrze, tylko na DWIE
// rzeczy naraz: wartosci urosly z f11 na f13, a wykres dostal 10 dodatkowych
// wierszy (pas z 22 na 32). Na dwa metry wiekszy font wazy wiecej niz podpis.
//
// F13 JEST TU WEZSZY OD F11 MIMO WIEKSZEJ WYSOKOSCI i to nie jest przeoczenie,
// tylko zmierzona wlasciwosc tych dwoch tablic (suma xAdvance, jak stringWidth):
//   "11,0 kW"  f11 = 47 px   f13 = 45 px
//   "2,0 kW"   f11 = 40 px   f13 = 38 px
//   "100 %"    f11 = 34 px   f13 = 36 px
// Na wartosci mocy — czyli tam, gdzie jest ciasno — wieksze litery ZAJMUJA MNIEJ
// MIEJSCA. Procent jest o 2 px szerszy, ale stoi przy prawej krawedzi, gdzie zapasu
// jest 15 px, wiec nie ma to znaczenia.
//
// LICZBY SA POLICZONE Z METRYK FONTOW, nie przymierzone. Skrajne piksele glifow
// (xOffset/yOffset kazdego uzytego znaku) przy tych bazach wypadaja tak:
//   nazwa trybu, f13   y =  0..11   ("CAŁA NAPRZÓD" — akcentowane wersaliki siegaja
//                                    12 px nad baze, wiec baza 12 jest MINIMUM: przy
//                                    11 kreska nad "Ó" wypadlaby na y = -1 i px()
//                                    odcielaby ja bez sladu)
//   kreska             y = 16
//   wartosci, f13      y = 18..29   (gora od "k" w "kW", yOffset -11; dol od ogonka
//                                    przecinka w "2,0", ktory schodzi na sama baze)
//   wykres             y = 32..63
// "brak danych" w f13 ma ogonek "y" siegajacy y=14, czyli 2 px nad kreska — i to jest
// jedyny napis, ktory tam schodzi.
constexpr int kIdleModeY = 12;   // nazwa trybu, f13 — TERAZ PIERWSZY WIERSZ EKRANU
constexpr int kIdleRuleY = 16;   // pozioma kreska
// (v190) BAZA WARTOSCI 29, A NIE 28 — I TO JEST METRYKA, NIE GUST. Litera "k" w f13
// ma yOffset -11, wiec przy bazie 28 jej gorny piksel lezalby na y=17, DOKLADNIE
// przy kresce z y=16 — "kW" dotykaloby jej i czytaloby sie jak podkreslenie. Baza 29
// odsuwa napis o jeden wolny wiersz (y=17) i wciaz zostawia dwa wolne (y=30..31) do
// gornej granicy wykresu, bo ogonek przecinka schodzi najwyzej na sama baze.
constexpr int kIdleValY = 29;    // wartosci, f13
// (v190) PROCENT BATERII WYROWNANY W PRAWO, a nie do stalej kolumny (dawne
// kIdleCol2X = 74). Kolumna miala sens, dopoki nad wartoscia stala etykieta, ktorej
// lewa krawedz trzeba bylo z czyms zgrac; bez etykiety liczba wisiala w powietrzu
// z rosnaca dziura po prawej — "9 %" konczylo sie 27 px przed krawedzia, "100 %" 16.
// Wyrownanie w prawo daje jej STALY punkt zaczepienia: prawa krawedz napisu nie
// rusza sie przy przejsciu z 9 na 10 i z 99 na 100 procent, a to jest jedyne miejsce
// tego wiersza, ktore zmienia szerokosc. 125 to ta sama krawedz, na ktorej konczy
// sie nadwozie ikony auta (kCarX1) i stan przycisku na ekranie TEST (kTestRightX),
// czyli 3 px do brzegu szkla — tyle samo, co kMarginX po lewej.
constexpr int kIdleRightX = 125; // prawa krawedz kolumny BATERIA

// (v186) Ikona auta — PRAWY GORNY ROG ekranu spoczynkowego, x 107..125, y 2..10.
// (v188) MIEJSCA NIE ZMIENILISMY ANI O PIKSEL, mimo ze pod ikone wjechala nazwa
// trybu z dawnego kIdleModeY = 23. Zapas dalej jest i dalej jest policzony, tylko
// z innego napisu: najszersza nazwa trybu ("CAŁA NAPRZÓD", 93 px w f13) zaczyna sie
// na kMarginX = 3 i konczy na x=95, wiec do lewej krawedzi dachu (x=110) zostaje
// 14 px przerwy, a do nadwozia (kCarX0 = 107) — 11 px. Pionowo tez sie mijaja:
// glify f13 przy bazie 12 siegaja y=0..11, ikona y=2..10, czyli LEZA OBOK SIEBIE
// w tym samym pasie i wlasnie dlatego 11 px odstepu jest tu warunkiem czytelnosci,
// a nie ozdoba.
constexpr int kCarX0 = 107;      // lewa krawedz nadwozia
constexpr int kCarX1 = 125;      // prawa krawedz nadwozia (2 px do krawedzi ekranu)
constexpr int kCarBodyY0 = 5;    // gorna krawedz nadwozia
constexpr int kCarBodyY1 = 8;    // dolna krawedz nadwozia
constexpr int kCarRoofY = 2;     // szczyt dachu
constexpr int kCarWheelY = 10;   // dol kol

// (v188) IKONY ZRODLA — miedzy wartoscia mocy a procentem baterii.
// Slonce stoi ZAWSZE w tym samym miejscu, takze gdy jest jedyna ikona: staly punkt
// zaczepienia czyta sie jako "tu jest zrodlo", a ikona skaczaca w lewo i w prawo
// zaleznie od tego, czy obok stoi druga — jako usterka rysowania.
//
// (v190) BLOK PRZESUNIETY Z x=52..71 NA x=54..73, czyli DOKLADNIE NA SRODEK EKRANU:
// slonce 11 px + 2 px przerwy + wtyczka 7 px = 20 px, a (128 - 20) / 2 = 54, wiec po
// obu stronach zostaje po 54 px. To srodek BLOKU, nie slonca — regula "slonce stoi
// zawsze tak samo" zostaje w mocy, wiec przy samym sloncu wieniec promieni wypada
// 4 px na lewo od osi ekranu. Wysrodkowanie ma tu byc zaczepieniem dla oka, ktore
// przebiega wiersz od lewej do prawej (moc — zrodlo — bateria), a nie osia symetrii
// do sprawdzania linijka.
//
// SRODEK PIONOWY 24, A NIE 23 (srodek calego pasa liter): ikony maja sie zgrac
// z CYFRAMI, a te w f13 przy bazie 29 zajmuja y=20..28, czyli maja srodek na 24.
// Ascender "k" siega wyzej (y=18), ale to jedna litera na koncu napisu i rownanie
// do niej scieloby ikony o piksel w gore wzgledem tego, z czym naprawde sasiaduja.
// Przy cy=24 slonce zajmuje y=19..29, wtyczka y=19..28 — obie z 2 px zapasu
// i do kreski (y=16), i do gornej granicy wykresu (y=32).
constexpr int kSrcSunCx = 59;    // srodek slonca (tarcza r=3, promienie do +-5)
constexpr int kSrcPlugX = 67;    // lewa krawedz wtyczki (szerokosc 7)
constexpr int kSrcCy = 24;       // srodek pionowy obu ikon = srodek cyfr wiersza
constexpr uint8_t kSrcSunLo = 10;   // ponizej: sama wtyczka
constexpr uint8_t kSrcSunHi = 90;   // powyzej: samo slonce
constexpr float kSrcMinKw = 0.1f;   // ponizej: ZADNEJ ikony — nie ma czego dzielic

// (v188) WYKRES MOCY LADOWANIA — DOLNY PAS, cala szerokosc ekranu.
// (v190) PAS UROSL Z y=42..63 NA y=32..63, czyli z 22 na 32 wiersze — o polowe.
// Dziesiec wierszy przyszlo z wykasowanego wiersza etykiet "MOC" / "BATERIA"
// (patrz kIdleValY wyzej). Zmienila sie WYLACZNIE wysokosc rysowania: bufor
// zostaje w PSRAM, probek dalej jest 128, okno dalej ma 6,4 h.
//   kGraphY1 to LINIA ZERA (os czasu), a kGraphY0 to poziom maksimum skali,
//   czyli na slupek zostaje kGraphY1 - kGraphY0 = 31 px wysokosci (bylo 21).
// Rozdzielczosc pionowa rosnie wiec o polowe i to jest cala rzecz: przy typowej
// nadwyzce 2 kW slupek mial 21 px na pelna skale, teraz ma 31, czyli roznica
// miedzy 1,5 a 2,5 kW to 15 px zamiast 10.
// 128 PROBEK NA 128 KOLUMN, jedna kolumna = jedna probka = 3 minuty. Okno wychodzi
// 6,4 h i to jest liczba dobrana do ZJAWISKA, a nie do wygody: tyle mniej wiecej
// trwa pelna sesja z niskiego stanu baterii, wiec typowy przebieg miesci sie na
// ekranie w calosci i bez usredniania.
//
// (v189) CZAS PLYNIE W PRAWO, "TERAZ" STOI PRZY PRAWEJ KRAWEDZI. Do v188 najstarsza
// probka lezala na x=0 i wykres rosl w prawo w pusty pas — czyli chwila biezaca
// wedrowala przez ekran i przy krotkiej sesji trzeba bylo jej szukac. Od v189 jest
// odwrotnie: OSTATNIA probka zawsze na x=127, starsze w lewo, wolne kolumny (sesja
// krotsza niz okno) zostaja PO LEWEJ. Tak dziala kazdy wykres kroczacy i tylko tak
// prawa krawedz znaczy zawsze to samo.
constexpr int kGraphY0 = 32;   // (v190) bylo 42 — patrz akapit wyzej
constexpr int kGraphY1 = 63;
constexpr int kGraphN = 128;        // probek = kolumn ekranu
constexpr int kGraphHourPx = 20;    // kreska godzinowa: 20 probek x 3 min = 60 min
// (v189) NAPIS SKALI PRZY LEWEJ KRAWEDZI PASA, NA JEGO GORNEJ GRANICY — i to jest
// POPRAWKA CZYTELNOSCI, nie przestawianie mebli. Do v188 stal w prawym gornym rogu,
// mniej wiecej na wysokosci szczytu slupkow, i wlasciciel odczytal go jako opis
// LINII PRZERYWANEJ tuz obok: skoro linia to "2 kW", to slupki dwa razy wyzsze
// musza byc 4 kW. Arytmetyka byla dobra, napis nie mowil, DO CZEGO sie odnosi.
// Przy lewej krawedzi, dokladnie na gornej granicy pasa, czyta sie jednoznacznie:
// "gora tego wykresu to tyle". Format bez spacji ("2kW"), bo napis ma byc etykieta
// osi, a nie odczytem — ten z przecinkiem i spacja ("2,0 kW") stoi wyzej, przy lewej
// krawedzi wiersza wartosci. (v190) Ta roznica zapisu robi teraz WIECEJ niz do v189:
// odkad etykiety "MOC" / "BATERIA" zniknely, "2,0 kW" w f13 i "2kW" w f10 sa dwoma
// jedynymi napisami z jednostka na tym ekranie i musza sie od siebie odroznic samym
// wygladem — inaczej ten mniejszy czytaloby sie jak drugi, mniej wazny odczyt mocy.
constexpr int kGraphSclX = 0;       // lewa krawedz napisu "NkW"
constexpr uint32_t kGraphStepMs = 180000;  // 3 min miedzy probkami
// Poczatek sesji: moc przekroczyla ten prog PO co najmniej takiej przerwie ponizej.
// 0,3 kW jest wyraznie powyzej szumu pomiaru i ponizej najslabszego realnego
// ladowania (1-fazowo 6 A to ~1,4 kW), a 10 minut przerwy nie da sie pomylic
// z chwilowym zanikiem nadwyzki z falownika przy przechodzacej chmurze.
constexpr float kGraphOnKw = 0.3f;
constexpr uint32_t kGraphGapMs = 600000;   // 10 min ciszy = koniec sesji

// Menu i test — cztery wiersze po 13 px, pierwszy zaczyna sie na y=12.
// Ostatni konczy sie dokladnie na y=63, czyli wypelnia ekran co do piksela.
constexpr int kRow0Y = 12;
constexpr int kRowH = 13;
constexpr int kMenuTextDX = 6;    // wciecie tekstu w wierszu menu
constexpr int kMenuDotX = 119;    // srodek kropki "tryb aktywny"
constexpr int kTestGpioX = 22;    // kolumna "GPIOnn" na ekranie testu
constexpr int kTestRightX = 125;  // prawa krawedz kolumny stanu przycisku

// (v187) USTAWIENIA JADA NA TEJ SAMEJ SIATCE WIERSZY, CO MENU TRYBU — cztery wiersze
// po kRowH od kRow0Y. To nie jest oszczednosc na kodzie, tylko warunek czytelnosci:
// wlasciciel przechodzi miedzy tymi ekranami w dwa nacisniecia, wiec podskakujaca
// siatka wierszy czytalaby sie jak usterka rysowania.
constexpr int kSetValRight = 121;  // prawa krawedz kolumny WARTOSCI (ramka siega 123)
constexpr int kSetVisible = 4;     // ile wierszy listy widac naraz (caly ekran)
// NAGLOWEK USTAWIEN STOI 2 px NIZEJ NIZ POZOSTALE (kIdleTagY = 8) I TO JEST POMIAR,
// NIE GUST. Tytul "JASNOŚĆ" ma AKCENTOWANE WERSALIKI: w metrykach f10 najwyzszy
// piksel "Ś" lezy 10 px nad linia bazowa, wiec przy bazie 8 kreski nad Ś i Ć
// wypadalyby na y = -2 i px() odcielaby je bez sladu — zostalby napis "JASNOSC"
// z dziurami, ktorego nikt by nie powiazal z ta linijka. Przy bazie 10 glify
// mieszcza sie w pasie y = 0..11, czyli dokladnie nad pierwszym wierszem (kRow0Y = 12),
// i nie ma juz czego przycinac. Ta sama pulapka co przy U+2014 na ekranie testu:
// silnik fontow pomija po cichu to, czego nie umie narysowac.
constexpr int kSetHdrY = 10;

// ====================== SSD1306 — SEKWENCJA INICJALIZACJI =====================
// Wartosci dla panelu 128x64 z wewnetrzna pompka ladunkowa (moduly 0,96" i ich
// klony na SSD1315 przyjmuja te sama sekwencje — SSD1315 jest wstecznie zgodny).
// Dwie pozycje sa tu WAZNIEJSZE od reszty i dlatego sa opisane:
//   0x20,0x02 — ADRESOWANIE STRONICOWE. To ono pozwala wyslac 128 B jednej strony
//               i nie ruszac pozostalych siedmiu. Tryb poziomy (0x00) po kazdym
//               zapisie przesuwalby wskaznik dalej i wymuszal pelna klatke.
//   0xA1,0xC8 — obrot obrazu o 180 stopni (SEG remap + COM scan odwrocony), czyli
//               ustawienie typowe dla tych modulow: bez tego obraz jest do gory
//               nogami wzgledem opisu na plytce.
const uint8_t kInit[] PROGMEM = {
    0xAE,        // wyswietlacz off na czas konfiguracji
    0xD5, 0x80,  // zegar: dzielnik 1, oscylator nominalny
    0xA8, 0x3F,  // multiplex 1/64
    0xD3, 0x00,  // brak przesuniecia obrazu
    0x40,        // pierwsza linia = 0
    0x8D, 0x14,  // pompka ladunkowa WLACZONA (modul nie ma zewnetrznego 7,5 V)
    0x20, 0x02,  // adresowanie STRONICOWE — patrz wyzej
    // Obrot sterowany stala z Config.h — patrz OLED_FLIP180 i komentarz przy niej.
    static_cast<uint8_t>(cfg::OLED_FLIP180 ? 0xA0 : 0xA1),
    static_cast<uint8_t>(cfg::OLED_FLIP180 ? 0xC0 : 0xC8),
    0xDA, 0x12,  // uklad wyprowadzen COM: alternatywny, bez zamiany lewa/prawa
    0x81, 0x7F,  // kontrast sredni — w lazience w nocy pelny oslepia
    0xD9, 0xF1,  // faza wstepnego ladowania
    0xDB, 0x40,  // poziom deselect VCOMH
    0xA4,        // tresc z RAM-u (a nie "wszystko zapalone")
    0xA6,        // obraz normalny, nie negatyw
    0x2E,        // przewijanie sprzetowe wylaczone
    0xAF,        // wyswietlacz on
};

// ============================ TRANSPORT I2C ==================================

// Timeout POJEDYNCZEJ transakcji. 50 ms to ~16x wiecej, niz trwa najdluzsza z nich
// (129 B przy 400 kHz to ~3 ms) — czyli zapas na przeciazony rdzen, a nie na czekanie
// w nieskonczonosc przy urwanym przewodzie. i2c_master_transmit() blokuje ZADANIE
// (czeka na semafor, transfer robi przerwanie), wiec przez ten czas inne zadania
// dostaja procesor.
constexpr int kXferTimeoutMs = 50;

bool xfer(const uint8_t* buf, size_t len) {
  if (gDev == nullptr) return false;
  if (i2c_master_transmit(gDev, buf, len, kXferTimeoutMs) == ESP_OK) return true;
  ++gI2cErr;
  return false;
}

void cmd(uint8_t c) {
  const uint8_t b[2] = {0x00, c};  // bajt sterujacy 0x00: dalej ida KOMENDY
  xfer(b, sizeof(b));
}

// Wysyla gPage na wskazana strone kontrolera. JEDNA transakcja na 129 B: bajt
// sterujacy siedzi juz w gPage[0], wiec nie ma tu ani kopiowania, ani dzielenia
// pakietu na kawalki (sterownik IDF nie ma ograniczenia 128 B, ktore ma Wire).
//
// (v176) TU POWSTAJE TAKZE KOPIA DLA PODGLADU WWW i to jest jedyne miejsce, w ktorym
// moze powstac: kopiujemy DOKLADNIE te bajty, ktore poszly na szklo, w chwili, gdy
// tam poszly. TYLKO PO UDANEJ TRANSMISJI — gdy xfer() zawiodl (urwany przewod, zly
// styk), na szkle zostala STARA tresc tej strony, wiec podglad tez ma zostac stary.
// Kopiowanie mimo bledu pokazywaloby w przegladarce obraz, ktorego nikt nigdy nie
// zobaczyl na module. Zrodlem jest gPage + 1, bo gPage[0] to bajt sterujacy 0x40
// i do obrazu NIE nalezy.
void sendPage(uint8_t page) {
  cmd(static_cast<uint8_t>(0xB0 | page));  // wybor strony
  cmd(0x00);                               // kolumna 0, mlodsze cztery bity
  cmd(0x10);                               // kolumna 0, starsze cztery bity
  const bool ok = xfer(gPage, sizeof(gPage));
  ++gPagesSent;   // licznik PROB, tak jak dotad — nieudane widac obok w gI2cErr
  if (ok && gShadow != nullptr && page < 8) {
    memcpy(gShadow + page * 128, gPage + 1, 128);
  }
}

// ====================== PRYMITYWY RYSOWANIA (jedna strona) ===================
// Kazdy z nich pisze WYLACZNIE do gPage, czyli do osmiu wierszy o numerach
// gRow0..gRow0+7. Piksel spoza tego pasa jest odrzucany bez sladu — to wlasnie
// dzieki temu ta sama funkcja rysujaca ekran moze byc wolana osiem razy z rzedu
// i za kazdym razem zostawia po sobie inny fragment obrazu.

inline void px(int x, int y, bool on) {
  if (x < 0 || x >= kW) return;
  const int r = y - gRow0;
  if (r < 0 || r > 7) return;
  const uint8_t m = static_cast<uint8_t>(1u << r);
  // +1, bo gPage[0] to bajt sterujacy transmisji, a nie kolumna obrazu.
  uint8_t& b = gPage[1 + x];
  if (on) {
    b |= m;
  } else {
    b = static_cast<uint8_t>(b & ~m);
  }
}

void hline(int x0, int x1, int y, bool on) {
  for (int x = x0; x <= x1; ++x) px(x, y, on);
}

void fillRect(int x0, int y0, int x1, int y1, bool on) {
  for (int y = y0; y <= y1; ++y) hline(x0, x1, y, on);
}

// Kropka "tryb aktywny". Promien 2 daje kolko 5x5 — mniejsze gubi sie na siatce
// 128x64, wieksze zaczyna wygladac jak drugi kursor.
void disc(int cx, int cy, int r, bool on) {
  for (int dy = -r; dy <= r; ++dy) {
    for (int dx = -r; dx <= r; ++dx) {
      if (dx * dx + dy * dy <= r * r + 1) px(cx + dx, cy + dy, on);
    }
  }
}

// (v187) RAMKA 1 px WOKOL WARTOSCI — znak "TE liczbe wlasnie zmieniasz".
// Potrzebny, bo na tym panelu podswietlenie wiersza znaczy juz "tu stoi kursor",
// a w menu ustawien te dwa stany trzeba rozroznic: w jednym ∧∨ przesuwaja sie po
// wierszach, w drugim kreca wartoscia. Bez drugiego znaku wlasciciel naciska
// strzalke i nie wie, czy przeskoczy wiersz, czy zmieni jasnosc — a to jest
// dokladnie ta klasa niepewnosci, przez ktora panel dostal juz raz zgloszenie
// "przyciski sa zepsute". Ramka, a nie negatyw wartosci: negatyw na podswietlonym
// wierszu zlewa sie z tlem sasiada.
void frame(int x0, int y0, int x1, int y1, bool on) {
  hline(x0, x1, y0, on);
  hline(x0, x1, y1, on);
  for (int y = y0 + 1; y < y1; ++y) {
    px(x0, y, on);
    px(x1, y, on);
  }
}

// ============================ TEKST ==========================================
// Uzywamy SILNIKA FONTOW PROJEKTU: te same tablice (Plex10/11/13), ten sam dekoder
// UTF-8 i te same metryki, co glowny ekran — pltxt::decodeUtf8, pltxt::glyphIndex
// i pltxt::stringWidth nie znaja TFT_eSPI i dzialaja tu bez zmiany. Wlasny jest
// TYLKO ostatni krok: zamiast s.drawPixel() wolamy px(), czyli piszemy do strony.
// Fonty sa jednobitowe, wiec nadaja sie do OLED-a wprost.
//
// `on` = kolor pikseli glifu: true na czarnym tle, false na podswietlonym wierszu
// menu (inwersja kursora). Tla NIE malujemy — wiersz jest juz wypelniony fillRect.
int str(const pltxt::FontSet& f, const char* t, int x, int baseline, bool on) {
  int cx = x;
  const char* p = t;
  while (*p != '\0') {
    const int cp = pltxt::decodeUtf8(p);
    const int idx = pltxt::glyphIndex(f, cp);
    // POMINIECIE PO CICHU: dokladnie tak samo zachowuje sie pltxt::drawString.
    // Dlatego kazdy polski znak uzyty na tym panelu jest sprawdzony w tablicy kodow
    // wybranego fontu (Plex10/11/13 maja komplet) — inaczej z "SŁOŃCE" zostaloby
    // "SOCE" i nikt by sie o tym nie dowiedzial.
    if (idx < 0) continue;

    GFXglyph g;
    memcpy_P(&g, &f.glyphs[idx], sizeof(GFXglyph));
    const int top = baseline + g.yOffset;

    // Odrzucenie glifu spoza pasa PRZED petla po pikselach. Bez tego kazda z osmiu
    // stron przechodzilaby po wszystkich pikselach wszystkich glifow — osiem razy
    // wiecej pracy niz trzeba.
    if (g.width > 0 && g.height > 0 && top <= gRow0 + 7 && top + g.height - 1 >= gRow0) {
      const uint8_t rowBytes = static_cast<uint8_t>((g.width + 7) / 8);
      const uint8_t* bm = f.bitmaps + g.bitmapOffset;
      for (uint8_t yy = 0; yy < g.height; ++yy) {
        const int py = top + yy;
        if (py < gRow0 || py > gRow0 + 7) continue;
        for (uint8_t xx = 0; xx < g.width; ++xx) {
          if ((pgm_read_byte(&bm[yy * rowBytes + (xx >> 3)]) & (0x80 >> (xx & 7))) != 0) {
            px(cx + g.xOffset + xx, py, on);
          }
        }
      }
    }
    cx += g.xAdvance;
  }
  return cx - x;
}

void strRight(const pltxt::FontSet& f, const char* t, int right, int baseline, bool on) {
  str(f, t, right - pltxt::stringWidth(f, t), baseline, on);
}

// ============================ TRESC EKRANOW ==================================

// (v188) IKONY ZRODLA — CO WIDAC, W JEDNEJ LICZBIE.
//
// Do v187 stalo w tym miejscu ZDANIE na dole ekranu ("ładuje ze słońca", "czeka na
// słońce", "brak kabla"...). Zniklo nie dlatego, ze bylo zle, tylko dlatego, ze bylo
// DRUGIM napisem o tym samym: nazwa trybu stoi juz na gorze wielkimi literami, a
// jedyna rzecza, ktorej nie dalo sie z niej odczytac, bylo ZRODLO PRADU plynacego
// w te sekunde. Zrodlo miesci sie w dwoch ikonach obok wartosci mocy — i dopiero
// zwolniony w ten sposob dolny pas dal miejsce na wykres, ktory pokazuje cala sesje
// zamiast jednej chwili.
//
// Zwraca MASKE tego, co ma sie znalezc na szkle: bit 0 = slonce, bit 1 = wtyczka.
// Osobna funkcja, a nie warunki wprost w drawIdle, bo TE SAME dwa bity musza wejsc
// do signature() — i to jest cala jej racja bytu. Gdyby podpis mieszal surowe
// `sunPct`, kazda zmiana o jeden procent skladalaby pelna klatke (osiem stron,
// ~25 ms) mimo ze na ekranie nie zmienia sie ani jeden piksel; a gdyby liczyl te
// bity po swojemu, byloby to drugie miejsce z progami 10/90 do rozjechania.
//
// PROGI 10 I 90 SA SZEROKIE CELOWO. Udzial slonca liczy sie z nadwyzki, ktora
// faluje przy kazdej chmurze, wiec ostry prog w polowie skali dawalby ikone
// zapalajaca sie i gasnaca co kilkanascie sekund — czyli migotanie, ktore czyta
// sie jak usterka. Przedzial 10..90 pokazuje OBIE ikony i to jest prawda o tym,
// co sie wtedy dzieje: energia idzie z dwoch zrodel naraz.
constexpr uint8_t kSrcSun = 1;
constexpr uint8_t kSrcPlug = 2;

uint8_t srcIcons(const AutoModel& a, bool fresh) {
  // PONIZEJ 0,1 kW NIE MA ZADNEJ IKONY, i to nie jest oszczednosc miejsca. Przy
  // niedzialajacym ladowaniu `sp` nie ma z czego wyjsc i przychodzi jako 0, czyli
  // "same z sieci" — a to jest zdanie o przeplywie, ktorego nie ma. Brak ikony
  // mowi "nie laduje", tak samo jak kreska zamiast mocy mowi "nie wiem".
  if (!fresh || a.kw < kSrcMinKw) return 0;
  uint8_t m = 0;
  if (a.sunPct >= kSrcSunLo) m |= kSrcSun;
  if (a.sunPct <= kSrcSunHi) m |= kSrcPlug;
  return m;
}

// SLONCE — tarcza r=3 i osiem promieni po jednym pikselu w odleglosci 5 od srodka.
// Calosc miesci sie w 11 x 11 px (cx-5..cx+5, cy-5..cy+5).
//
// PROMIENIE PO JEDNYM PIKSELU, A NIE KRESKI: przy tarczy o srednicy 7 px kreska
// dluzsza niz piksel zlewa sie z tarcza w klaks, ktory z dwoch metrow czyta sie jak
// kropka. Osiem osobnych punktow w regularnym wiencu widac jako slonce nawet wtedy,
// gdy pojedynczego punktu nie da sie rozroznic — to ten sam mechanizm, dzieki
// ktoremu ikona auta obok dziala przy 19 x 9 px.
void drawSunIcon(int cx, int cy) {
  disc(cx, cy, 3, true);
  px(cx, cy - 5, true);          // gora / dol / lewo / prawo
  px(cx, cy + 5, true);
  px(cx - 5, cy, true);
  px(cx + 5, cy, true);
  px(cx - 4, cy - 4, true);      // skosy
  px(cx + 4, cy - 4, true);
  px(cx - 4, cy + 4, true);
  px(cx + 4, cy + 4, true);
}

// WTYCZKA — 7 x 10 px, lewa krawedz na x0, srodek pionowy na cy.
//   y-5  .#...#.   dwa bolce
//   y-4  .#...#.
//   y-3  #######   korpus
//   y-2  #######
//   y-1  #######
//   y+0  #######
//   y+1  .#####.
//   y+2  ...#...   przewod
//   y+3  ...#...
//   y+4  ...#...
// Sylwetka jest CELOWO inna w obrysie niz slonce (prostokat kontra kolo), bo obie
// ikony potrafia stac obok siebie i wtedy rozroznia je nie detale, tylko ksztalt.
void drawPlugIcon(int x0, int cy) {
  px(x0 + 1, cy - 5, true);
  px(x0 + 5, cy - 5, true);
  px(x0 + 1, cy - 4, true);
  px(x0 + 5, cy - 4, true);
  fillRect(x0, cy - 3, x0 + 6, cy, true);
  hline(x0 + 1, x0 + 5, cy + 1, true);
  for (int y = cy + 2; y <= cy + 4; ++y) px(x0 + 3, y, true);
}

// (v186) IKONA AUTA — PRAWY GORNY ROG EKRANU SPOCZYNKOWEGO.
//
// CO MOWI: czy klucz BLE ma TERAZ zywe polaczenie z autem, czyli czy wybor trybu
// zrobiony na tym panelu ma przez co pojsc. Sylwetka WYPELNIONA = polaczenie jest
// i polecenie zadziala; sam OBRYS = klucz nie siega auta i polecenie NIE przejdzie.
//
// OBA STANY SA WIDOCZNE I TO JEST CALY POMYSL. Chowanie ikony przy braku polaczenia
// bylo pierwszym odruchem i jest bledem: znikajacy element czyta sie jak usterka
// wyswietlacza, a nie jak informacja. Wlasciciel ma rozroznic WYPELNIONA od PUSTEJ,
// a nie "jest" od "nie ma" — tylko pierwsza para mowi cokolwiek o aucie.
//
// TRZECI STAN — BRAK SWIEZYCH DANYCH — ROZSTRZYGA WYWOLUJACY, nie ta funkcja: przy
// !fresh drawIdle w ogole tu nie wchodzi i ikony nie ma na ekranie wcale. Wtedy nie
// wiemy NIC, a pusty obrys twierdzilby "sprawdzilem, polaczenia nie ma" — czyli
// klamalby o wiedzy, ktorej nie mamy. To ta sama konwencja, co kreski zamiast mocy
// i procentu baterii oraz "brak danych" zamiast nazwy trybu.
//
// PRYMITYWY, NIE BITMAPA. Tablica na te sylwetke to ~24 B stalych plus kod blittera,
// a kilkanascie hline() nie kosztuje ANI BAJTA pamieci — ta sama zasada rzadzi calym
// tym plikiem (patrz kInit i celowy brak WeatherIcons.h po stronie MQTT).
//
// SYLWETKA, PIKSEL PO PIKSELU (x rosnie w prawo, y w dol):
//   y= 2        ########             dach
//   y= 3       ##########
//   y= 4      ############
//   y= 5  ###################        nadwozie
//   y= 6  ###################
//   y= 7  ###################
//   y= 8  ###################
//   y= 9     ###      ###            kola
//   y=10     ###      ###
// Przy 19x9 px UPROSZCZENIE WYGRYWA Z DETALEM: szyby, klamki czy nadkola zlewaja sie
// z dwoch metrow w szara plame, a nadwozie + dach + dwa kola czyta sie jako samochod
// od razu. Ikona lezy w calosci w pasie y=0..15, czyli na stronach 0 i 1 kontrolera —
// px() sam odrzuca to, co nie nalezy do skladanej wlasnie strony, wiec funkcja moze
// byc wolana przy kazdej z osmiu i nie trzeba jej nigdzie warunkowac.
void drawCarIcon(bool link) {
  // 1) PELNA SYLWETKA — rysowana ZAWSZE, w obu stanach.
  hline(112, 119, kCarRoofY, true);                          // dach
  hline(111, 120, kCarRoofY + 1, true);
  hline(110, 121, kCarRoofY + 2, true);
  fillRect(kCarX0, kCarBodyY0, kCarX1, kCarBodyY1, true);    // nadwozie
  fillRect(110, kCarBodyY1 + 1, 112, kCarWheelY, true);      // kolo przednie
  fillRect(120, kCarBodyY1 + 1, 122, kCarWheelY, true);      // kolo tylne

  if (link) return;   // polaczenie jest — zostaje sylwetka wypelniona

  // 2) BRAK POLACZENIA: gasimy WNETRZE, zostaje obrys grubosci 1 px.
  //
  // WYCINANIE WNETRZA Z GOTOWEJ SYLWETKI, a nie rysowanie obrysu osobno — dzieki
  // temu obie wersje maja DOKLADNIE ten sam ksztalt zewnetrzny i nie da sie ich
  // rozjechac przy nastepnej poprawce ukladu. Ponizsze wiersze to dokladnie te
  // piksele, ktore maja wszystkich czterech sasiadow wewnatrz sylwetki.
  hline(112, 119, kCarRoofY + 1, false);
  hline(111, 120, kCarRoofY + 2, false);
  hline(110, 121, kCarBodyY0, false);          // pod dachem; maska i bagaznik zostaja
  hline(kCarX0 + 1, kCarX1 - 1, kCarBodyY0 + 1, false);
  hline(kCarX0 + 1, kCarX1 - 1, kCarBodyY0 + 2, false);
  hline(110, 112, kCarBodyY1, false);          // nad kolami; reszta spodu zostaje
  hline(120, 122, kCarBodyY1, false);
  px(111, kCarBodyY1 + 1, false);              // srodki kol
  px(121, kCarBodyY1 + 1, false);
}

// (v188) WYKRES MOCY LADOWANIA — DOLNY PAS EKRANU SPOCZYNKOWEGO.
//
// CO POKAZUJE: cala biezaca (albo ostatnia) sesje ladowania, probka co 3 minuty,
// 128 probek na 128 kolumn — czyli okno 6,4 h. Wartosc w liczbie nad wykresem mowi,
// ILE PLYNIE TERAZ; wykres mowi, JAK TO SZLO — a przy ladowaniu z nadwyzki
// fotowoltaicznej to jest wlasciwe pytanie, bo moc zmienia sie z kazda chmura
// i pojedynczy odczyt nie odrozni "slabo dzis" od "wlasnie przeszla chmura".
//
// SKALA PIONOWA JEST AUTOMATYCZNA i to jest decyzja, nie wygoda. Przy stalej skali
// 0-11 kW (maksimum ladowarki) typowa sesja z nadwyzki — 1,5 do 3 kW — lezalaby
// plaskim wezem tuz nad dolna krawedzia, gdzie roznicy miedzy 1,5 a 2,5 kW nie da
// sie zobaczyc. Skalujemy wiec do MAKSIMUM SESJI zaokraglonego w gore do pelnych kW
// i wypisujemy je przy LEWEJ krawedzi, na gornej granicy pasa (v189 — patrz nizej,
// przy napisie), bo wykres bez podpisanej skali to ladny ksztalt, a nie pomiar.
//
// (v189) OKNO JEDZIE ZA "TERAZ": najnowsza probka lezy na x=127, starsze w lewo,
// a po zapelnieniu bufora najstarsza wypada (patrz graphTick). Prawa krawedz znaczy
// wiec ZAWSZE to samo — chwile biezaca.
//
// PRZYGASZENIE PO SESJI ROBI RASTER, BO NIC INNEGO NIE MA. Wyswietlacz jest
// JEDNOBITOWY: piksel jest albo zapalony, albo zgaszony, odcieni nie ma i zadne
// sterowanie kontrastem ich nie zrobi (kontrast SSD1306 dziala na CALY ekran naraz).
// Jedyny polton, jaki istnieje na takim szkle, to szachownica — co drugi piksel —
// i z dwoch metrow czyta sie dokladnie jako "to bylo, a nie jest".
// (v191) ZASADA, KTORA RZADZI TA FUNKCJA — I TO JUZ DRUGI RAZ W TYM PLIKU:
// ELEMENT, KTORY ZNIKA, CZYTA SIE JAKO AWARIA, A NIE JAKO INFORMACJA.
// Tak samo rozwiazana jest ikona auta w v186: przy zerwanym kluczu BLE zostaje
// sam OBRYS nadwozia, a nie pustka po ikonie. Wykres zachowuje sie teraz spojnie:
// pas y=32..63 ZAWSZE ma ramke wykresu — linie zera, kreski godzinowe i napis
// skali. Przy zerze probek brakuje w nim WYLACZNIE slupkow, a to, ze ich nie ma,
// jest wprost napisane na srodku pasa.
//
// USTERKA, KTORA TO NAPRAWIA (zgloszona z zywego urzadzenia, graf: 0). Do v190
// stalo tu `if (gGraph == nullptr || gGraphCnt == 0) return;` i pusty bufor kasowal
// CALY pas: ani ramki, ani osi, ani podzialki, ani skali. Wlasciciel zobaczyl pusty
// prostokat w miejscu, gdzie przed chwila byl wykres, i zglosil to jako "wykres
// zniknal". Bufor siedzi w PSRAM i NIE PRZEZYWA RESTARTU (patrz deklaracja gGraph),
// wiec kazda aktualizacja firmware'u przy niepodpietym aucie pokazywala ten sam
// obraz awarii — choc liczenie bylo i jest poprawne. Zla byla WYLACZNIE reakcja
// na stan pusty i tylko ona sie tu zmienia.
//
// WARUNEK NA SAM BUFOR ZOSTAJE W CALOSCI I TO NIE JEST NIEKONSEKWENCJA WOBEC
// POWYZSZEJ ZASADY. gGraph == nullptr znaczy "alokacja 128 B w PSRAM sie nie udala",
// czyli stan ustalony RAZ przy starcie i niezmienny do konca zycia programu (patrz
// graphTick — wychodzi ta sama linia). Nic tu nie znika w trakcie patrzenia, wiec
// nie ma czego czytac jako awarii. Wiecej: bez bufora nie da sie odroznic "nie
// ladowalo" od "nie ma czym zmierzyc", a napis na srodku pasa twierdzilby wtedy
// "brak ładowania" TAKZE w trakcie ladowania. W tym jednym przypadku milczenie
// jest uczciwsze niz zdanie, ktoremu nikt nie kazal byc prawdziwym.
void drawGraph(bool fresh) {
  if (gGraph == nullptr) return;
  // Strona spoza pasa wykresu: wychodzimy PRZED petla po 128 kolumnach. Bez tego
  // kazda z osmiu stron przechodzilaby przez caly wykres, zeby px() odrzucilo
  // wszystko co do piksela — osiem razy wiecej pracy niz trzeba, przy pasie, ktory
  // lezy na czterech stronach z osmiu.
  // (v190) PO PODNIESIENIU GORNEJ GRANICY NA y=32 PAS ZACZYNA SIE DOKLADNIE NA
  // GRANICY STRONY (32 = 4 x 8), wiec zajmuje strony 4..7 W CALOSCI. Do v189 gorna
  // granica (y=42) lezala w srodku strony 5 i te strone caly wykres przechodzil dla
  // szesciu wierszy z osmiu. Pas urosl o polowe, a stron do przejscia przybyla
  // JEDNA — z trzech na cztery — i zadna nie jest juz przechodzona na wpol darmo.
  if (gRow0 > kGraphY1 || gRow0 + 7 < kGraphY0) return;

  // Skala: maksimum sesji (w 0,1 kW) w gore do pelnych kW, nigdy mniej niz 1 kW —
  // przy samych zerach dzielenie przez zero, a przy 0,4 kW slupek na cala wysokosc
  // ekranu, ktory klamalby o skali rzedu wielkosci.
  int kwMax = (gGraphMax + 9) / 10;
  if (kwMax < 1) kwMax = 1;

  const int h = kGraphY1 - kGraphY0;   // (v190) 31 px na kwMax kW — bylo 21
  // LINIE POMOCNICZE CO 1 kW TYLKO WTEDY, GDY NA 1 kW WYPADAJA CO NAJMNIEJ 3 px.
  // Ciasniej niz co 3 px kropkowane linie zlewaja sie na monochromie w szara kase,
  // ktora zaslania przebieg zamiast go opisywac — wtedy przechodzimy na co 2 kW.
  //
  // (v190) WARUNEK ZOSTAJE BEZ ZMIANY I TO JEST SPRAWDZONE, A NIE PRZEOCZONE. Jest
  // liczony z `h`, wiec sam nadaza za wyzszym pasem — a nadazyc musial, bo prog
  // przesunal sie mocno: przy h=21 linie co 1 kW przechodzily do kwMax=7 (21/7=3),
  // przy h=31 przechodza do kwMax=10 (31/10=3). Policzone polozenia dla nowego pasa:
  //   kwMax=10 → 9 linii co 3 px (najgesciej, jakie ten warunek przepuszcza)
  //   kwMax=11 → step 2 kW, 5 linii co 5-6 px (31/11=2, wiec 1 kW odpada)
  // Najgestszy przypadek trafia dokladnie w prog 3 px, czyli w to, co ta regula
  // mowi — nie ma tu nic do poprawiania, jest co potwierdzic.
  const int stepKw = ((h / kwMax) >= 3) ? 1 : 2;
  // Solidne wypelnienie tylko przy TRWAJACEJ sesji I swiezych danych; inaczej —
  // szachownica. `fresh` jest tu razem z gCharging, bo milczaca od 45 s automatyka
  // zostawilaby gCharging na `true` (koniec sesji stwierdzamy dopiero po 10 minutach
  // niskiej mocy, a bez wiadomosci nie ma czego stwierdzac) i wykres rysowalby sie
  // pelnym wypelnieniem, czyli twierdzil "to dzieje sie TERAZ" o danych sprzed
  // kwadransa. Raster mowi wtedy to, co trzeba: "to bylo".
  const bool solid = gCharging && fresh;

  // (v189) PRZESUNIECIE KOLUMNA -> PROBKA. Bufor jest chronologiczny (gGraph[0] =
  // najstarsza), a rysujemy OD PRAWEJ, wiec ostatnia probka ma trafic na x=127:
  //   idx = x + gGraphCnt - kGraphN  →  x = 127 daje idx = gGraphCnt - 1.
  // Przy niepelnym buforze wychodzi ujemne dla lewych kolumn i to wlasnie znaczy
  // "tu jeszcze nie bylo pomiaru" — puste zostaja kolumny PO LEWEJ, nie po prawej.
  const int idxOff = static_cast<int>(gGraphCnt) - kGraphN;

  // (v191) ZERO PROBEK — JEDYNE, CZEGO WTEDY NIE RYSUJEMY, TO SLUPKI. Warunek nie
  // zostal napisany na nowo, tylko PRZESUNIETY z wejscia funkcji dokladnie tam,
  // gdzie od poczatku nalezal: przy gGraphCnt == 0 wychodzi idxOff = -128, wiec
  // `has` jest falszem dla KAZDEJ ze 128 kolumn i petla sama z siebie nie postawi
  // ani jednego slupka — siatka, kreski godzinowe i os jada dalej bez zmiany.
  // `empty` w warunku `has` ponizej jest przez to nadmiarowe i stoi tam swiadomie:
  // jedno slowo trzyma te zaleznosc WIDOCZNA w kodzie, zamiast chowac ja w znaku
  // liczby ujemnej, ktory nastepna przebudowa okna moze przestawic bez ostrzezenia.
  const bool empty = (gGraphCnt == 0);

  for (int x = 0; x < kGraphN; ++x) {
    const int idx = x + idxOff;
    const bool has = !empty && (idx >= 0);
    // Gorna krawedz slupka. Bez probki slupka nie ma i to jest cala reszta okna:
    // sesja krotsza niz 6,4 h zostawia LEWA czesc pasa pusta, zamiast udawac zera.
    const int top = has ? (kGraphY1 - (gGraph[idx] * h) / (kwMax * 10)) : kGraphY1;

    // SIATKA RYSOWANA TYLKO NAD SLUPKIEM, a nie pod spodem i przykrywana. Gdyby szla
    // przez cale pole, przy PRZYGASZONYM wypelnieniu przezierala przez co drugi
    // piksel szachownicy i obie rzeczy zamienialyby sie w jednolita plame.
    for (int k = stepKw; k < kwMax; k += stepKw) {
      const int y = kGraphY1 - (k * h) / kwMax;
      if (y < top && (x & 3) == 0) px(x, y, true);   // kropkowana, co czwarta kolumna
    }
    // Kreska godzinowa co kGraphHourPx kolumn — pionowa, kropkowana, tez tylko nad
    // slupkiem. To ona daje osi czasu podzialke: bez niej "szeroki garb" nie mowi,
    // czy trwal dwadziescia minut, czy dwie godziny.
    // (v189) LICZONA OD PRAWEJ KRAWEDZI, nie od lewej — kreski znacza teraz "godzine
    // temu", "dwie godziny temu" i stoja w tych samych kolumnach zawsze, a przebieg
    // przesuwa sie pod nimi. Liczone od lewej wedrowalyby razem z poczatkiem sesji
    // i podzialka zmienialaby polozenie co trzy minuty, czyli nie bylaby podzialka.
    const int ago = kGraphN - 1 - x;   // ile kolumn (3-minutowek) wstecz od "teraz"
    if (ago != 0 && (ago % kGraphHourPx) == 0) {
      for (int y = kGraphY0; y < top; y += 2) px(x, y, true);
    }

    if (!has) continue;
    for (int y = top; y < kGraphY1; ++y) {
      if (solid || (((x + y) & 1) == 0)) px(x, y, true);
    }
  }

  hline(0, kW - 1, kGraphY1, true);   // linia zera = os czasu

  // (v191) PUSTY PAS MOWI, DLACZEGO JEST PUSTY. Ramka bez slupkow i bez slowa
  // wyjasnienia byla polowa poprawki: wyglada juz jak wykres, ale wciaz nie tlumaczy,
  // czemu nic na niej nie ma — a niewyjasniona pustka czyta sie tak samo jak
  // zniknieta, czyli jako awaria (patrz zasada nad ta funkcja).
  //
  // BRZMIENIE. "brak ładowania" — nie "brak danych", nie "błąd", nie "brak sesji".
  // Stan, ktory opisujemy, jest NORMALNY, a nie zepsuty: auto po prostu nie ladowalo,
  // odkad panel wstal. Zdanie musi byc prawdziwe we wszystkich trzech drogach, ktore
  // tu prowadza, i jest: po restarcie (bufor w PSRAM go nie przezywa), po pierwszym
  // wgraniu firmware'u i po prostu wtedy, gdy od uruchomienia nie bylo ani jednej
  // sesji. "brak danych" sugerowaloby zerwana lacznosc, ktora ma na tym ekranie
  // WLASNY, inny znak (kreski zamiast liczb i zgaszona ikona auta), wiec mowiloby
  // o czyms, co sie nie stalo.
  //
  // NIE ROZMIJA SIE Z RZECZYWISTOSCIA NAWET NA JEDEN OBIEG PETLI, i to nie jest
  // szczescie, tylko wlasciwosc graphTick: pierwsza probka sesji trafia do bufora
  // OD RAZU, bo odstep kGraphStepMs jest pomijany dokladnie przy gGraphCnt == 0.
  // Zaczete ladowanie kasuje wiec ten napis w tym samym obiegu, w ktorym moc
  // przekroczyla kGraphOnKw — nie po trzech minutach.
  //
  // FONT TEN SAM, CO NAPIS SKALI NIZEJ (f10), bo to druga etykieta TEGO SAMEGO pasa,
  // a nie odczyt: wiekszy konkurowalby z wartoscia mocy w f13 wiersz wyzej i czytalby
  // sie jako komunikat calego ekranu, a nie opis wykresu.
  //
  // WYSRODKOWANIE W PIONIE POLICZONE Z METRYK, NIE PRZYMIERZONE. W f10 wersaliki
  // i wydluzenia gorne siegaja 8 px nad linie bazowa (b, d, i, k, ł maja yOffset -8),
  // reszta liter tego napisu (a, r, o, w, n) 5 px, a NIE MA W NIM ANI JEDNEJ litery
  // schodzacej pod linie bazowa — "ą", "ę", "y", "g", "j", "p" tu nie wystepuja,
  // wiec dolna krawedz to baseline-1. Napis zajmuje przez to DOKLADNIE 8 wierszy:
  // baseline-8 .. baseline-1. Przy bazie kGraphY0 + 20 = 52 leza one na y=44..51,
  // czyli zostaje po 12 wolnych wierszy nad napisem (32..43) i pod nim (52..63) —
  // pas 32-wierszowy dzieli sie 12/8/12 co do piksela. Gdyby brzmienie kiedys
  // dostalo ogonek, dolna krawedz zejdzie na baseline+1 i TE liczbe trzeba przeliczyc
  // razem z prostokatem nizej. Wyrazenie z kGraphY0, a nie wpisana liczba 52: pas juz
  // raz urosl (v190, z 42 na 32) i baza ma pojechac razem z nim — tak samo jak baza
  // napisu skali.
  //
  // WYSRODKOWANIE W POZIOMIE ZE STRINGWIDTH, nie z policzonych znakow: napis ma
  // 70 px, wiec stoi na x=29..98. To go rozmija z napisem skali (x=0..18, y=32..40)
  // i tak ma zostac — dwie etykiety tego samego pasa nie moga na siebie wejsc.
  //
  // KASOWANIE PROSTOKATA POD NAPISEM — z tego samego powodu, co przy napisie skali,
  // tylko tu jest ono WARUNKIEM CZYTELNOSCI, a nie ulepszeniem: przy pustym pasie
  // "slupek" ma wysokosc zero, wiec kreski godzinowe ida przez CALA wysokosc pasa
  // (rysujemy je nad slupkiem) i napis lezalby wprost na pionowych liniach
  // kropkowanych. Prostokat obejmuje wiersze 43..52, czyli glify z jednopikselowa
  // obwodka z gory i z dolu, i kolumny 28..99 — kasuje przez to trzy z szesciu kresek
  // godzinowych (x=47, 67, 87) na wysokosci napisu, a kreski z x=7, 27 i 107 zostaja
  // nietkniete. Cena jest tu zerowa: pod spodem nie ma zadnego pomiaru do zaslonienia.
  if (empty) {
    const char* msg = "brak ładowania";
    const int mw = pltxt::stringWidth(plex::f10(), msg);
    const int mx = (kW - mw) / 2;
    const int mb = kGraphY0 + 20;   // linia bazowa napisu — patrz rachunek wyzej
    fillRect(mx - 1, mb - 9, mx + mw, mb, false);
    str(plex::f10(), msg, mx, mb, true);
  }

  // (v189) MAKSIMUM SKALI PRZY LEWEJ KRAWEDZI, NA GORNEJ GRANICY PASA, NA WYCZYSZCZONYM
  // TLE. Baza liter to kGraphY0 + 8 i to nie jest okragla liczba, tylko metryka f10:
  // "k" ma yOffset -8, wiec przy tej bazie jego gorny piksel lezy dokladnie na
  // kGraphY0, czyli NA gornej granicy pasa. O piksel wyzej px() scielaby mu czubek,
  // o piksel nizej napis odklejalby sie od granicy, ktora ma opisywac. Cyfry (yOffset
  // -7) siegaja kGraphY0 + 1 i to jest w porzadku — granice wyznacza litera obok.
  // (v190) BAZA JEST LICZONA Z kGraphY0, WIEC PRZY PODNIESIENIU PASA NA y=32 NIE BYLO
  // TU NIC DO PRZESTAWIANIA: napis pojechal razem z granica na y=32..40, tak jak
  // stal na 42..50. To wlasnie dlatego stoi tu wyrazenie, a nie wpisana liczba 50.
  //
  // KASOWANIE PROSTOKATA POD NAPISEM jest tu WAZNIEJSZE niz w v188, bo od v189 slupki
  // dochodza przy dlugiej sesji az do LEWEJ krawedzi: bez tla napis lezalby wprost na
  // wypelnieniu i na kreskach siatki. Cena: "11kW" (najszerszy przypadek — 11 kW to
  // maksimum ladowarki) ma 24 px, wiec z jednopikselowym odstepem prostokat zjada
  // kolumny 0..24 w wierszach 32..40 — 25 ze 128 kolumn i gorne 9 wierszy pasa,
  // czyli szczyty slupkow z NAJSTARSZYCH 75 minut okna. Typowe "2kW" to 18 px i 19
  // kolumn. (v190) TA CENA SPADLA O POLOWE, choc prostokat ma te same wymiary:
  // przy pasie 22-wierszowym zjadal 9 z 22 wierszy, przy 32-wierszowym zjada 9 z 32.
  // To jest ta sama swiadoma wymiana, co w v188, tylko przeniesiona z rogu,
  // gdzie napis klamal o tym, czego dotyczy: wykres bez podanej skali jest ladnym
  // ksztaltem, a nie pomiarem, a najstarszy skraj okna to najtansze miejsce, jakie
  // przy rysowaniu od prawej w ogole zostalo.
  // (v191) NAPIS SKALI RYSUJE SIE TAKZE PRZY ZERZE PROBEK, Z DOMYSLNYM "1kW" — i to
  // jest wybor miedzy dwoma wariantami, nie przeoczenie po przesunieciu warunku.
  //   * "NIE RYSUJ" ODPADA na tej samej zasadzie, ktora rzadzi cala ta funkcja
  //     (patrz jej poczatek): etykieta pojawiajaca sie i znikajaca razem z pierwsza
  //     probka mrugalaby na POCZATKU I KONCU KAZDEJ SESJI, a mrugniecie czyta sie
  //     jako usterka rysowania, nie jako informacja. Pas mialby wtedy dwa rozne
  //     wyglady pustki — z opisem i bez — czyli dokladnie to, co tu naprawiamy.
  //   * "WARTOSC DOMYSLNA" NICZEGO NIE ZMYSLA, bo 1 kW nie jest wzieta z powietrza:
  //     to PODLOGA SKALI, ktora `kwMax` wymusza tak samo przy KAZDEJ sesji (patrz
  //     zaokraglenie wyzej). Pusty pas jest wiec opisany dokladnie ta sama skala,
  //     jaka dostanie pierwsza slaba sesja — nic sie pod napisem nie przeskaluje,
  //     dopoki probki nie przekrocza 1 kW. Odczytania tego jako "zmierzono do 1 kW"
  //     nie da sie obronic: na srodku pasa stoi wtedy "brak ładowania".
  // Kodu ta decyzja nie kosztuje ani jednej linii — po przesunieciu warunku z wejscia
  // funkcji `kwMax` sam wychodzi 1 przy gGraphMax == 0.
  char s[8];
  snprintf(s, sizeof(s), "%dkW", kwMax);
  const int w = pltxt::stringWidth(plex::f10(), s);
  fillRect(kGraphSclX, kGraphY0, kGraphSclX + w, kGraphY0 + 8, false);
  str(plex::f10(), s, kGraphSclX, kGraphY0 + 8, true);
}

void drawIdle(const AutoModel& a, bool fresh) {
  // (v188) NAPIS "TRYB" ZNIKNAL, a nazwa trybu wskoczyla na jego miejsce. Etykieta
  // nad "TYLKO SŁOŃCE" nie dodawala nic, czego nie widac z samej nazwy — a wiersz,
  // ktory zajmowala, byl na tym ekranie najdrozszym miejscem, jakie mielismy.

  // Ikona auta stoi w rogu, ktorego nazwa trybu nie siega (konczy sie najdalej na
  // x=95, patrz kCarX0). Rysujemy ja WYLACZNIE przy swiezych danych; bez nich nie
  // ma czego pokazac i nie udajemy, ze mamy.
  if (fresh) drawCarIcon(a.bleLink);

  const int act = fresh ? autoModeIndex(a.mode) : -1;
  str(plex::f13(), fresh ? autoModeLabel(act) : "brak danych", kMarginX, kIdleModeY, true);

  hline(0, kW - 1, kIdleRuleY, true);

  // (v190) JEDEN WIERSZ ZAMIAST DWOCH. Do v189 stal tu wiersz etykiet ("MOC" po
  // lewej, "BATERIA" od x=74) i pod nim wiersz wartosci w f11. Etykiety wylecialy
  // w calosci: "kW" i "%" stoja przy samych liczbach i mowia to samo, a na dwa metry
  // WIEKSZY FONT WAZY WIECEJ NIZ PODPIS. Wartosci przeszly z f11 na f13 — i to nie
  // kosztuje ani piksela szerokosci, bo w tych tablicach f13 jest dla liczb WEZSZY
  // od f11 mimo wiekszej wysokosci ("11,0 kW" to 45 px zamiast 47; pelny pomiar przy
  // kIdleValY). Zwolniony wiersz poszedl na wykres: pas urosl z 22 na 32 px.
  //
  // TRZY PUNKTY ZACZEPIENIA, nie dwie kolumny: moc do LEWEJ krawedzi, ikona zrodla
  // POSRODKU, procent do PRAWEJ. Kolumny mialy sens przy etykietach, ktore wyznaczaly
  // im lewe krawedzie; bez nich jedynymi stalymi punktami tego wiersza sa krawedzie
  // ekranu i jego srodek — i tylko na nich liczby stoja tak samo przy kazdej wartosci.
  char num[12];
  char val[16];
  if (fresh) {
    fmt1(num, sizeof(num), a.kw);   // polski przecinek dziesietny — Format.h
    snprintf(val, sizeof(val), "%s kW", num);
  } else {
    snprintf(val, sizeof(val), "-");
  }
  str(plex::f13(), val, kMarginX, kIdleValY, true);

  if (fresh) {
    snprintf(val, sizeof(val), "%u %%", static_cast<unsigned>(a.soc));
  } else {
    snprintf(val, sizeof(val), "-");
  }
  // Kreska przy braku danych tez idzie w prawo, na te sama krawedz, co liczba.
  // Wyjatek "bez danych rysujemy od lewej" kazalby jej skakac przez pol ekranu
  // w chwili, w ktorej automatyka milknie — czyli mowilby o polozeniu, a nie o
  // danych. Ta sama zasada, co przy kolumnie MOC: kreska zastepuje liczbe w jej
  // wlasnym miejscu.
  strRight(plex::f13(), val, kIdleRightX, kIdleValY, true);

  // (v188) IKONY ZRODLA obok wartosci mocy — (v190) blok x=54..73, czyli srodek
  // ekranu; srodek pionowy kSrcCy = 24, na wysokosci cyfr.
  // Zadna, jedna albo obie; pelne uzasadnienie progow stoi przy srcIcons().
  const uint8_t src = srcIcons(a, fresh);
  if ((src & kSrcSun) != 0) drawSunIcon(kSrcSunCx, kSrcCy);
  if ((src & kSrcPlug) != 0) drawPlugIcon(kSrcPlugX, kSrcCy);

  // (v188) WYKRES ZOSTAJE TAKZE PRZY BRAKU SWIEZYCH DANYCH — i to nie jest
  // niekonsekwencja wobec ikony auta, ktora wtedy znika. Ikona mowi o STANIE TERAZ
  // ("polecenie przejdzie"), wiec bez swiezych danych klamalaby. Wykres mowi o
  // PRZESZLOSCI ("tak szla ostatnia sesja") — a to, ze automatyka zamilkla, nie
  // uniewaznia pomiarow sprzed godziny. Rysuje sie za to rastrem (patrz drawGraph),
  // czyli tym samym znakiem, co po zakonczeniu sesji: "to bylo, a nie jest".
  drawGraph(fresh);
}

// KURSOR I KROPKA TO DWIE ROZNE RZECZY i musza dac sie rozroznic TAKZE wtedy, gdy
// kursor stoi na trybie aktywnym. Dlatego:
//   * KURSOR to inwersja CALEGO wiersza (biale tlo, czarny tekst) — mowi "tu jestes",
//   * KROPKA po prawej mowi "to jest tryb, ktory auto ma TERAZ" i jest rysowana
//     kolorem PRZECIWNYM do tla swojego wiersza. Na wierszu podswietlonym jest wiec
//     czarna, na zwyklym biala — widoczna w obu przypadkach, nigdy nie znika.
// Kropka pochodzi WYLACZNIE z pola `tryb` w auto/stan; wybor wlasciciela jej nie
// przesuwa i nie ma prawa przesunac, dopoki automatyka nie potwierdzi.
void drawMenu(const AutoModel& a, bool fresh) {
  const char* title = "WYBIERZ TRYB";
  if (gMsg == MSG_SENDING) title = "wysyłam do HA...";
  else if (gMsg == MSG_NOACK) title = "brak potwierdzenia";
  else if (gMsg == MSG_FAILED) title = "nie wysłano";
  str(plex::f10(), title, kMarginX, kIdleTagY, true);

  const int act = fresh ? autoModeIndex(a.mode) : -1;
  for (int i = 0; i < kAutoModeCount; ++i) {
    const int top = kRow0Y + i * kRowH;
    const bool sel = (i == static_cast<int>(gCursor));
    if (sel) fillRect(0, top, kW - 1, top + kRowH - 1, true);
    // +11 = wysokosc wersalika w f11; wiersz ma 13 px, wiec litery stoja w nim
    // z jednopikselowym marginesem u gory i u dolu.
    str(plex::f11(), autoModeLabel(i), kMenuTextDX, top + 11, !sel);
    if (i == act) disc(kMenuDotX, top + kRowH / 2, 2, !sel);
  }
}

// Diagnostyka mapowania przyciskow. (v178) Wchodzi sie tu cfg::BTN_OK i cfg::BTN_BACK
// trzymanymi RAZEM przez cfg::OLED_TEST_HOLD_MS, a wychodzi KAZDYM krotkim
// nacisnieciem i puszczeniem — uzasadnienie obu polowek stoi przy pollButtons().
//
// INSTRUKCJA WYJSCIA MUSI BYC NA EKRANIE i to nie jest ozdoba. Przyciski w tescie
// celowo nic nie wybieraja, wiec bez tej linijki ekran wyglada dokladnie jak
// zawieszony panel — i tak wlasnie zostal odczytany przez wlasciciela.
//
// TYTUL SKROCONY Z "TEST PRZYCISKÓW" DO "TEST" I TO JEST SWIADOMY UBYTEK, nie
// niedopatrzenie. Pomiar w metrykach f10: "TEST PRZYCISKÓW" ma 86 px, a
// "naciśnij, aby wyjść" 82 px — na jednej linii (122 px uzytecznych) nie mieszcza
// sie razem, a pionowo nie ma juz ANI JEDNEGO wolnego wiersza: naglowek siedzi na
// linii bazowej 8, cztery wiersze po kRowH koncza sie ogonkiem "Ę" na y=62, a ekran
// ma 64. Zmieszczenie pietej linii wymagaloby scisniecia wierszy do 11 px, czyli
// zejscia z siatki wspolnej z menu i zerowych odstepow miedzy akcentami. Wiersze
// mowia same za siebie ("K1  GPIO18  WCIŚNIĘTY"), wiec slowo "PRZYCISKÓW" jest tu
// mniej warte niz zdanie o tym, jak stad wyjsc.
void drawTest() {
  str(plex::f10(), "TEST", kMarginX, kIdleTagY, true);
  // Ta sama prawa krawedz, co kolumna stanu ponizej — naglowek nie rusza siatki
  // kolumn, tylko dosiada sie do wolnego miejsca po prawej (koniec "TEST" na x=25,
  // poczatek napisu na x=43, czyli 18 px przerwy).
  strRight(plex::f10(), "naciśnij, aby wyjść", kTestRightX, kIdleTagY, true);
  for (int i = 0; i < 4; ++i) {
    const int base = kRow0Y + i * kRowH + 10;
    char b[8];
    snprintf(b, sizeof(b), "K%d", i + 1);
    str(plex::f10(), b, kMarginX, base, true);
    snprintf(b, sizeof(b), "GPIO%d", kPins[i]);
    str(plex::f10(), b, kTestGpioX, base, true);
    // U+2212 (MINUS SIGN) — jest w tablicy kodow fontow Plex; U+2014 (myslnik)
    // NIE JEST i zniknalby po cichu.
    strRight(plex::f10(), ((gDown & (1u << i)) != 0) ? "WCIŚNIĘTY" : "−", kTestRightX,
             base, true);
  }
}

// ====================== (v187) MENU USTAWIEN — MODEL ========================
// Wszystko ponizej liczy sie NA MIEJSCU, z settings() i z kViewNames. Zadna z tych
// funkcji nie ma wlasnej tablicy ani wlasnej kopii wartosci — bo kazda taka kopia
// musialaby byc odswiezana i predzej czy pozniej rozjechalaby sie ze zrodlem.

// Przypiety widok wg glownego ekranu; -1 = rotacja automatyczna albo brak spiecia.
int pinnedView() { return (gPinnedFn != nullptr) ? gPinnedFn() : -1; }

// Czy slot `v` jest ZYWYM widokiem. Sloty 0 i 2 sa wycofane (RETRO/GODZINY, v162)
// i niosa w kViewNames "—" — to JEST ich znacznik i innego nie ma po co wymyslac.
// Numeru widoku nie odgadujemy tu z niczego innego: gdy kiedys wroca albo dojdzie
// nowy ekran, ta lista zmieni sie w jednym miejscu i panel pojdzie za nia sam.
bool viewLive(int v) { return strcmp(kViewNames[v], "—") != 0; }

// Wiersz 0 to "auto (rotacja)", czyli ZDJECIE przypiecia; dalej ida zywe widoki.
int viewRows() {
  int n = 1;
  for (int v = 0; v < cfg::VIEW_COUNT; ++v) {
    if (viewLive(v)) ++n;
  }
  return n;
}

// Numer widoku dla wiersza listy. Wiersz 0 -> -1, czyli dokladnie ta wartosc,
// ktora WeatherUi::pinView() rozumie jako "wroc do rotacji".
int viewForRow(int row) {
  if (row <= 0) return -1;
  int n = 0;
  for (int v = 0; v < cfg::VIEW_COUNT; ++v) {
    if (!viewLive(v)) continue;
    if (++n == row) return v;
  }
  return -1;
}

// Odwrotnosc viewForRow() — zeby wejscie w liste stawalo OD RAZU na tym, co jest
// teraz na duzym ekranie, a nie na poczatku listy.
int rowForView(int view) {
  int n = 0;
  for (int v = 0; v < cfg::VIEW_COUNT; ++v) {
    if (!viewLive(v)) continue;
    ++n;
    if (v == view) return n;
  }
  return 0;
}

// Przewijanie: kursor ma byc widoczny, okno przesuwa sie o tyle, ile trzeba i ani
// piksela wiecej. Bez wysrodkowywania — przy czterech wierszach skakaloby ono
// okno o dwa wiersze na kazde nacisniecie i lista czytalaby sie jak losowa.
void scrollIntoView() {
  if (gRowCur < gTop) gTop = gRowCur;
  if (gRowCur >= gTop + kSetVisible) gTop = static_cast<uint8_t>(gRowCur - (kSetVisible - 1));
}

// Ile wierszy ma podekran WARTOSCI (jasnosc ma trzy, pozostale po dwa).
uint8_t valRows(uint8_t scr) { return (scr == SCR_BRI) ? 3 : 2; }

// --- WARTOSCI: jedno okienko na settings(), zamiast siedmiu galezi w kazdym miejscu -
// Menu nie trzyma kopii ustawien, tylko siega po nie przez te cztery funkcje. Dzieki
// temu rysowanie, edycja, cofanie i podpis tresci widza DOKLADNIE to samo pole i nie
// da sie ich rozjechac przy dopisaniu kolejnego wiersza.
uint16_t valGet(uint8_t scr, uint8_t row) {
  const Settings& s = settings();
  switch (scr) {
    case SCR_BRI:   return (row == 0) ? s.blDay : ((row == 1) ? s.blDim : s.blNight);
    case SCR_ROT:   return (row == 0) ? (s.autoRotate ? 1u : 0u) : s.dwellS;
    case SCR_NIGHT: return (row == 0) ? s.nightStartH : s.nightEndH;
    default:        return 0;
  }
}

void valSet(uint8_t scr, uint8_t row, uint16_t v) {
  Settings& s = settings();
  switch (scr) {
    case SCR_BRI:
      if (row == 0) s.blDay = static_cast<uint8_t>(v);
      else if (row == 1) s.blDim = static_cast<uint8_t>(v);
      else s.blNight = static_cast<uint8_t>(v);
      break;
    case SCR_ROT:
      if (row == 0) s.autoRotate = (v != 0);
      else s.dwellS = v;
      break;
    case SCR_NIGHT:
      if (row == 0) s.nightStartH = static_cast<uint8_t>(v);
      else s.nightEndH = static_cast<uint8_t>(v);
      break;
    default: break;
  }
}

// ZAKRESY BIERZEMY Z Settings, A NIE PISZEMY ICH TU DRUGI RAZ. Twarde minima
// jasnosci istnieja po to, zeby z ekranu w lazience dalo sie wrocic — panel nie ma
// prawa ich obchodzic, a gdyby mial wlasne liczby, obszedlby je przy pierwszej
// zmianie po tamtej stronie. Settings::clampTuning() i tak przycina wszystko przy
// zapisie; te dwie funkcje sprawiaja tylko, ze wlasciciel NIE WIDZI wartosci,
// ktorej i tak nie dostanie.
uint16_t valMin(uint8_t scr, uint8_t row) {
  switch (scr) {
    case SCR_BRI:
      return (row == 0) ? Settings::BL_DAY_MIN
                        : ((row == 1) ? Settings::BL_DIM_MIN : Settings::BL_NIGHT_MIN);
    case SCR_ROT: return (row == 0) ? 0 : Settings::DWELL_MIN;
    default:      return 0;   // godziny
  }
}

uint16_t valMax(uint8_t scr, uint8_t row) {
  switch (scr) {
    case SCR_BRI: return 255;
    case SCR_ROT: return (row == 0) ? 1 : Settings::DWELL_MAX;
    default:      return 23;  // godziny
  }
}

// KROK JASNOSCI TO 5, A NIE 1, i to jest decyzja o CZASIE WLASCICIELA, nie o
// precyzji: roznicy jednego stopnia z 255 nie widzi zadne oko, a przejechanie
// zakresu 60..255 krokiem 1 to 195 nacisniec. Krokiem 5 sa 39, a wartosci dziela
// sie rowno (195, 225 i 240 sa podzielne przez 5), wiec oba konce zakresu daja sie
// trafic dokladnie. Reszta pol idzie krokiem 1 — tam kazda jednostka cos znaczy.
uint16_t valStep(uint8_t scr) { return (scr == SCR_BRI) ? 5 : 1; }

// Napis wartosci. Jednostka JEST czescia napisu ("9 s", "22:00"), bo sama liczba
// w kolumnie obok slowa "Koniec" nie mowi, czy to godzina, czy minuty.
void valText(uint8_t scr, uint8_t row, char* out, size_t n) {
  const unsigned v = valGet(scr, row);
  switch (scr) {
    case SCR_BRI:   snprintf(out, n, "%u", v); break;
    case SCR_ROT:
      if (row == 0) snprintf(out, n, "%s", (v != 0) ? "tak" : "nie");
      else snprintf(out, n, "%u s", v);
      break;
    case SCR_NIGHT: snprintf(out, n, "%u:00", v); break;
    default:        snprintf(out, n, "-"); break;
  }
}

// Etykiety — switch, a nie tablica wskaznikow: tablica `static const char* const`
// to cztery wskazniki, ktore moga wyladowac w statycznym RAM-ie, a switch nie
// kosztuje ani bajta danych. Ta sama zasada rzadzi calym tym plikiem (patrz kInit
// i celowy brak bitmapy ikony auta).
const char* setItem(int i) {
  switch (i) {
    case 0:  return "Ekran";
    case 1:  return "Jasność";
    case 2:  return "Rotacja";
    default: return "Noc";
  }
}

const char* valLabel(uint8_t scr, uint8_t row) {
  switch (scr) {
    // Te trzy slowa sa TE SAME, ktorymi petla glowna opisuje poziomy LDR w dzienniku
    // ("ciemno" / "polmrok" / "swiatlo", patrz pogoda-gdynia.ino) — wlasciciel czyta
    // jedno i drugie, wiec nie moga sie nazywac inaczej.
    case SCR_BRI:   return (row == 0) ? "Światło" : ((row == 1) ? "Półmrok" : "Ciemno");
    case SCR_ROT:   return (row == 0) ? "Auto-rotacja" : "Czas ekranu";
    case SCR_NIGHT: return (row == 0) ? "Początek" : "Koniec";
    default:        return "";
  }
}

const char* setTitle(uint8_t scr) {
  switch (scr) {
    case SCR_VIEW:  return "EKRAN";
    case SCR_BRI:   return "JASNOŚĆ";
    case SCR_ROT:   return "ROTACJA";
    case SCR_NIGHT: return "NOC";
    default:        return "USTAWIENIA";
  }
}

// ====================== (v187) MENU USTAWIEN — RYSOWANIE =====================

// Naglowek podekranu. W EDYCJI TYTUL USTEPUJE MIEJSCA INSTRUKCJI i to nie jest
// ozdoba: to jedyna chwila, w ktorej dwa te same przyciski robia co innego niz
// przed sekunda, a wlasciciel patrzy wtedy na DUZY ekran (podglad jasnosci), nie
// na panel. Wracajac wzrokiem musi od razu wiedziec, ze zmiana jeszcze nie jest
// przyjeta. Tytul i tak stoi na ekranie, z ktorego przed chwila tu wszedl.
void drawSetHeader(uint8_t scr) {
  str(plex::f10(), gEditing ? "* zapisz   # anuluj" : setTitle(scr), kMarginX, kSetHdrY,
      true);
}

// Wiersz "etykieta ......... wartosc". `edit` doklada ramke wokol wartosci.
void drawValRow(int i, const char* label, const char* value, bool sel, bool edit) {
  const int top = kRow0Y + i * kRowH;
  if (sel) fillRect(0, top, kW - 1, top + kRowH - 1, true);
  // +11 = wysokosc wersalika w f11, tak samo jak w menu trybu.
  str(plex::f11(), label, kMenuTextDX, top + 11, !sel);
  const int x = kSetValRight - pltxt::stringWidth(plex::f11(), value);
  str(plex::f11(), value, x, top + 11, !sel);
  if (edit) frame(x - 3, top + 1, kSetValRight + 2, top + kRowH - 2, !sel);
}

void drawSettings() {
  str(plex::f10(), "USTAWIENIA", kMarginX, kSetHdrY, true);
  for (int i = 0; i < 4; ++i) {
    const int top = kRow0Y + i * kRowH;
    const bool sel = (i == static_cast<int>(gSetCur));
    if (sel) fillRect(0, top, kW - 1, top + kRowH - 1, true);
    str(plex::f11(), setItem(i), kMenuTextDX, top + 11, !sel);
  }
}

// PILOT DO DUZEGO WYSWIETLACZA. Lista jest dluzsza od ekranu (czternascie pozycji
// na cztery wiersze), wiec naglowek niesie licznik "n/m" — bez niego przewijanie
// po ciemku wyglada jak lista bez konca. KROPKA znaczy tu DOKLADNIE to samo, co
// w menu trybu: "to jest stan, ktory obowiazuje TERAZ" — czyli przypiety widok
// albo, gdy nic nie jest przypiete, wiersz "auto (rotacja)".
void drawViewList() {
  const int rows = viewRows();
  str(plex::f10(), "EKRAN", kMarginX, kSetHdrY, true);
  char hdr[12];
  snprintf(hdr, sizeof(hdr), "%d/%d", static_cast<int>(gRowCur) + 1, rows);
  strRight(plex::f10(), hdr, kTestRightX, kSetHdrY, true);

  const int pin = pinnedView();
  for (int k = 0; k < kSetVisible; ++k) {
    const int r = gTop + k;
    if (r >= rows) break;
    const int top = kRow0Y + k * kRowH;
    const bool sel = (r == static_cast<int>(gRowCur));
    if (sel) fillRect(0, top, kW - 1, top + kRowH - 1, true);
    const int v = viewForRow(r);
    str(plex::f11(), (r == 0) ? "auto (rotacja)" : kViewNames[v], kMenuTextDX, top + 11,
        !sel);
    if ((r == 0) ? (pin < 0) : (v == pin)) disc(kMenuDotX, top + kRowH / 2, 2, !sel);
  }
}

void drawValues(uint8_t scr) {
  drawSetHeader(scr);
  const uint8_t rows = valRows(scr);
  char v[12];
  for (uint8_t i = 0; i < rows; ++i) {
    valText(scr, i, v, sizeof(v));
    drawValRow(i, valLabel(scr, i), v, i == gRowCur, (i == gRowCur) && gEditing);
  }
}

// ============ (v188) ZBIERANIE PROBEK DO WYKRESU MOCY ========================
// Wolane RAZ NA OBIEG step(), niezaleznie od tego, ktory ekran jest na szkle: dane
// maja sie zbierac takze wtedy, gdy wlasciciel grzebie w ustawieniach albo panel
// stoi na menu. Inaczej wykres mialby dziure dokladnie w tych minutach, w ktorych
// ktos przy nim byl.
//
// GDZIE SIE ZACZYNA SESJA — I DLACZEGO NIE NA "stan == laduje". Pole `stan` mowi,
// co o sobie sadzi integracja Tesli, i potrafi stac na "laduje" przez cala noc przy
// zerowej mocy albo przeskoczyc na "czeka" na jedna wiadomosc w srodku ladowania.
// Wykres jest o MOCY, wiec i sesje wyznacza moc: przekroczenie kGraphOnKw po co
// najmniej kGraphGapMs ciszy ponizej tego progu. Dziesiec minut jest tu progiem
// osobnym od trzech minut probkowania i to jest celowe — chmura potrafi sciac
// nadwyzke do zera na kwadrans, ale takie zalamanie ma zostac WIDOCZNE W SRODKU
// wykresu jako dolina, a nie skasowac dotychczasowy przebieg i zaczac od nowa.
void graphTick(const AutoModel& a, uint32_t now, bool fresh) {
  if (gGraph == nullptr || !fresh) return;

  // (v194) DOKONCZENIE ODTWORZENIA Z NVS — pierwszy przebieg, w ktorym zegar jest juz
  // wazny. Prog 1700000000 (listopad 2023) to ten sam sprawdzian "NTP doszedl", co
  // w AirHistory::advance() — przed synchronizacja time() oddaje rok 1970.
  //
  // ROZSTRZYGAMY JEDNO PYTANIE: czy sesja sprzed restartu trwa dalej. Miara jest
  // kGraphGapMs, czyli TA SAMA przerwa (10 min), ktora w normalnej pracy oddziela
  // dwie sesje — a nie nowy prog wymyslony na te okazje. To wazne: gdyby przerwa
  // "po restarcie" miala inna miare niz przerwa "w trakcie pracy", wykres zachowywalby
  // sie inaczej po aktualizacji niz po chmurze, przy tej samej dlugosci ciszy.
  //
  //   przerwa < 10 min  -> sesja TRWA. Cofamy gGraphHighMs i gGraphSampMs o dlugosc
  //                        przerwy, dzieki czemu idleLong nizej wypada FALSZ i bufor
  //                        NIE JEST kasowany, a nastepna probka trafia w rytm.
  //                        Odjecie ponizej zera jest bezpieczne: wszystkie porownania
  //                        czasu ida przez roznice na int32_t (patrz nizej), wiec
  //                        liczy sie wylacznie ODSTEP, a nie sama wartosc.
  //   przerwa >= 10 min -> sesja SIE SKONCZYLA. Probki zostaja NA EKRANIE (to wciaz
  //                        prawdziwy, zamkniety przebieg i lepszy niz pusty pas), ale
  //                        gCharging = false, wiec najblizsze ladowanie zacznie bufor
  //                        od nowa — czyli dokladnie tak, jak gdyby restartu nie bylo.
  //
  // Doklejanie nowych probek do przebiegu sprzed wielu godzin byloby jedynym
  // nieuczciwym wyjsciem: os czasu nie ma jak pokazac dziury, wiec przerwa
  // zniknelaby, a dwa odlegle ladowania zlalyby sie w jedno ciagle.
  if (gRestorePending) {
    const uint32_t epoch = static_cast<uint32_t>(time(nullptr));
    if (epoch >= 1700000000UL) {
      gRestorePending = false;
      const bool sane = (gRestoreEpoch >= 1700000000UL) && (epoch >= gRestoreEpoch);
      // (P1-1) PRZYCIECIE PRZED *1000: gapS to sekundy (epoch), a gapMs ma byc w
      // milisekundach na uint32_t. Przy przerwie > 49,7 dnia (0xFFFFFFFF / 1000
      // sekund) samo mnozenie przepelnia i moglo dac MALA liczbe przez przypadek —
      // czyli "sesja trwa dalej" po miesiacach wylaczenia. Przycinamy do
      // kMaxGapS PRZED mnozeniem: kazdy gapS powyzej tego progu i tak jest kilkaset
      // razy wiekszy niz kGraphGapMs (10 min), wiec zaokraglenie w gore do
      // 0xFFFFFFFF nie zmienia wyniku ponizszego porownania.
      constexpr uint32_t kMaxGapS = 0xFFFFFFFFUL / 1000UL;
      const uint32_t gapS = sane ? (epoch - gRestoreEpoch) : 0;
      const uint32_t gapMs = !sane ? 0xFFFFFFFFUL
                             : (gapS > kMaxGapS) ? 0xFFFFFFFFUL
                                                  : gapS * 1000UL;
      if (gRestoreCharging && gapMs < kGraphGapMs) {
        gCharging = true;
        gGraphHighMs = now - gapMs;
        if (gGraphHighMs == 0) gGraphHighMs = 1;   // 0 znaczy "nigdy"
        gGraphSampMs = now - gapMs;
        LOG("OLED: wykres — sesja trwa dalej (przerwa %lu s)\n",
            static_cast<unsigned long>(gapMs / 1000UL));
      } else {
        gCharging = false;
        LOG("OLED: wykres — sesja zamknieta, %u probek zostaje do pokazania\n",
            static_cast<unsigned>(gGraphCnt));
      }
    }
  }

  const bool high = (a.kw >= kGraphOnKw);
  // Roznice czasu ZAWSZE przez int32_t — millis() przekreca sie po ~49 dniach,
  // a ten panel ma chodzic miesiacami bez restartu (ta sama zasada, co przy atMs).
  // (P1-1) Liczone przez isFresh() z Freshness.h (ten sam wzor co w step() wyzej i
  // w WeatherUiV3.cpp) — "przerwa dluga" to po prostu NIE "swiezo", wiec negacja.
  const bool idleLong = !isFresh(now, gGraphHighMs, kGraphGapMs);

  if (high) {
    if (!gCharging && idleLong) {   // POCZATEK NOWEJ SESJI — czyscimy bufor
      gGraphCnt = 0;
      gGraphMax = 0;
    }
    gCharging = true;
    gGraphHighMs = now;
    if (gGraphHighMs == 0) gGraphHighMs = 1;   // 0 znaczy "nigdy", patrz wyzej
  } else if (idleLong) {
    gCharging = false;   // bufor ZOSTAJE — od tej chwili rysuje sie rastrem
  }

  if (!gCharging) return;
  // (P1-1) isFresh() z Freshness.h — patrz komentarz przy idleLong wyzej. gGraphSampMs
  // realnie nie jest nigdy 0, gdy gGraphCnt != 0 (ustawiane razem, patrz nizej), wiec
  // szczegolny przypadek "0 = nigdy" w isFresh() jest tu wylacznie bezpiecznikiem.
  if (gGraphCnt != 0 && isFresh(now, gGraphSampMs, kGraphStepMs)) {
    return;
  }

  // Zaokraglenie do 0,1 kW, czyli do rozdzielczosci napisu nad wykresem. Ujemna moc
  // (oddawanie z auta) idzie jako 0: os zaczyna sie na zerze i slupek w dol nie mialby
  // gdzie sie zmiescic, a takie chwile trwaja pojedyncze sekundy.
  int v = static_cast<int>(a.kw * 10.f + 0.5f);
  if (v < 0) v = 0;
  if (v > 255) v = 255;

  // (v189) BUFOR PELNY = PRZEWIJAMY, A NIE ZATRZYMUJEMY SIE. To ODWROCENIE decyzji
  // z v188 i powod jest jeden: od v189 rysujemy OD PRAWEJ, gdzie prawa krawedz to
  // zawsze "teraz". Zatrzymanie dopisywania po 6,4 h znaczyloby, ze prawa krawedz
  // zastyga na chwili sprzed godzin, a wykres cichcem przestaje pokazywac biezaca
  // moc — czyli klamie, i to w miejscu, w ktore patrzy sie najpierw. Przy rysowaniu
  // od lewej (v188) ta sama decyzja byla dobra: tam zamarzal PRAWY, pusty skraj,
  // a chroniony byl poczatek sesji.
  //
  // CENA JEST ZNANA I PRZYJETA: najstarsza probka wypada, wiec sesja dluzsza niz
  // 6,4 h traci swoj rozbieg. Okno jedzie za biezaca chwila i pokazuje OSTATNIE
  // 6,4 h zamiast pierwszych. Gdyby ktos chcial wrocic do v188, wystarczy w tym
  // miejscu `if (gGraphCnt >= kGraphN) return;` — ale wtedy trzeba wrocic TAKZE do
  // rysowania od lewej, inaczej wyjdzie zamarznieta prawa krawedz opisana wyzej.
  //
  // memmove po 128 B PSRAM-u wypada RAZ NA TRZY MINUTY i tylko przy pelnym buforze —
  // przy zegarze 240 MHz to czas nie do zmierzenia w tej petli.
  if (gGraphCnt >= kGraphN) {
    memmove(gGraph, gGraph + 1, kGraphN - 1);
    gGraphCnt = kGraphN - 1;
  }

  gGraph[gGraphCnt++] = static_cast<uint8_t>(v);
  // gGraphMax ZOSTAJE MAKSIMUM CALEJ SESJI, takze gdy szczyt wyjechal juz poza lewa
  // krawedz. Przeliczanie go po przewinieciu (skan 128 B) byloby tanie, ale skala
  // skakalaby wtedy w chwili, w ktorej z okna wypada pojedyncza probka — caly
  // przebieg podskoczylby na wysokosc bez zadnej zmiany w mocy. Stala skala sesji
  // czyta sie lepiej i to jest ta sama zasada, co przy zaokraglaniu w gore do
  // pelnych kW: skala ma byc odniesieniem, a nie kolejnym ruchomym elementem.
  if (static_cast<uint8_t>(v) > gGraphMax) gGraphMax = static_cast<uint8_t>(v);
  ++gGraphSeq;   // (v189) patrz deklaracja: BEZ TEGO wykres zamarza po 6,4 h
  gGraphSampMs = now;

  // (v194) UTRWALENIE PO KAZDEJ DOPISANEJ PROBCE. Zapis siedzi TUTAJ, a nie w
  // osobnym timerze, i to jest cala jego prostota: probka powstaje najwyzej raz na
  // 3 minuty i tylko w trakcie sesji, wiec kadencja zapisu wychodzi z natury danych,
  // a nie z drugiego, niezaleznego zegara, ktory trzeba by z tym pierwszym godzic.
  // Poza ladowaniem probki nie powstaja w ogole, wiec i zapisow nie ma — realnie to
  // kilkadziesiat na dobe, nie 480.
  //
  // ZNACZNIK CZASU JEST ZEGAROWY (epoch), NIE millis(). Cala reszta tego pliku mierzy
  // czas od startu, bo interesuja ja ODSTEPY w obrebie jednej sesji pracy — ale ten
  // jeden znacznik ma przezyc restart, po ktorym millis() rusza od zera. Zapisanie tu
  // millis() dalo by liczbe nie do porownania z niczym po drugiej stronie restartu.
  // Gdy NTP jeszcze nie doszedl, idzie 0 i blob sam sie do tego przyzna (GraphBlob.h).
  GraphBlob b;
  b.cnt = gGraphCnt;
  b.max = gGraphMax;
  b.charging = gCharging ? 1 : 0;
  const uint32_t epoch = static_cast<uint32_t>(time(nullptr));
  b.lastEpoch = (epoch >= 1700000000UL) ? epoch : 0;
  memcpy(b.s, gGraph, kGraphN);
  graphBlobSave(b);
}

// ====================== PODPIS TRESCI (kiedy przerysowac) ====================
// Pelna klatka to 1 kB po I2C, czyli okolo 25 ms. Gdyby leciala co obieg, panel
// zabieralby polowe kazdej klatki glownego ekranu — dlatego przerysowujemy
// WYLACZNIE po zmianie tresci, a nie po uplywie czasu.
//
// Podpis liczymy z tego, CO WIDAC, a nie z calego modelu: moc trafia do niego
// zaokraglona do 0,1 kW, czyli do rozdzielczosci napisu na ekranie. Bez tego
// zaokraglenia szum ostatniego bitu float-a wywolywalby pelna klatke co 15 s przy
// kazdej wiadomosci z Home Assistanta, mimo ze na szkle nic by sie nie zmienilo.
uint32_t signature(const AutoModel& a, bool fresh) {
  uint32_t h = 2166136261u;  // FNV-1a, 32 bity
  auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };

  mix(gScr);
  if (gScr == SCR_TEST) {
    mix(gDown);   // ekran testu pokazuje WYLACZNIE stan przyciskow
    return h;
  }

  // (v187) USTAWIENIA MAJA WLASNY PODPIS I TO JEST WARUNEK ICH DZIALANIA, nie
  // optymalizacja. Regula tego panelu brzmi: KAZDY stan widoczny na szkle musi
  // wejsc do podpisu, inaczej ekran zamarza az do najblizszej zmiany czegos innego
  // (dokladnie tak zachowala sie ikona auta przed poprawka z v186, patrz bleLink
  // nizej). Tu widac kursory, flage edycji i SIEDEM POL settings() — a ostatnie
  // zmieniaja sie takze SPOZA panelu, z formularza WWW, wiec bez nich menu
  // pokazywaloby liczbe, ktorej juz nie ma.
  //
  // Osobna galaz, a nie dopisanie pol do wspolnej sciezki: na ekranach ustawien nie
  // widac ANI JEDNEGO pola z AutoModelu, wiec mieszanie ich tutaj kazaloby skladac
  // pelna klatke (osiem stron, ~25 ms) przy kazdej wiadomosci z Home Assistanta —
  // co 15 s, przez caly czas, gdy wlasciciel czyta menu.
  if (inSettings()) {
    mix(gSetCur);
    mix(gRowCur);
    mix(gTop);
    mix(gEditing ? 1u : 0u);
    const Settings& s = settings();
    mix(s.nightStartH);
    mix(s.nightEndH);
    mix(s.dwellS);
    mix(s.blDay);
    mix(s.blDim);
    mix(s.blNight);
    mix(s.autoRotate ? 1u : 0u);
    // Przypiecie zmienia sie takze dotykiem i z panelu WWW, a widac je jako kropke
    // na liscie widokow. +1, zeby "brak przypiecia" (-1) nie wchodzilo do mieszania
    // jako 0xFFFFFFFF razem z sasiednim polem — czytelnosc, nie poprawnosc.
    mix(static_cast<uint32_t>(pinnedView() + 1));
    return h;
  }

  // (v190) PRZEGLAD PODPISU PO SKASOWANIU WIERSZA ETYKIET — WYNIK: BEZ ZMIAN, i to
  // jest wniosek z rachunku, a nie z przeoczenia. Regula tego pliku dziala w obie
  // strony (patrz opis nad ta funkcja), wiec po kazdej przebudowie ekranu trzeba
  // sprawdzic OBIE: czy nic widocznego nie wypadlo i czy nic martwego nie zostalo.
  //   * NIC NIE WYPADLO. "MOC" i "BATERIA" byly LITERALAMI — tekstem wpisanym
  //     w kod, ktory nie zalezal od zadnego pola modelu — wiec nigdy nie mialy
  //     wlasnego wkladu do podpisu i nie ma stad czego usuwac. Tak samo font
  //     (f11 → f13) i wspolrzedne: to stale kompilacji, ktore zmieniaja sie razem
  //     z obrazem tylko przez nowy wsad, a nie w trakcie pracy.
  //   * NIC NIE DOSZLO. Wiersz wartosci pokazuje DOKLADNIE te same dwie liczby,
  //     co przedtem — a.kw (nizej, zaokraglone do 0,1 kW) i a.soc — tylko wiekszym
  //     fontem i w innych miejscach. Wyrownanie procentu w prawo tez nic nie wnosi:
  //     polozenie napisu liczy sie z jego szerokosci, czyli z tej samej wartosci.
  //   * WYKRES UROSL, ALE NIE ZMIENIL ZRODEL. Wyzszy pas (y=32..63 zamiast 42..63)
  //     przelicza slupki i siatke z tych samych czterech pol ponizej: gGraphCnt,
  //     gGraphMax, gGraphSeq i gCharging. Wysokosc jest stala kompilacji.
  // Kompletna lista tego, co widac na ekranie spoczynkowym, i pola, ktore to trzyma:
  //   ikona auta → a.bleLink | nazwa trybu → a.mode | moc → a.kw | ikony zrodla →
  //   srcIcons() | procent → a.soc | wykres → gGraphCnt, gGraphMax, gGraphSeq,
  //   gCharging | wszystko naraz → fresh. Kazde z nich jest mieszane ponizej.
  mix(gCursor);
  mix(gMsg);
  mix(fresh ? 1u : 0u);

  // (v189) WYKRES MOCY — CZTERY LICZBY, KTORE OPISUJA GO W CALOSCI. Czwarta doszla
  // razem z PRZEWIJANIEM: gGraphSeq rosnie przy kazdej dopisanej probce takze wtedy,
  // gdy bufor jest pelny i gGraphCnt stoi juz na 128, a wtedy caly przebieg jedzie
  // o kolumne w lewo i prawa krawedz dostaje nowa wartosc. Bez tego pola podpis po
  // 6,4 h sesji przestawalby sie zmieniac i WYKRES ZAMARZALBY na ostatnim obrazie,
  // choc dane plynelyby dalej — ten sam blad zlapano w tym pliku juz dwa razy.
  // Liczba probek rosnie przy KAZDYM dopisaniu do pelna (i wraca do zera na poczatku
  // sesji), wiec do 6,4 h wystarcza sama; maksimum ustawia SKALE, czyli polozenie
  // wszystkich slupkow i napis "NkW" przy lewej krawedzi; gCharging przelacza wypelnienie
  // miedzy pelnym a rastrem. Bufora NIE mieszamy bajt po bajcie — to 128 odczytow
  // z PSRAM-u na kazdy obieg petli rysowania za informacje, ktora niesie juz licznik.
  //
  // Podpis liczymy TAKZE przy !fresh, inaczej niz reszta pol ponizej: wykres zostaje
  // na ekranie takze bez swiezych danych (patrz drawIdle) i musi wtedy przejsc
  // z pelnego wypelnienia na raster — a bez tych bitow w podpisie przejscie nie
  // mialoby prawa sie przerysowac.
  // (v191) PRZEJSCIE PUSTY <-> PELNY JEST W PODPISIE — SPRAWDZONE, NIE ZALOZONE.
  // Napis "brak ładowania" na srodku pasa (patrz drawGraph) zalezy od DOKLADNIE
  // dwoch rzeczy i obie sa tu rozliczone:
  //   * gGraphCnt == 0 — mieszane linijke nizej, wiec kazda zmiana liczby probek
  //     rusza podpisem;
  //   * gGraph != nullptr — ustalane RAZ przy starcie i do konca zycia programu
  //     niezmienne, wiec do podpisu nie ma po co wchodzic (to samo rozumowanie, co
  //     przy wspolrzednych i fontach w akapicie z v190: stala kompilacji lub startu
  //     nie jest stanem).
  // Pierwsza probka sesji rusza przy tym TRZEMA z czterech pol naraz — gGraphCnt
  // 0 -> 1, gGraphSeq +1, gCharging false -> true — wiec podpis zmienia sie w tym
  // samym obiegu petli, w ktorym napis ma zniknac, i to potrojnie. Sprawdzenie nie
  // jest tu formalnoscia: to DOKLADNIE ten blad, ktory w tym pliku zlapano juz trzy
  // razy (ikona auta w v186, przewijanie okna w v189, a przed nimi ekrany ustawien
  // w v187) i za kazdym razem objawial sie tak samo — element zamarza na szkle az
  // do najblizszej zmiany czegos zupelnie innego. Tutaj kosztowalby ekran, ktory po
  // pierwszej probce zostaje na napisie "brak ładowania" mimo plynacych danych.
  mix(gGraphCnt);
  mix(gGraphMax);
  mix(gGraphSeq);   // (v189) przewiniecie okna — patrz akapit wyzej
  mix(gCharging ? 1u : 0u);

  if (fresh) {
    mix(a.soc);
    // (v188) IKONY ZRODLA JAKO DWA BITY, A NIE SUROWE `sunPct`. Do podpisu wchodzi
    // to, CO WIDAC (patrz opis nad ta funkcja): udzial slonca skacze o kilka procent
    // przy kazdej chmurze, a ikona zmienia sie tylko na progach 10 i 90 — mieszanie
    // procentow skladaloby wiec pelna klatke (osiem stron, ~25 ms) co 15 s bez ani
    // jednego zmienionego piksela.
    mix(srcIcons(a, fresh));
    // (v186) bleLink MUSI tu byc, inaczej ikona auta zamarza. Podpis jest JEDYNYM
    // powodem przerysowania (patrz opis nad ta funkcja), wiec pole, ktore widac na
    // ekranie, a ktorego nie ma w podpisie, zmienia sie na szkle dopiero przy
    // najblizszej zmianie czegos innego — czyli objawia sie jako "ikona pokazuje
    // nieprawde przez kilka minut", a nie jako brak rysowania.
    mix(a.bleLink ? 1u : 0u);
    // Rzutowanie przez int32_t, bo moc bywa UJEMNA (oddawanie z auta): konwersja
    // ujemnego float-a wprost na uint32_t jest zachowaniem niezdefiniowanym.
    mix(static_cast<uint32_t>(static_cast<int32_t>(a.kw * 10.f)));
    for (const char* p = a.mode; *p != '\0'; ++p) mix(static_cast<uint8_t>(*p));
    // (v188) `state` I `cable` WYPADLY Z PODPISU RAZEM ZE ZDANIEM NA DOLE EKRANU.
    // Po usunieciu sceneLine() nie widac ich juz NIGDZIE — ani na spoczynku, ani
    // w menu trybu — a regula tego panelu dziala w obie strony: pole widoczne MUSI
    // byc w podpisie, pole niewidoczne nie ma prawa w nim byc. Zostawione skladalyby
    // pelna klatke (osiem stron, ~25 ms) za kazdym razem, gdy integracja Tesli
    // przerzuci "czeka" na "stoi", czyli za nic.
  }
  return h;
}

void renderPage(const AutoModel& a, bool fresh) {
  memset(gPage + 1, 0, sizeof(gPage) - 1);   // gPage[0] to bajt sterujacy — zostaje
  switch (gScr) {
    case SCR_MENU:  drawMenu(a, fresh); break;
    case SCR_TEST:  drawTest(); break;
    case SCR_SET:   drawSettings(); break;
    case SCR_VIEW:  drawViewList(); break;
    case SCR_BRI:
    case SCR_ROT:
    case SCR_NIGHT: drawValues(gScr); break;
    default:        drawIdle(a, fresh); break;
  }
}

// ============================ PRZYCISKI ======================================

void sendMode(uint32_t now) {
  const char* m = autoModeMqtt(gCursor);
  mqttha::requestAutoMode(m);
  strncpy(gSentMode, m, sizeof(gSentMode) - 1);
  gSentMode[sizeof(gSentMode) - 1] = '\0';
  gSentAtMs = now;
  if (gSentAtMs == 0) gSentAtMs = 1;  // 0 znaczy "nic nie czeka" — patrz tick()
  gMsg = MSG_SENDING;
}

// ====================== (v187) MENU USTAWIEN — OBSLUGA KLAWISZY ==============

// PODGLAD NA ZYWO. Wartosc siedzi JUZ w settings() w RAM-ie, a caly firmware czyta
// te pola co klatke — wiec czas ekranu i auto-rotacja dzialaja natychmiast same
// z siebie, bez ani jednej linii wsparcia stad. Jasnosc wymaga jednego ruchu
// wiecej i to jest jedyny powod, dla ktorego ta funkcja w ogole istnieje: petla
// glowna wystawia na PWM ten z TRZECH poziomow, ktory pasuje do biezacego odczytu
// LDR (pogoda-gdynia.ino), wiec ustawianie "Światła" po ciemku nie dawaloby ZADNEGO
// widocznego skutku. Na czas edycji wymuszamy wiec poziom EDYTOWANY — i tylko na
// czas edycji: wymuszenie jest ograniczone czasowo po drugiej stronie
// (WeatherUi::testBacklight), wiec samo gasnie, gdyby wlasciciel odszedl od panelu.
void preview() {
  if (gScr != SCR_BRI || gBlFn == nullptr) return;
  gBlFn(static_cast<uint8_t>(valGet(SCR_BRI, gRowCur)), cfg::OLED_BL_PREVIEW_MS);
}

// Koniec podgladu — ODDAJEMY sterowanie automatowi z LDR OD RAZU, zamiast czekac,
// az wymuszenie wygasnie samo. Bez tego po anulowaniu edycji ekran przez ponad
// dwie sekundy swiecilby jasnoscia, ktora wlasciciel wlasnie odrzucil, i wygladalo
// by to na zignorowane "#". 0 ms znaczy "wymuszenie wygaslo" — tickBacklight()
// sprawdza to przy najblizszej klatce.
void previewOff() {
  if (gScr != SCR_BRI || gBlFn == nullptr) return;
  gBlFn(static_cast<uint8_t>(valGet(SCR_BRI, gRowCur)), 0);
}

void enterSettings() {
  gScr = SCR_SET;
  gSetCur = 0;
  gEditing = false;
}

// Wejscie w podekran. Mapowanie "wiersz menu -> ekran" jest ARYTMETYCZNE, wiec
// pilnuje go straznik ponizej: przestawienie kolejnosci w setItem() bez przestawienia
// enuma otwieraloby cichaczem nie ten ekran, co trzeba.
static_assert(SCR_VIEW + 1 == SCR_BRI && SCR_BRI + 1 == SCR_ROT && SCR_ROT + 1 == SCR_NIGHT,
              "kolejnosc SCR_VIEW/BRI/ROT/NIGHT musi odpowiadac wierszom setItem()");
void enterSub(uint8_t scr) {
  gScr = scr;
  gEditing = false;
  gRowCur = 0;
  gTop = 0;
  if (scr != SCR_VIEW) return;
  // Lista widokow otwiera sie NA TYM, CO JEST TERAZ NA DUZYM EKRANIE. Wlasciciel
  // siega po panel, zeby cos ZMIENIC wzgledem stanu biezacego, wiec kursor stojacy
  // gdzie indziej kazalby mu najpierw odszukac, gdzie jest.
  const int pin = pinnedView();
  gRowCur = static_cast<uint8_t>((pin < 0) ? 0 : rowForView(pin));
  scrollIntoView();
}

// ZATWIERDZENIE TO JEDEN ZAPIS DO NVS, A NIE PIEC. saveTuning() pisze KOMPLET
// (dwie godziny nocne, czas ekranu, trzy jasnosci, auto-rotacja) i robi to pod
// osobnymi kluczami, natychmiast — dokladnie tak samo, jak formularz w panelu WWW.
// Wartosci sa juz w settings(), bo edycja pisze tam na biezaco (to ona daje podglad
// na zywo), wiec tutaj tylko UTRWALAMY to, co wlasciciel widzi. Przewijanie menu
// i sama edycja nie dotykaja flasha ANI RAZU.
void commitEdit() {
  gEditing = false;
  previewOff();
  Settings& s = settings();
  s.saveTuning(s.nightStartH, s.nightEndH, s.dwellS, s.blDay, s.blDim, s.blNight,
               s.autoRotate);
}

// ANULOWANIE PRZYWRACA STAN SPRZED WEJSCIA W EDYCJE. Nie "wartosc domyslna" i nie
// "ostatnio zapisana" — dokladnie te, ktora byla na ekranie w chwili nacisniecia
// "✱". Do NVS nic nie poszlo, wiec cofniecie to jeden zapis do RAM-u.
void cancelEdit() {
  valSet(gScr, gRowCur, gEditOld);
  gEditing = false;
  previewOff();
}

// Zmiana wartosci W KOLKO, a nie do sciany — ta sama regula, co przy kursorze menu
// trybu i z tego samego powodu: przy 39 krokach jasnosci albo 24 godzinach dojscie
// do drugiego konca "po sciance" trwaloby caly zakres, a tak jest jedno nacisniecie.
// Ryzyka nie ma, bo nic tu jeszcze nie jest zapisane: "#" cofa kazdy przeskok.
void bumpValue(int dir) {
  const int32_t lo = valMin(gScr, gRowCur);
  const int32_t hi = valMax(gScr, gRowCur);
  int32_t v = static_cast<int32_t>(valGet(gScr, gRowCur)) + dir * static_cast<int32_t>(valStep(gScr));
  if (v > hi) v = lo;
  if (v < lo) v = hi;
  valSet(gScr, gRowCur, static_cast<uint16_t>(v));
  preview();
}

// Podekran "Ekran". Tu NIE MA trybu edycji i nie jest to niekonsekwencja: przypiecie
// widoku dziala natychmiast i widac je na duzym ekranie, wiec "zatwierdzanie" nie
// mialoby czego zatwierdzac. Do NVS tez nic nie idzie — przypiecie zyje w polu
// WeatherUi::pinned_ i ginie przy restarcie, tak jak przypiecie z panelu WWW.
void viewKey(uint8_t i) {
  const int rows = viewRows();
  switch (i) {
    case cfg::BTN_UP:
      gRowCur = static_cast<uint8_t>((gRowCur + rows - 1) % rows);
      scrollIntoView();
      break;
    case cfg::BTN_DOWN:
      gRowCur = static_cast<uint8_t>((gRowCur + 1) % rows);
      scrollIntoView();
      break;
    // viewForRow(0) oddaje -1, czyli dokladnie to, co pinView() rozumie jako
    // "zwolnij przypiecie" — wiersz "auto (rotacja)" nie potrzebuje wiec zadnego
    // przypadku szczegolnego.
    case cfg::BTN_OK:
      if (gPinFn != nullptr) gPinFn(viewForRow(gRowCur));
      break;
    case cfg::BTN_BACK: gScr = SCR_SET; break;
    default: break;
  }
}

// Trzy podekrany wartosci maja WSPOLNA obsluge, bo roznia sie wylacznie danymi
// (ile wierszy, jaki zakres, jaki napis) — a te siedza w valRows()/valMin()/
// valMax()/valText(). Osobne funkcje na jasnosc, rotacje i noc znaczylyby trzy
// kopie tej samej maszynki stanow, ktore rozjezdzaja sie przy pierwszej poprawce.
void valueKey(uint8_t i) {
  const uint8_t rows = valRows(gScr);
  if (!gEditing) {
    switch (i) {
      case cfg::BTN_UP:   gRowCur = static_cast<uint8_t>((gRowCur + rows - 1) % rows); break;
      case cfg::BTN_DOWN: gRowCur = static_cast<uint8_t>((gRowCur + 1) % rows); break;
      case cfg::BTN_OK:
        gEditing = true;
        gEditOld = valGet(gScr, gRowCur);
        preview();   // jasnosc ma byc widoczna JUZ przy wejsciu, nie dopiero po zmianie
        break;
      case cfg::BTN_BACK: gScr = SCR_SET; break;
      default: break;
    }
    return;
  }
  switch (i) {
    case cfg::BTN_UP:   bumpValue(+1); break;
    case cfg::BTN_DOWN: bumpValue(-1); break;
    case cfg::BTN_OK:   commitEdit(); break;
    case cfg::BTN_BACK: cancelEdit(); break;
    default: break;
  }
}

void settingsKey(uint8_t i) {
  if (gScr == SCR_SET) {
    switch (i) {
      case cfg::BTN_UP:   gSetCur = static_cast<uint8_t>((gSetCur + 3) & 3); break;
      case cfg::BTN_DOWN: gSetCur = static_cast<uint8_t>((gSetCur + 1) & 3); break;
      case cfg::BTN_OK:   enterSub(static_cast<uint8_t>(SCR_VIEW + gSetCur)); break;
      case cfg::BTN_BACK: gScr = SCR_IDLE; break;
      default: break;
    }
    return;
  }
  if (gScr == SCR_VIEW) {
    viewKey(i);
    return;
  }
  valueKey(i);
}

// Akcja przypisana do PUSZCZENIA przycisku (patrz pollButtons — i tam jest
// uzasadnienie, dlaczego nie do wcisniecia).
void onKey(uint8_t i, uint32_t now, const AutoModel& a) {
  // W tescie przyciski TYLKO sie pokazuja. (v178) Puszczenie, ktore z tego ekranu
  // WYCHODZI, w ogole tu nie dociera — pollButtons() obsluguje je u siebie i celowo
  // nie wola onKey(), zeby wyjscie z testu nie zmienilo przy okazji trybu ladowania.
  if (gScr == SCR_TEST) return;

  if (gScr == SCR_IDLE) {
    // (v187) ZE SPOCZYNKU ROZCHODZA SIE DWIE DROGI, A NIE JEDNA. Do v186 KAZDY
    // przycisk budzil menu trybu — teraz strzalki prowadza tam, a "✱" do ustawien.
    // "#" NIE ROBI TU NIC I TO JEST CALA JEGO ROLA: skoro obiecalismy, ze ten
    // przycisk zawsze cofa, to ze spoczynku, gdzie cofac sie nie ma dokad, ma
    // milczec. Gdyby otwieral cokolwiek, przestalby byc przyciskiem, ktory mozna
    // nacisnac na oslep — a to jedyny taki na tym module.
    if (i == cfg::BTN_BACK) return;
    if (i == cfg::BTN_OK) {
      enterSettings();
      return;
    }
    // Pierwsze nacisniecie BUDZI menu i celowo NICZEGO nie wybiera: wlasciciel
    // siega do panelu, nie wiedzac, ktory przycisk trzyma pod palcem, wiec
    // wykonanie akcji "od razu" bylo by losowaniem.
    gScr = SCR_MENU;
    const int act = autoModeIndex(a.mode);
    gCursor = (act >= 0) ? static_cast<uint8_t>(act) : 0;
    gMsg = MSG_NONE;
    return;
  }

  if (inSettings()) {
    settingsKey(i);
    return;
  }

  switch (i) {
    // W KOLKO, a nie do sciany: cztery pozycje to za malo, zeby oplacalo sie
    // pilnowac konca listy — z "CAŁA NAPRZÓD" do "STOP" ma byc jeden ruch.
    case cfg::BTN_UP:   gCursor = static_cast<uint8_t>((gCursor + 3) & 3); break;
    case cfg::BTN_DOWN: gCursor = static_cast<uint8_t>((gCursor + 1) & 3); break;
    case cfg::BTN_OK:   sendMode(now); break;
    // Wyjscie ZAMYKA takze oczekiwanie na potwierdzenie: komunikat o nim ma gdzie
    // stanac tylko w naglowku menu, wiec trzymanie go po wyjsciu znaczyloby, ze
    // ekran spoczynkowy przerysowuje sie za 10 s bez zadnej widocznej przyczyny.
    case cfg::BTN_BACK: gScr = SCR_IDLE; gMsg = MSG_NONE; gSentAtMs = 0; break;
    default: break;
  }
}

// (v178) CHWYT OTWIERAJACY EKRAN TEST: cfg::BTN_OK i cfg::BTN_BACK wcisniete
// JEDNOCZESNIE przez cfg::OLED_TEST_HOLD_MS. Fizycznie sa to dwa SASIADUJACE
// klawisze na gorze modulu, wiec chwyt jest wygodny, a przypadkiem sie go nie zrobi.
// Uzasadnienie samej zmiany (i tego, czemu przytrzymanie DOWOLNEGO przycisku bylo
// pulapka) stoi przy cfg::OLED_TEST_HOLD_MS w Config.h.
//
// TRZYMANIE LICZYMY OD POZNIEJSZEGO Z DWOCH WCISNIEC, czyli bierzemy KROTSZY z dwoch
// czasow: chwyt zaczyna sie dopiero wtedy, gdy OBA klawisze sa juz na dole. Roznice
// "now - gDownAt" sa odporne na przekrecenie millis(), porownanie samych znacznikow
// nie byloby.
void checkTestCombo(uint32_t now) {
  constexpr uint8_t kCombo =
      static_cast<uint8_t>((1u << cfg::BTN_OK) | (1u << cfg::BTN_BACK));
  if (gScr == SCR_TEST) return;
  if ((gDown & kCombo) != kCombo) return;

  // ZASLONA ZAPADA JUZ TERAZ, a nie dopiero po 3 s. Dzieki temu chwyt PRZERWANY
  // w polowie (wlasciciel puscil za wczesnie) nie konczy sie wyslaniem trybu do auta
  // przez puszczenie "zatwierdz" — to dokladnie ten wzglad bezpieczenstwa, ktorego
  // do v177 pilnowal warunek czasowy przy puszczeniu, skasowany nizej.
  gSwallow = static_cast<uint8_t>(gSwallow | kCombo);

  const uint32_t heldOk = now - gDownAt[cfg::BTN_OK];
  const uint32_t heldBack = now - gDownAt[cfg::BTN_BACK];
  const uint32_t held = (heldOk < heldBack) ? heldOk : heldBack;
  if (held < cfg::OLED_TEST_HOLD_MS) return;

  // (v187) CHWYT DZIALA TAKZE Z USTAWIEN — i wtedy MUSI ZAMKNAC EDYCJE JAK "#".
  // Inaczej byloby to jedyne wyjscie z ekranu wartosci, ktore zostawia w RAM-ie
  // liczbe nigdzie nie zatwierdzona: z testu wychodzi sie do spoczynku, wiec
  // wlasciciel nie zobaczylby juz tamtego wiersza i nie mialby jak jej cofnac.
  // Obietnica "anulowanie przywraca stan sprzed edycji" nie moze miec wyjatku
  // ukrytego pod chwytem serwisowym.
  if (gEditing) cancelEdit();

  gScr = SCR_TEST;
  gMsg = MSG_NONE;
  gLastKeyMs = now;
  gSwallow = gDown;   // patrz uzasadnienie przy wyjsciu z testu w pollButtons()
}

// AKCJE LECA NA PUSZCZENIE, NIE NA WCISNIECIE — i to jest decyzja o bezpieczenstwie,
// nie o wygodzie. Na koncu roli "zatwierdz" stoi WYSLANIE TRYBU LADOWANIA AUTA, wiec
// ma sie ono dziac wtedy, gdy palec SCHODZI z guzika, a nie w chwili dotkniecia.
//
// (v178) ZNIKNAL WARUNEK "trzymane krocej niz cfg::OLED_TEST_HOLD_MS" przy puszczeniu
// i to jest zmiana zamierzona, a nie zgubiona linia. Byl potrzebny dopoty, dopoki
// ekran testu otwieralo PRZYTRZYMANIE DOWOLNEGO przycisku: bez niego przytrzymanie
// tego, ktory okazywal sie "zatwierdz", najpierw wyslaloby zmiane trybu, a dopiero
// potem otworzylo diagnostyke. Skoro pojedyncze przytrzymanie niczego juz nie
// otwiera, ma sie zachowywac jak ZWYKLE NACISNIECIE — inaczej guzik trzymany
// "chwile dluzej" bylby ignorowany po cichu, czyli dokladnie tak, jak wygladala
// usterka zgloszona przez wlasciciela. Przypadek, ktorego tamten warunek pilnowal,
// zalatwia teraz checkTestCombo(): oba klawisze chwytu dostaja zaslone gSwallow
// w chwili, gdy oba sa na dole, wiec chwyt przerwany w polowie tez nic nie wysyla.
void pollButtons(uint32_t now, const AutoModel& a) {
  for (uint8_t i = 0; i < 4; ++i) {
    const bool down = digitalRead(kPins[i]) == LOW;  // zwiera do masy, INPUT_PULLUP
    const bool was = (gDown & (1u << i)) != 0;

    // Brak zbocza. Samo PRZYTRZYMANIE nie ma juz tu nic do roboty — jedyne, co
    // z niego wynika, to chwyt otwierajacy test, a ten liczy checkTestCombo() nizej.
    if (down == was) continue;

    // Holdoff na drgania styku. Liczony od OSTATNIEGO PRZYJETEGO zbocza, wiec
    // dziala tak samo na zbocze w dol i w gore.
    if ((now - gEdgeAt[i]) < cfg::OLED_BTN_HOLDOFF_MS) continue;
    gEdgeAt[i] = now;
    gLastKeyMs = now;

    if (down) {
      gDown |= static_cast<uint8_t>(1u << i);
      gDownAt[i] = now;
      continue;
    }

    gDown = static_cast<uint8_t>(gDown & ~(1u << i));
    const bool swallowed = (gSwallow & (1u << i)) != 0;
    gSwallow = static_cast<uint8_t>(gSwallow & ~(1u << i));

    // (v178) WYJSCIE Z TESTU JEST NATYCHMIASTOWE i jest JEDYNYM skutkiem tego
    // puszczenia: onKey() sie nie wykonuje, wiec wyjscie nie zmienia przy okazji
    // trybu ladowania auta. Gornego limitu czasu trzymania tu NIE MA celowo — caly
    // sens tej poprawki polega na tym, ze z ekranu testu da sie wyjsc ZAWSZE, a nie
    // tylko wtedy, gdy trafi sie we wlasciwe okno czasowe.
    //
    // Przyciski TRZYMANE W TEJ CHWILI zaslaniamy, bo ich puszczenia naleza do tego
    // samego ruchu reki i tez nie maja nic wybierac. gSwallow = gDown, a NIE 0x0F:
    // bit zapalony na przycisku, ktorego nikt nie trzyma, dotrwalby do jego
    // nastepnego nacisniecia i POLKNALBY je (ten sam blad opisuje komentarz przy
    // wyjsciu po bezczynnosci w tick()).
    if (gScr == SCR_TEST) {
      if (!swallowed) {
        gScr = SCR_IDLE;
        gSwallow = gDown;
      }
      continue;
    }

    if (!swallowed) onKey(i, now, a);
  }

  checkTestCombo(now);
}

// (v176) ZDJECIE WIRTUALNYCH NACISNIEC Z KOLEJKI — patrz gInject u gory pliku.
// Wykonujemy TE SAMA funkcje, co puszczenie fizycznego przycisku, wiec strona WWW
// nie ma wlasnej sciezki akcji, ktora moglaby sie rozjechac z panelem.
//
// STANU PRZYCISKOW CELOWO NIE DOTYKAMY: gDown/gDownAt/gSwallow zostaja nietkniete,
// bo to one licza PRZYTRZYMANIE. Dzieki temu klikniecie ze strony jest zawsze
// KROTKIM nacisnieciem i puszczeniem POJEDYNCZEGO przycisku — (v178) nie da sie nim
// zlozyc chwytu otwierajacego ekran TEST (cfg::BTN_OK + cfg::BTN_BACK trzymane RAZEM
// przez cfg::OLED_TEST_HOLD_MS; a wejscie w ten ekran z drugiego konca miasta nie
// mialoby sensu: sluzy on do patrzenia na guziki, ktore ma sie pod palcem) i nie da
// sie nim zaklamac ekranu testu, ktory pokazuje wylacznie stan fizycznych stykow.
//
// Z TESTU TEZ SIE STAD NIE WYCHODZI, i to jest wybor, nie przeoczenie: onKey()
// wychodzi w tescie pierwsza linia, a wyjscie siedzi w pollButtons() przy PUSZCZENIU
// fizycznego styku. Ekran testu oglada sie stojac przy module, wiec klikniecie
// w przegladarce nie ma prawa przerwac komus tego ogladania. Wlasciciel przy panelu
// ma dwie wlasne drogi wyjscia (dowolne nacisniecie oraz cfg::OLED_TEST_EXIT_MS),
// a z przegladarki i tak nie da sie w test WEJSC, wiec nie da sie w nim utknac.
//
// gLastKeyMs przesuwamy tak samo, jak przy zboczu fizycznym: klikniecie w przegladarce
// ma trzymac menu otwarte przez cfg::OLED_MENU_IDLE_MS, a nie pozwalac mu zgasnac
// pod palcami wlasciciela.
void drainInject(uint32_t now, const AutoModel& a) {
  const uint32_t m = gInject.exchange(0, std::memory_order_relaxed);
  if (m == 0) return;
  gLastKeyMs = now;
  for (uint8_t i = 0; i < 4; ++i) {
    if ((m & (1u << i)) != 0) onKey(i, now, a);
  }
}

// ============================ UPLYW CZASU ====================================
// PANEL NIE MOZE UDAWAC, ZE COS USTAWIL. Po zatwierdzeniu kropka zostaje na STARYM
// trybie i czeka na `tryb` z <prefix>/auto/stan. Sa dokladnie trzy wyjscia z tego
// czekania i kazde jest widoczne dla wlasciciela:
//   * automatyka potwierdzila (a.mode == to, co wyslalismy) -> komunikat znika,
//     a kropka przeskakuje SAMA, bo bierze sie z danych,
//   * MQTT w ogole nie wyslal polecenia -> "nie wysłano",
//   * minelo cfg::OLED_CONFIRM_MS bez potwierdzenia -> "brak potwierdzenia".
void tick(uint32_t now, const AutoModel& a, bool fresh) {
  if (gSentAtMs != 0) {
    const uint8_t st = mqttha::autoModeReqState();
    if (st == 3) {
      gMsg = MSG_FAILED;
      gSentAtMs = 0;
    } else if (fresh && strcmp(a.mode, gSentMode) == 0) {
      gMsg = MSG_NONE;
      gSentAtMs = 0;
    } else if ((now - gSentAtMs) >= cfg::OLED_CONFIRM_MS) {
      gMsg = MSG_NOACK;
      gSentAtMs = 0;
    }
  }

  if (gScr == SCR_MENU && (now - gLastKeyMs) >= cfg::OLED_MENU_IDLE_MS) {
    gScr = SCR_IDLE;
    gMsg = MSG_NONE;
    gSentAtMs = 0;   // patrz uzasadnienie przy BTN_BACK w onKey()
  }
  // (v187) USTAWIENIA GASNA PO WLASNYM, DLUZSZYM PROGU (cfg::OLED_SET_IDLE_MS) —
  // uzasadnienie tej drugiej liczby stoi przy niej w Config.h. WYJSCIE ZACHOWUJE SIE
  // DOKLADNIE JAK "#": nie zapisuje niczego i przywraca wartosc sprzed wejscia
  // w edycje. Odejscie od panelu w polowie dobierania jasnosci nie ma prawa niczego
  // utrwalic — to jedyna zmiana w tym menu, ktorej z drugiego konca mieszkania nie
  // widac, wiec musi sama po sobie posprzatac.
  if (inSettings() && (now - gLastKeyMs) >= cfg::OLED_SET_IDLE_MS) {
    if (gEditing) cancelEdit();
    gScr = SCR_IDLE;
  }
  // (v178) DRUGA FURTKA Z TESTU: 10 s bez zadnego zbocza i bez trzymanego guzika.
  // Pierwsza i wazniejsza jest natychmiastowa — krotkie nacisniecie i puszczenie
  // dowolnego przycisku (pollButtons). Ta zostaje na wypadek, gdyby wlasciciel wszedl
  // w test i po prostu odszedl od panelu. Warunek gDown == 0 pilnuje, zeby trzymanie
  // guzika, ktory wlasnie sie sprawdza, nie wyrzucalo z ekranu.
  if (gScr == SCR_TEST && gDown == 0 && (now - gLastKeyMs) >= cfg::OLED_TEST_EXIT_MS) {
    gScr = SCR_IDLE;
    // ZASLONA JEST TU JUZ PUSTA i to zerowanie tylko to potwierdza — zostaje jako
    // straznik niezmiennika, a nie jako naprawa. DO v177 wejscie w test zapalalo
    // WSZYSTKIE cztery bity (0x0F), wiec bity trzech nienacisnietych przyciskow
    // dotrwalyby tutaj i po powrocie do spoczynku POLKNELY BY pierwsze nacisniecie
    // kazdego z nich — menu nie otworzyloby sie za pierwszym razem. Od v178 zaslone
    // stawiamy WYLACZNIE na przyciski faktycznie trzymane (gSwallow = gDown), a do
    // tego miejsca dochodzimy przy gDown == 0, wiec nie ma juz czego kasowac.
    gSwallow = 0;
  }
}

// ACK pod danym adresem — to on rozstrzyga o obecnosci modulu, a nie nadruk na
// plytce. 50 ms z zapasem: sam cykl adresowy to kilkadziesiat mikrosekund.
bool probe(uint8_t addr) {
  return i2c_master_probe(gBus, addr, 50) == ESP_OK;
}

}  // namespace

// ================================ API ========================================

void begin() {
  // Piny przyciskow ustawiamy ZAWSZE, takze bez modulu: kosztuje to cztery wpisy do
  // rejestru, a zostawienie ich w stanie domyslnym (wejscie bez podciagniecia) daje
  // cztery plywajace wejscia, ktore potrafia laskotac pobor pradu.
  for (int i = 0; i < 4; ++i) pinMode(kPins[i], INPUT_PULLUP);

  gPage[0] = 0x40;  // bajt sterujacy "dalej ida DANE" — stoi tu do konca zycia programu

  i2c_master_bus_config_t bc = {};
  bc.i2c_port = -1;  // niech sterownik wybierze wolny kontroler — nikt inny go tu nie uzywa
  bc.sda_io_num = static_cast<gpio_num_t>(cfg::PIN_OLED_SDA);
  bc.scl_io_num = static_cast<gpio_num_t>(cfg::PIN_OLED_SCL);
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;   // wartosc zalecana przez IDF dla zwyklej magistrali
  bc.intr_priority = 0;       // domyslny priorytet przerwania
  bc.trans_queue_depth = 0;   // 0 = tylko transakcje SYNCHRONICZNE, a innych nie robimy
  // Podciagniecia wewnetrzne (~45 kΩ) sa ZA SLABE na 400 kHz i NIE zastepuja tych na
  // module — wlaczamy je jako zabezpieczenie na wypadek egzemplarza bez wlasnych
  // rezystorow, gdzie i tak trzeba by zejsc z czestotliwoscia. Rownolegle do 4,7 kΩ
  // z modulu nie zmieniaja niczego.
  bc.flags.enable_internal_pullup = 1;

  if (i2c_new_master_bus(&bc, &gBus) != ESP_OK) {
    gBus = nullptr;
    LOG("OLED: nie udalo sie otworzyc magistrali I2C (SDA=%d SCL=%d) — panel wylaczony\n",
        cfg::PIN_OLED_SDA, cfg::PIN_OLED_SCL);
    return;
  }

  // ADRESU NIE ZAKLADAMY. Pytamy po kolei; ACK rozstrzyga.
  gAddr = probe(cfg::OLED_ADDR_A) ? cfg::OLED_ADDR_A
                                  : (probe(cfg::OLED_ADDR_B) ? cfg::OLED_ADDR_B : 0);
  if (gAddr == 0) {
    // BRAK MODULU NIE JEST AWARIA — wlasciciel wgrywa firmware, zanim cokolwiek
    // podlaczy. Zwalniamy magistrale (razem z jej pamiecia na stercie) i WYLACZAMY
    // panel na cale zycie programu: gPresent zostaje false, wiec step() wychodzi
    // pierwsza linia i nie ma ani ponawiania co klatke, ani wiszacych transakcji.
    i2c_del_master_bus(gBus);
    gBus = nullptr;
    LOG("OLED: brak modulu na SDA=%d SCL=%d (sprawdzone 0x%02X i 0x%02X) — panel wylaczony\n",
        cfg::PIN_OLED_SDA, cfg::PIN_OLED_SCL, cfg::OLED_ADDR_A, cfg::OLED_ADDR_B);
    return;
  }

  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = gAddr;
  dc.scl_speed_hz = cfg::OLED_I2C_HZ;
  if (i2c_master_bus_add_device(gBus, &dc, &gDev) != ESP_OK) {
    gDev = nullptr;
    i2c_del_master_bus(gBus);
    gBus = nullptr;
    gAddr = 0;
    LOG("OLED: modul odpowiedzial, ale nie udalo sie go dodac do magistrali — panel wylaczony\n");
    return;
  }

  for (size_t i = 0; i < sizeof(kInit); ++i) cmd(pgm_read_byte(&kInit[i]));

  // (v176) KOPIA OBRAZU DLA PODGLADU WWW — 1024 B W PSRAM, nie w statycznym RAM-ie.
  // Plytka ma PSRAM wlaczony w FQBN, wiec jest skad brac; MALLOC_CAP_SPIRAM zabrania
  // sterowniku zejsc po cichu do wewnetrznego RAM-u, czyli albo dostajemy pamiec
  // stamtad, gdzie jej nie brakuje, albo nie dostajemy jej wcale.
  //
  // NIEUDANA ALOKACJA NIE JEST BLEDEM — panel ma dzialac dalej dokladnie tak samo,
  // niedostepny jest wtedy TYLKO podglad w przegladarce. Dlatego nie ma tu ani
  // return, ani gaszenia panelu: gShadow zostaje nullptr, sendPage() nic nie kopiuje,
  // a shadow() oddaje nullptr i to strona decyduje, co z tym zrobic.
  //
  // ZERUJEMY OD RAZU: heap_caps_malloc oddaje pamiec NIEWYCZYSZCZONA, a strona WWW
  // moze poprosic o podglad, zanim panel zlozy pierwsza pelna klatke (osiem obiegow
  // petli). Bez tego pokazalaby wtedy smieci po poprzednim uzytkowniku PSRAM-u
  // zamiast czerni, ktora w tej chwili naprawde jest na szkle.
  gShadow = static_cast<uint8_t*>(heap_caps_malloc(1024, MALLOC_CAP_SPIRAM));
  if (gShadow != nullptr) {
    memset(gShadow, 0, 1024);
  } else {
    LOG("OLED: brak 1024 B w PSRAM na kopie obrazu — panel dziala, podgladu WWW nie ma\n");
  }

  // (v188) BUFOR WYKRESU MOCY — 128 B, TA SAMA DROGA I TA SAMA UMOWA, co kopia
  // obrazu wyzej: PSRAM, bo w statycznym RAM-ie do bariery 76 000 B zostalo ~1,4 kB
  // i 128 B z tego za jeden pas jednego ekranu to zla wymiana. Nieudana alokacja NIE
  // JEST BLEDEM — gGraph zostaje nullptr, graphTick() i drawGraph() wychodza pierwsza
  // linia, a ekran spoczynkowy dziala bez zmiany, tylko dolny pas jest pusty.
  //
  // ZERUJEMY, bo heap_caps_malloc oddaje pamiec nietknieta: bez tego pierwsza sesja
  // rysowalaby smieci po poprzednim uzytkowniku PSRAM-u. gGraphCnt = 0 i tak nie
  // pozwolilby ich pokazac, ale bufor, ktorego zawartosc zalezy od przypadku, jest
  // gorszy od bufora wyzerowanego przy KAZDYM pozniejszym bledzie.
  gGraph = static_cast<uint8_t*>(heap_caps_malloc(kGraphN, MALLOC_CAP_SPIRAM));
  if (gGraph != nullptr) {
    memset(gGraph, 0, kGraphN);

    // (v194) ODTWORZENIE WYKRESU Z NVS. Do v193 kazdy restart — a wiec KAZDA
    // aktualizacja OTA — zaczynal dolny pas od zera i przez pierwsze minuty
    // pokazywal "brak ładowania", chociaz auto ladowalo sie bez przerwy.
    //
    // TU TYLKO WCZYTUJEMY PROBKI. Rozstrzygniecie "czy sesja trwa dalej" zapada
    // PoZNIEJ, w graphTick, i to nie jest komplikacja dla samej komplikacji:
    // w tej chwili NTP jeszcze nie doszedl (init panelu idzie przed synchronizacja
    // czasu), wiec time() oddaje rok 1970 i przerwy nie da sie policzyc. Gdybysmy
    // decydowali teraz, wychodziloby zawsze "sesja skonczona" — a wtedy pierwsza
    // probka po restarcie uznalaby sie za poczatek NOWEJ sesji i skasowala bufor,
    // ktory wlasnie odtworzylismy. Zapis przezylby restart i tak nigdy nie trafilby
    // na ekran. Pelny opis pulapki: GraphBlob.h.
    GraphBlob b;
    if (graphBlobLoad(b)) {
      memcpy(gGraph, b.s, kGraphN);
      gGraphCnt = b.cnt;
      gGraphMax = b.max;
      gRestoreEpoch = b.lastEpoch;
      gRestoreCharging = (b.charging != 0);
      gRestorePending = true;
      LOG("OLED: wykres odtworzony z NVS — %u probek, ladowanie=%u\n",
          static_cast<unsigned>(b.cnt), static_cast<unsigned>(b.charging));
    }
  } else {
    LOG("OLED: brak %d B w PSRAM na wykres mocy — panel dziala, wykresu nie ma\n", kGraphN);
  }

  // Czyscimy WSZYSTKIE osiem stron, zanim wlasciciel cokolwiek zobaczy: pamiec
  // kontrolera po wlaczeniu zasilania jest przypadkowa, a pierwsza nasza klatka
  // pojdzie dopiero za kilka obiegow petli.
  memset(gPage + 1, 0, sizeof(gPage) - 1);
  for (uint8_t p = 0; p < 8; ++p) sendPage(p);

  gPresent = true;
  gPageIdx = 0;   // pierwsza klatka do zlozenia od zaraz
  gSig = 0;
  LOG("OLED: modul 0x%02X na SDA=%d SCL=%d, przyciski GPIO %d/%d/%d/%d\n", gAddr,
      cfg::PIN_OLED_SDA, cfg::PIN_OLED_SCL, kPins[0], kPins[1], kPins[2], kPins[3]);
}

// JEDEN OBIEG = przyciski + uplyw czasu + NAJWYZEJ JEDNA STRONA obrazu.
//
// RACHUNEK, KTORY O TYM DECYDUJE: jedna strona to 128 B danych plus dwa bajty
// sterujace i trzy komendy adresujace, czyli okolo 136 bajtow ramek I2C. Przy
// 400 kHz i dziewieciu bitach na bajt (bajt + ACK) daje to ~3,1 ms, do czego
// dochodzi zlozenie strony w pamieci. Pelna klatka to osiem takich obiegow, czyli
// ~25 ms rozlozone na osiem klatek glownego ekranu zamiast wyjete z jednej.
// Gdy tresc sie nie zmienia — a to jest stan normalny — koszt spada do czterech
// odczytow GPIO i policzenia podpisu, czyli do kilku mikrosekund.
void step(const AutoModel& a, uint32_t now) {
  if (!gPresent) return;
  const uint32_t t0 = micros();

  // Ta sama regula swiezosci, co dla ekranu AUTO na TFT (cfg::AUTO_STALE_MS): dane
  // sa PCHANE co ~15 s, wiec 45 s ciszy znaczy, ze automatyka nie dostarcza.
  // Roznica na int32, bo millis() przekreca sie po ~49 dniach, a atMs pisze INNY
  // rdzen niz ten, ktory tu liczy. (P1-1) Liczone teraz przez isFresh() z
  // Freshness.h, zeby ten sam wzor nie zyl w trzech kopiach — patrz komentarz tam.
  const bool fresh = isFresh(now, a.atMs, cfg::AUTO_STALE_MS);

  // Tryb potwierdzony przez automatyke — liczony RAZ na obieg i zapamietany dla
  // activeMode(), zeby strona WWW czytala dokladnie to samo, z czego bierze sie
  // kropka na ekranie. Bez swiezych danych: -1, czyli "nie wiadomo", a nie ostatnia
  // znana wartosc — panel nie ma prawa twierdzic, ze auto stoi w trybie, o ktorym
  // milczy od 45 s (cfg::AUTO_STALE_MS).
  gActive = fresh ? static_cast<int8_t>(autoModeIndex(a.mode)) : -1;

  pollButtons(now, a);
  // PO przyciskach fizycznych, PRZED uplywem czasu: klikniecie ze strony ma trafic
  // w ten sam obieg, w ktorym tick() sprawdza wygaszanie menu, wiec swieze
  // gLastKeyMs juz sie liczy i menu nie gasnie w tej samej chwili, w ktorej ktos
  // je otworzyl z przegladarki.
  drainInject(now, a);
  tick(now, a, fresh);
  // (v188) Probka do wykresu — PRZED policzeniem podpisu, zeby dopisana wartosc
  // trafila na szklo w TYM obiegu, a nie dopiero przy najblizszej zmianie czegos
  // innego. Wolane niezaleznie od ekranu: dane zbieraja sie takze wtedy, gdy panel
  // stoi na menu albo w ustawieniach.
  graphTick(a, now, fresh);

  // Zmiana podpisu przerywa skladanie biezacej klatki i zaczyna od strony 0. Przez
  // te ~25 ms gorne strony pokazuja juz nowa tresc, a dolne jeszcze stara — na
  // ekranie 128x64 jest to niezauwazalne, a alternatywa (dokonczenie starej klatki
  // przed rozpoczeciem nowej) opoznialaby reakcje na przycisk o pol setnej sekundy
  // bez zadnego zysku.
  const uint32_t sig = signature(a, fresh);
  if (sig != gSig) {
    gSig = sig;
    gPageIdx = 0;
  }

  if (gPageIdx < 8) {
    gRow0 = gPageIdx * 8;
    renderPage(a, fresh);
    sendPage(gPageIdx);
    ++gPageIdx;
  }

  gStepUs = micros() - t0;
}

bool present() { return gPresent; }
uint8_t address() { return gAddr; }
uint32_t pagesSent() { return gPagesSent; }
uint32_t i2cErrors() { return gI2cErr; }
uint32_t lastStepUs() { return gStepUs; }
uint8_t buttonMask() { return gDown; }
const char* sentMode() { return gSentMode; }
uint8_t graphCount() { return gGraph != nullptr ? gGraphCnt : 0; }

// --- (v176) podglad i sterowanie z panelu WWW — patrz OledPanel.h --------------

const uint8_t* shadow() { return gShadow; }

uint8_t cursor() { return gCursor; }

const char* activeMode() { return gActive >= 0 ? autoModeMqtt(gActive) : ""; }

// TU SIE NIC NIE WYKONUJE — zapalamy bit i wracamy. Wola nas ZADANIE SERWERA WWW,
// a caly stan panelu (gScr, gCursor, gMsg) nalezy do zadania petli rysowania;
// wejscie w onKey() stad byloby wyscigiem o kazde z tych pol i o wysylke MQTT.
// Odbior bitu i wykonanie akcji siedzi w drainInject(), wolanym ze step().
void injectPress(uint8_t role) {
  if (!gPresent) return;   // bez modulu nie ma czego sterowac
  if (role > 3) return;    // rola spoza cfg::BTN_* — ignorujemy, zamiast zgadywac
  gInject.fetch_or(1u << role, std::memory_order_relaxed);
}

// (v187) JEDNA NAZWA NA CALE DRZEWO USTAWIEN, a nie piec osobnych. /api/diag ma
// odpowiadac na pytanie "co panel teraz robi", a nie prowadzic wlasnej nawigacji:
// piksele i tak widac w podgladzie (shadow()), a piec nowych napisow trzeba by
// utrzymywac w zgodzie z enumem przy kazdym nowym podekranie.
const char* screenName() {
  switch (gScr) {
    case SCR_MENU: return "menu";
    case SCR_TEST: return "test";
    case SCR_SET:
    case SCR_VIEW:
    case SCR_BRI:
    case SCR_ROT:
    case SCR_NIGHT: return "ustawienia";
    default: return "spoczynek";
  }
}

// Trzy wskazniki do glownego ekranu — pelne uzasadnienie stoi przy deklaracji
// w OledPanel.h. Ustawiane RAZ, w setup(), zanim cokolwiek moze je wywolac:
// menu ustawien budzi sie dopiero z nacisniecia przycisku, a to jest o cala
// inicjalizacje pozniej.
void setUiHooks(void (*pinFn)(int), int (*pinnedFn)(), void (*blFn)(uint8_t, uint32_t)) {
  gPinFn = pinFn;
  gPinnedFn = pinnedFn;
  gBlFn = blFn;
}

}  // namespace oled
