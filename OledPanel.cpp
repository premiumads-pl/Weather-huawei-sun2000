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

#include "Config.h"
#include "Format.h"      // fmt1() — polski przecinek dziesietny, wspolny z ekranem TFT
#include "Log.h"
#include "MqttClient.h"  // requestAutoMode() / autoModeReqState()
#include "PlText.h"      // pltxt:: — silnik fontow projektu (dekoder UTF-8, metryki)
#include "PlexText.h"    // plex::f10/f11/f13 — te same fonty, co glowny ekran

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

enum Screen : uint8_t { SCR_IDLE = 0, SCR_MENU = 1, SCR_TEST = 2 };
uint8_t gScr = SCR_IDLE;
uint8_t gCursor = 0;      // podswietlony wiersz menu (0..3) — to NIE jest tryb aktywny
uint32_t gLastKeyMs = 0;  // ostatnie zbocze na dowolnym przycisku

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
// oko. Najciasniejsze miejsce calego panelu to wiersz ekranu TEST: "GPIO15" konczy
// sie na x=56, a prawostronnie wyrownany napis "WCIŚNIĘTY" (53 px) zaczyna sie na
// x=72 — zostaje 15 px przerwy. Pozostale zapasy: nazwa trybu na ekranie
// spoczynkowym ("CAŁA NAPRZÓD", 93 px w f13) ma 32 px do prawej krawedzi, wiersz
// menu ("SŁOŃCE + MIN.", 85 px w f11) ma 26 px do kropki trybu aktywnego, a kolumna
// MOC ("99,9 kW", 47 px w f11) ma 18 px do kolumny BATERIA.
constexpr int kW = 128;
constexpr int kMarginX = 3;

// Ekran spoczynkowy — linie bazowe (dolna krawedz liter, standard GFX).
constexpr int kIdleTagY = 8;     // "TRYB", f10
constexpr int kIdleModeY = 23;   // nazwa trybu, f13
constexpr int kIdleRuleY = 28;   // pozioma kreska
constexpr int kIdleLabY = 39;    // "MOC" / "BATERIA", f10
constexpr int kIdleValY = 50;    // wartosci, f11
constexpr int kIdleTextY = 62;   // zdanie o tym, co sie dzieje, f10
constexpr int kIdleCol2X = 68;   // lewa krawedz kolumny BATERIA

// Menu i test — cztery wiersze po 13 px, pierwszy zaczyna sie na y=12.
// Ostatni konczy sie dokladnie na y=63, czyli wypelnia ekran co do piksela.
constexpr int kRow0Y = 12;
constexpr int kRowH = 13;
constexpr int kMenuTextDX = 6;    // wciecie tekstu w wierszu menu
constexpr int kMenuDotX = 119;    // srodek kropki "tryb aktywny"
constexpr int kTestGpioX = 22;    // kolumna "GPIOnn" na ekranie testu
constexpr int kTestRightX = 125;  // prawa krawedz kolumny stanu przycisku

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

// Jedno zdanie o tym, CO SIE TERAZ DZIEJE — nie powtorka trybu, tylko jego skutek.
// Zamknieta lista, tak samo jak autoStateLabel() w AutoData.h i z tego samego
// powodu: napis ma sie zmiescic w 122 px, a gdyby przychodzil gotowy z Home
// Assistanta, kazde slowo dopisane po tamtej stronie potrafiloby rozwalic uklad.
// Najdluzsza pozycja to "ładowanie wyłączone" (97 px w f10) — 25 px zapasu.
const char* sceneLine(const AutoModel& a, bool fresh) {
  if (!fresh) return "brak danych z auta";
  // "laduje" SPRAWDZAMY PRZED kablem, mimo ze wyglada to na odwrotna kolejnosc.
  // Oba pola przychodza z tej samej wiadomosci, ale nie musza byc spojne (integracja
  // Tesli potrafi odswiezyc jedno przed drugim). Gdy sie klocza, wierzymy temu, co
  // mowi o PRZEPLYWIE ENERGII: napis "brak kabla" przy realnie ladujacym aucie
  // jest bledem, ktory wlasciciel zobaczy i ktoremu uwierzy.
  if (a.stateIs("laduje")) {
    if (a.modeIs("PV")) return "ładuje ze słońca";
    if (a.modeIs("PV+MIN")) return "słońce + minimum";
    if (a.modeIs("MAX")) return "ładuje pełną mocą";
    return "ładuje";
  }
  if (!a.cable) return "brak kabla";
  if (a.stateIs("czeka")) return a.modeIs("PV") ? "czeka na słońce" : "czeka na warunki";
  if (a.stateIs("stoi")) return a.modeIs("OFF") ? "ładowanie wyłączone" : "postój, nie ładuje";
  if (a.stateIs("spi")) return "auto śpi";
  if (a.stateIs("brak")) return "brak kabla";
  return "stan nieznany";
}

void drawIdle(const AutoModel& a, bool fresh) {
  str(plex::f10(), "TRYB", kMarginX, kIdleTagY, true);

  const int act = fresh ? autoModeIndex(a.mode) : -1;
  str(plex::f13(), fresh ? autoModeLabel(act) : "brak danych", kMarginX, kIdleModeY, true);

  hline(0, kW - 1, kIdleRuleY, true);

  str(plex::f10(), "MOC", kMarginX, kIdleLabY, true);
  str(plex::f10(), "BATERIA", kIdleCol2X, kIdleLabY, true);

  char num[12];
  char val[16];
  if (fresh) {
    fmt1(num, sizeof(num), a.kw);   // polski przecinek dziesietny — Format.h
    snprintf(val, sizeof(val), "%s kW", num);
  } else {
    snprintf(val, sizeof(val), "-");
  }
  str(plex::f11(), val, kMarginX, kIdleValY, true);

  if (fresh) {
    snprintf(val, sizeof(val), "%u %%", static_cast<unsigned>(a.soc));
  } else {
    snprintf(val, sizeof(val), "-");
  }
  str(plex::f11(), val, kIdleCol2X, kIdleValY, true);

  str(plex::f10(), sceneLine(a, fresh), kMarginX, kIdleTextY, true);
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

  mix(gCursor);
  mix(gMsg);
  mix(fresh ? 1u : 0u);
  if (fresh) {
    mix(a.soc);
    mix(a.cable ? 1u : 0u);
    // Rzutowanie przez int32_t, bo moc bywa UJEMNA (oddawanie z auta): konwersja
    // ujemnego float-a wprost na uint32_t jest zachowaniem niezdefiniowanym.
    mix(static_cast<uint32_t>(static_cast<int32_t>(a.kw * 10.f)));
    for (const char* p = a.mode; *p != '\0'; ++p) mix(static_cast<uint8_t>(*p));
    for (const char* p = a.state; *p != '\0'; ++p) mix(static_cast<uint8_t>(*p));
  }
  return h;
}

void renderPage(const AutoModel& a, bool fresh) {
  memset(gPage + 1, 0, sizeof(gPage) - 1);   // gPage[0] to bajt sterujacy — zostaje
  switch (gScr) {
    case SCR_MENU: drawMenu(a, fresh); break;
    case SCR_TEST: drawTest(); break;
    default:       drawIdle(a, fresh); break;
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

// Akcja przypisana do PUSZCZENIA przycisku (patrz pollButtons — i tam jest
// uzasadnienie, dlaczego nie do wcisniecia).
void onKey(uint8_t i, uint32_t now, const AutoModel& a) {
  // W tescie przyciski TYLKO sie pokazuja. (v178) Puszczenie, ktore z tego ekranu
  // WYCHODZI, w ogole tu nie dociera — pollButtons() obsluguje je u siebie i celowo
  // nie wola onKey(), zeby wyjscie z testu nie zmienilo przy okazji trybu ladowania.
  if (gScr == SCR_TEST) return;

  if (gScr == SCR_IDLE) {
    // Pierwsze nacisniecie BUDZI menu i celowo NICZEGO nie wybiera: wlasciciel
    // siega do panelu, nie wiedzac, ktory przycisk trzyma pod palcem, wiec
    // wykonanie akcji "od razu" bylo by losowaniem.
    gScr = SCR_MENU;
    const int act = autoModeIndex(a.mode);
    gCursor = (act >= 0) ? static_cast<uint8_t>(act) : 0;
    gMsg = MSG_NONE;
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
  // rdzen niz ten, ktory tu liczy.
  const bool fresh = (a.atMs != 0) &&
                     (static_cast<int32_t>(now - a.atMs) <
                      static_cast<int32_t>(cfg::AUTO_STALE_MS));

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

const char* screenName() {
  switch (gScr) {
    case SCR_MENU: return "menu";
    case SCR_TEST: return "test";
    default: return "spoczynek";
  }
}

}  // namespace oled
