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
    0xA1, 0xC8,  // obrot 180 stopni
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
void sendPage(uint8_t page) {
  cmd(static_cast<uint8_t>(0xB0 | page));  // wybor strony
  cmd(0x00);                               // kolumna 0, mlodsze cztery bity
  cmd(0x10);                               // kolumna 0, starsze cztery bity
  xfer(gPage, sizeof(gPage));
  ++gPagesSent;
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

// Diagnostyka mapowania przyciskow. Wchodzi sie tu PRZYTRZYMANIEM DOWOLNEGO
// przycisku, bo tego, ktory jest ktory, wlasnie NIE WIEMY — wejscie zalezne od
// konkretnego guzika bylo by pytaniem o odpowiedz, ktorej szukamy.
void drawTest() {
  str(plex::f10(), "TEST PRZYCISKÓW", kMarginX, kIdleTagY, true);
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
  if (gScr == SCR_TEST) return;  // w tescie przyciski TYLKO sie pokazuja

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

// AKCJE LECA NA PUSZCZENIE, NIE NA WCISNIECIE — i to jest decyzja o bezpieczenstwie,
// nie o wygodzie. Wejscie w ekran testu wymaga PRZYTRZYMANIA dowolnego przycisku,
// wiec przy akcjach na wcisnieciu przytrzymanie tego, ktory okaze sie "zatwierdz",
// najpierw WYSLALOBY zmiane trybu do auta, a dopiero potem otworzylo diagnostyke.
// Jedna regula — "akcja przy puszczeniu, jesli trzymanie bylo krotsze niz
// cfg::OLED_TEST_HOLD_MS" — usuwa ten przypadek w calosci i nie ma wyjatkow.
void pollButtons(uint32_t now, const AutoModel& a) {
  for (uint8_t i = 0; i < 4; ++i) {
    const bool down = digitalRead(kPins[i]) == LOW;  // zwiera do masy, INPUT_PULLUP
    const bool was = (gDown & (1u << i)) != 0;

    if (down == was) {
      // Trzymany dostatecznie dlugo -> ekran testu. gSwallow zaslania WSZYSTKIE
      // cztery przyciski, bo wlasciciel moze puscic je w dowolnej kolejnosci,
      // a zadne z tych puszczen nie ma juz nic wykonac.
      if (down && gScr != SCR_TEST &&
          (now - gDownAt[i]) >= cfg::OLED_TEST_HOLD_MS) {
        gScr = SCR_TEST;
        gMsg = MSG_NONE;
        gLastKeyMs = now;
        gSwallow = 0x0F;
      }
      continue;
    }

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
    if (!swallowed && (now - gDownAt[i]) < cfg::OLED_TEST_HOLD_MS) {
      onKey(i, now, a);
    }
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
  // Z testu wychodzimy dopiero, gdy przez 10 s nic nie jest wciskane ANI trzymane —
  // inaczej trzymanie guzika, ktore wlasnie sie sprawdza, wyrzucaloby z ekranu.
  if (gScr == SCR_TEST && gDown == 0 && (now - gLastKeyMs) >= cfg::OLED_TEST_EXIT_MS) {
    gScr = SCR_IDLE;
    // KASUJEMY ZASLONE. Wejscie w test ustawia ja na WSZYSTKICH czterech przyciskach
    // (bo puscic je mozna w dowolnej kolejnosci), a zdejmuje ja dopiero puszczenie
    // DANEGO przycisku. Bez tej linii bity trzech nienacisnietych zostawaly by
    // zapalone i po powrocie do spoczynku POLKNELY BY pierwsze nacisniecie kazdego
    // z nich — czyli menu nie otworzyloby sie za pierwszym razem. Bezpieczne tutaj,
    // bo do tego miejsca dochodzimy wylacznie przy gDown == 0, czyli gdy nie ma juz
    // ani jednego puszczenia do obsluzenia.
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

  pollButtons(now, a);
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

const char* screenName() {
  switch (gScr) {
    case SCR_MENU: return "menu";
    case SCR_TEST: return "test";
    default: return "spoczynek";
  }
}

}  // namespace oled
