#pragma once

#include <TFT_eSPI.h>
#include <WiFiClient.h>

#include "AirData.h"
#include "FlightData.h"
#include "PvData.h"
#include "WeatherData.h"
#include "Viessmann.h"

enum class AlertKind : uint8_t {
  NONE = 0,
  STORM,
  WIND,
  FROST,
  HEAT,
  HEAVY_RAIN,
  PV_FAULT,
  PV_OFFLINE,
  // Wartownik. lastAlertAt[] w .ino jest indeksowane wprost wartoscia tego enuma,
  // a mialo zaszyte [8] — dokladnie tyle, ile jest pozycji, wiec tablica wygladala
  // na zapasowa, a byla wypelniona co do miejsca. Dziewiaty alert (np. SNOW — kody
  // WMO 71/73/75 sa juz obslugiwane w ikonach) zapisalby 4 bajty poza tablice
  // globalna, wprost w sasiedni symbol w .bss. Objaw: losowo psujaca sie pogoda
  // albo alert, ktory nie chce zgasnac. Nie do powiazania z przyczyna.
  COUNT
};

struct Alert {
  AlertKind kind = AlertKind::NONE;
  char title[24] = {};
  char text[48] = {};
  uint16_t color = 0;
  int iconCode = -1;  // kod WMO do ikony, -1 = brak
};

class WeatherUi {
 public:
  bool begin();

  void drawBoot(const char* status, int attempt);
  void drawFatal(const char* msg);
  void drawColorTest();
  void drawSetup(const char* apSsid, const char* apPass, const char* apIp);
  void drawOta(int progress, const char* msg);
  void drawNetInfo(const char* ssid, const char* ip, int rssi, int secsLeft, int total);
  // drawLedTest() usuniety w v106 — patrz komentarz w WeatherUi.cpp. Autotest diody
  // zyje dalej w Led.cpp i nie zabiera juz ekranu.

  // Na czas OTA oddajemy bufor ekranu — inaczej TLS + pobieranie 1,3 MB
  // nie mają z czego działać.
  void releaseBuffer(bool clearScreen = true);

  // Zrzut ekranu do przegladarki: BMP 320x240 24-bit, wysylany wierszami.
  // Rysuje ekran od nowa do wlasnego, malego sprite'a — nie dotyka bufora
  // wyswietlacza, wiec obraz na TFT sie nie zatrzymuje.
  void streamScreenshot(WiFiClient& client, const WeatherModel& w, const PvModel& pv,
                        const PvHistory& hist, const FlightModel& fl, bool wifiOk);
  void drawFooterTo(TFT_eSPI& dst, const PvModel& pv, bool wifiOk);
  bool restoreBuffer();  // odtwarza bufor po zakończonym OTA
  void drawOtaDirect(int progress, const char* msg);

  // Główna pętla rysowania. Zwraca true, jeśli coś się animuje (potrzebne szybkie klatki).
  bool render(const WeatherModel& w, const PvModel& pv, const PvHistory& hist,
              const FlightModel& fl, bool wifiOk, uint32_t nowMs);

  // Czy zadanie sieciowe ma teraz odswiezac loty (ekran aktywny lub zaraz bedzie).
  bool needsFlights(uint32_t nowMs) const;

  // Klatka jest "spokojna": nie trwa przejscie ani alert. Wtedy kolejne klatki
  // roznia sie kosmetycznie, wiec mozna czytac bufor bez zatrzymywania rysowania.
  bool stableFrame() const { return !transitioning_ && !alertActive_; }

  // Podglad w przegladarce: przypiecie ekranu (idx < 0 = rotacja automatyczna).
  void pinView(int idx);

  // Dotyk GPIO7: odliczanie bieżącego ekranu startuje od nowa. Nie zatrzymuje
  // rotacji na stałe — po prostu przedłuża to, na co patrzysz.
  void restartHold() { viewStart_ = millis(); }

  // Podwójne dotknięcie: krok wstecz. Pomija ekrany, które i tak są wyłączone
  // z rotacji (radar bez opadu, "w domu" bez czujników) — inaczej cofnięcie
  // trafiałoby w pustą kartę.
  void prevView();

  // Historia 24 h z czujnikow BLE. Wskaznik, a nie kopia — struktura ma 1,7 kB,
  // a przewlekanie jej przez render/paintFrame/drawView tylko po to, zeby doszla
  // do jednego widoku, zasmiecaloby cztery sygnatury.
  void setRoomHistory(const struct RoomHistory* rh) { rooms_ = rh; }
  void setBoiler(const vi::Model* b) { boiler_ = b; }
  void setBurnerHistory(const struct BurnerHistory* h) { burner_ = h; }
  // Jakosc powietrza (v117) — ten sam wzorzec co rooms_/boiler_/burner_ powyzej:
  // wskaznik do kopii aktualizowanej przez loop() pod gLock, a NIE parametr w
  // render()/drawView() — ekran POWIETRZE nie potrzebuje watku danych az tak
  // centralnego jak pogoda/PV/loty (brak prefetchu, brak wplywu na inne widoki).
  void setAir(const struct AirModel* a) { air_ = a; }
  void viewState(int& cur, int& pin) const {
    cur = view_;
    pin = pinned_;
  }

  void raiseAlert(const Alert& a, uint32_t nowMs);
  void setBacklightTarget(uint8_t v) {
    // Wymuszenie z panelu (testBacklight) ma pierwszenstwo nad automatem z LDR —
    // inaczej petla loop() nadpisywalaby test w nastepnej klatce i nie dalo by sie
    // niczego sprawdzic zdalnie.
    if (blForceUntil_ != 0) return;
    blTarget_ = v;
  }

  // Diagnostyka podswietlenia: co kod REALNIE wystawia na PWM. Wlasciciel zglosil,
  // ze ekran "nie przyciemnia sie w ogole" — a kod wyglada poprawnie. Te dwie liczby
  // rozstrzygaja spor: jesli blCurrent spada do 45, a ekran swieci pelnia, to znaczy
  // ze pin podswietlenia NIE jest sterowany z GPIO (np. wpiety na stale do zasilania)
  // i zaden software tego nie naprawi. Bez tego pomiaru zgadywalibysmy.
  uint8_t backlightCurrent() const { return blCurrent_; }
  uint8_t backlightTarget() const { return blTarget_; }

  // Test sprzetu: wymus jasnosc na `ms` milisekund, potem wroc do automatu z LDR.
  // Ograniczone czasowo CELOWO — urzadzenie wisi w lazience bez klawiatury, wiec
  // pomylkowe ustawienie 0 na stale zostawiloby czarny ekran bez drogi powrotu.
  void testBacklight(uint8_t v, uint32_t ms) {
    blForceUntil_ = millis() + ms;
    blTarget_ = v;
  }
  void tickBacklight();

 private:
  TFT_eSPI tft_;
  TFT_eSprite spr_{&tft_};
  bool ready_ = false;
  bool freed_ = false;

  // --- RYSOWANIE W DWÓCH PASACH ---------------------------------------------
  // Bufor obejmuje tylko y=0..205 (belka + pasek + widoki); stopka PV idzie wprost
  // na TFT. Ale nawet 320x206x16bpp to 132 kB — sterta tego nie wytrzymywała
  // (heap_min_ever spadał do ~10 kB, dekoder PNG radaru nie miał gdzie się zmieścić).
  //
  // Dlatego sprite ma teraz tylko 320x103 = 66 kB i jest rysowany DWA RAZY na klatkę:
  // pas górny (y=0..102) i pas dolny (y=103..205), każdy wypychany osobno.
  // Kod rysujący nadal operuje na globalnych współrzędnych ekranu (0..205) —
  // przesunięcie i przycięcie robi viewport sprite'a (setViewport z vpDatum),
  // który TFT_eSPI honoruje we wszystkich prymitywach (drawPixel, fillRect,
  // drawFastH/VLine, drawLine, drawChar, readPixel — wszystkie są wirtualne).
  static constexpr int VIEW_H = 206;   // wirtualna wysokość obszaru rysowania (y=0..205)
  // Dwa pasy istniały tylko po to, żeby bufor miał 66 kB zamiast 132 kB — a to
  // było potrzebne tylko dlatego, że nie wiedzieliśmy o 2 MB PSRAM (v50).
  // Teraz bufor mieszka w PSRAM, więc rysujemy JEDEN raz zamiast dwa: pół roboty.
  static constexpr int BAND_H = VIEW_H;
  static constexpr int BAND_N = 1;

  // Zrzut ekranu rysujemy w wąskich paskach do własnego sprite'a (240 = 10 x 24),
  // żeby nie ruszać bufora wyświetlacza i nie zamrażać obrazu.
  static constexpr int SHOT_H = 24;

  // Ustawia sprite jako pas [top, top+bandH) w układzie globalnym o wysokości virtH.
  static void setBand(TFT_eSprite& s, int top, int virtH);

  // Rysuje pełną klatkę (tło + widok + belka + pasek) do wskazanego celu.
  // Cel sam decyduje, co z tego wpada w jego pas — tu rysujemy zawsze całość.
  //
  // heapNow: JEDNA klatka = JEDEN moment. paintFrame() leci raz na pas (2x na klatkę,
  // 10x na zrzut), a ekran statystyk pokazuje dane, które zmieniają się same z siebie
  // (millis(), wolny heap). Gdyby każdy pas czytał je od nowa, napis przecięty granicą
  // pasa pokazałby w górnej połowie inną wartość niż w dolnej — litery rozjechałyby się
  // w poziomie. Widać to zwłaszcza w zrzucie: między paskami leci transmisja BMP, więc
  // mijają setki ms. Dlatego nowMs i heapNow łapiemy RAZ, u wołającego, i wieziemy
  // przez stos (nie przez pole — render() i zrzut jadą na różnych rdzeniach).
  void paintFrame(TFT_eSPI& spr, const WeatherModel& w, const PvModel& pv,
                  const PvHistory& hist, const FlightModel& fl, bool wifiOk, uint32_t nowMs,
                  uint32_t heapNow);

  // Rysuje treść dwa razy (pas górny + dolny) i wypycha oba pasy na TFT.
  template <typename F>
  void pushBands(F&& paint);

  // stopka: rysujemy tylko gdy dane się zmieniły (inaczej migotałaby)
  int32_t lastAc_ = INT32_MIN;
  int32_t lastGrid_ = INT32_MIN;
  int lastKwh_ = -1;
  int lastCpu_ = -1000;
  bool lastOnline_ = false;
  bool lastAsleep_ = false;   // falownik uśpiony (noc) — inaczej stopka by nie odświeżyła
  bool footerInit_ = false;

  // rotacja widoków
  uint8_t view_ = 0;
  const struct RoomHistory* rooms_ = nullptr;
  const vi::Model* boiler_ = nullptr;
  const struct BurnerHistory* burner_ = nullptr;
  const struct AirModel* air_ = nullptr;
  int8_t pinned_ = -1;  // >=0: ekran zablokowany z panelu WWW
  uint8_t prevView_ = 0;
  uint32_t viewStart_ = 0;
  uint32_t enterStart_ = 0;
  uint32_t transStart_ = 0;
  bool transitioning_ = false;

  // alert
  Alert alert_{};
  bool alertActive_ = false;
  uint32_t alertStart_ = 0;

  // animowane liczniki
  float animAcW_ = 0.f;
  float animLoadW_ = 0.f;
  float animGridW_ = 0.f;

  // podświetlenie
  uint8_t blCurrent_ = 0;
  uint8_t blTarget_ = 255;
  // 0 = automat z LDR rzadzi. Niezerowe = trwa test z panelu, do tego millis().
  uint32_t blForceUntil_ = 0;

  // temperatura rdzenia ESP32-S3 (odczyt co 2 s)
  float cpuTempC_ = 0.f;
  uint32_t cpuTempAt_ = 0;

  // Rysowanie. Wszystkie te funkcje operują na GLOBALNYCH współrzędnych ekranu
  // (y=0..205) i nie wiedzą, w którym pasie są — przycina je viewport celu.
  void drawHeader(TFT_eSPI& spr, const WeatherModel& w, bool wifiOk, uint32_t nowMs);
  void drawProgress(TFT_eSPI& spr, uint32_t nowMs);
  void drawFooter(const PvModel& pv, bool wifiOk);
  void drawSysBoxTo(TFT_eSPI& dst, int y);
  void drawContentBg(TFT_eSPI& spr);
  void drawView(TFT_eSPI& spr, uint8_t view, int ox, float t, const WeatherModel& w,
                const PvModel& pv, const PvHistory& hist, const FlightModel& fl,
                uint32_t nowMs, uint32_t heapNow);
  // v114: ekran ozdobny (patrz WeatherUi.cpp, sekcja "WIDOK 0: RETRO"). Ma WLASNY
  // HUD gorny i dolny (stylizowane na 8-bit), wiec w odroznieniu od reszty widokow
  // NIE korzysta ze wspolnego drawHeader/drawProgress/drawFooter — paintFrame()
  // i render()/streamScreenshot() maja z tego powodu osobna galaz dla VIEW_RETRO.
  void drawViewRetro(TFT_eSPI& spr, int ox, float t, const WeatherModel& w, uint32_t nowMs);
  // Dolny pasek RETRO (WILGOC/WIATR/HPA) zajmuje dokladnie to miejsce co stopka PV
  // (y=206..239, poza buforem VIEW_H) — patrz drawFooterTo(). Ten sam wzorzec:
  // dowolny cel (TFT na żywo albo pasek zrzutu), zeby dzialalo w obu miejscach.
  void drawViewRetroFooter(TFT_eSPI& dst, const WeatherModel& w);
  void drawViewNow(TFT_eSPI& spr, int ox, float t, const WeatherModel& w);
  void drawViewHours(TFT_eSPI& spr, int ox, float t, const WeatherModel& w);
  void drawViewDays(TFT_eSPI& spr, int ox, float t, const WeatherModel& w);
  void drawViewPv(TFT_eSPI& spr, int ox, float t, const PvModel& pv, const PvHistory& hist);
  void drawViewFlights(TFT_eSPI& spr, int ox, float t, const FlightModel& fl);
  void drawViewRadar(TFT_eSPI& spr, int ox, float t, const WeatherModel& w, uint32_t nowMs);
  void drawViewHome(TFT_eSPI& spr, int ox, float t, const WeatherModel& w);
  void drawViewBoiler(TFT_eSPI& spr, int ox, float t);
  void drawGasChart(TFT_eSPI& spr, int ox, float e);
  // v111: dwa nowe ekrany eksploracyjne (obok VIEW_STATS, ktory zostaje nietkniety).
  // heapNow: ta sama zlapana-raz-u-wolajacego wartosc, co dostaje drawViewStats —
  // "SRAM wolny TERAZ" ma pokazywac identyczna liczbe na obu ekranach. nowMs
  // (tylko RUCH) sluzy do "ostatni ruch X temu" z tego samego powodu.
  void drawViewMem(TFT_eSPI& spr, int ox, float t, uint32_t heapNow);
  void drawViewMotion(TFT_eSPI& spr, int ox, float t, uint32_t nowMs);
  void drawViewStats(TFT_eSPI& spr, int ox, float t, uint32_t nowMs, uint32_t heapNow);
  // v117: jakosc powietrza. `w` — TYLKO do opcjonalnego wiersza porownania z nasza
  // prognoza (w.current.tempC); juz i tak jest w drawView(), wiec to przekazanie
  // niczego nie kosztuje. Bez nowMs — jedyna wartosc "zywa" na tym ekranie (wiek
  // ostatniej probki) liczy sie z epoch (time(nullptr)), nie z millis(), a to jest
  // ten sam rodzaj odczytu "zegara sciennego", co godzina podswietlanego slupka w
  // drawViewMotion — tam tez wolane swiezo, z tym samym uzasadnieniem (patrz tamten
  // komentarz): kosmetyczna niescislosc rzedu sekundy miedzy paskami zrzutu nikomu
  // nie szkodzi, a wciaganie TEGO parametru przez caly lancuch render/paintFrame/
  // drawView tylko po to nie byloby tego warte.
  void drawViewAir(TFT_eSPI& spr, int ox, float t, const WeatherModel& w);
  void drawAlert(TFT_eSPI& spr, float t);
  // Podtytuł (sub) niesie powód ciszy falownika — noc, nie awaria.
  void drawNoData(TFT_eSPI& spr, int ox, const char* msg, const char* sub = nullptr);
  uint32_t holdFor(uint8_t view) const;
};
