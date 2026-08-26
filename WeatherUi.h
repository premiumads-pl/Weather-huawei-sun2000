#pragma once

#include <TFT_eSPI.h>
#include <WiFiClient.h>

#include "AirData.h"
#include "AutoData.h"
#include "FlightData.h"
#include "PvData.h"
#include "RadarData.h"
#include "RoomData.h"
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

  // Zrzut ekranu do przegladarki: JPEG 320x240 enkodowany na urzadzeniu (JPEGENC, tablice
  // we flashu, ~zero statyku RAM; ~15-30 kB zamiast 230 kB BMP -- podglad w panelu plynny).
  // Awaryjnie, gdy PSRAM lub enkoder zawiedzie, wysyla nieskompresowany BMP 24-bit
  // wierszami (stara sciezka). Rysuje ekran od nowa do wlasnego, malego sprite'a — nie
  // dotyka bufora wyswietlacza, wiec obraz na TFT sie nie zatrzymuje.
  void streamScreenshot(WiFiClient& client, const WeatherModel& w, const PvModel& pv,
                        const PvHistory& hist, const FlightModel& fl, bool wifiOk);
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

  // --- NAWIGACJA DOTYKIEM V3 "Pasmowy" (spec 7a) ----------------------------
  // (v160) JEDYNA nawigacja dotykiem. Do v159 stal obok niej wariant V1/V2
  // (restartHold/prevView); zniknal razem z tamtymi motywami.
  // 1x stukniecie: nastepny ekran w PETLI 8 widokow (GLOWNY->RADAR->5 DNI->PRAD->
  // POKOJE->OGRZEWANIE->POWIETRZE->SAMOLOTY->GLOWNY), z pominieciem niedostepnych
  // (viewSkipped). Bedac w diagnostyce (STATS/MEM) przelacza miedzy nimi.
  void touchTapV3();
  // 2x stukniecie: wejscie/wyjscie z diagnostyki. Z dowolnego widoku -> STATS
  // (diag 1 "zrodla"); bedac w STATS/MEM -> GLOWNY.
  void touchDoubleV3();
  // Znacznik SUROWEGO dotyku (przed rozroznieniem 1x/2x) — do kropki feedbacku V3.
  // Wolane z petli, gdy touch::pressedRaw(). Samo ustawia czas; kropke rysuje drawV3.
  void noteRawTouch() { rawTouchMs_ = millis(); }

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
  // (v174) Stan auta z MQTT — TEN SAM wzorzec, co setAir() wyzej: wskaznik do kopii
  // odswiezanej w loop() (uiAuto w pogoda-gdynia.ino), a nie kolejny parametr
  // render()/drawV3(). Powod bez zmian: jeden ekran nie jest wart przewleczenia
  // czwartego modelu przez cztery sygnatury.
  void setAuto(const struct AutoModel* a) { auto_ = a; }
  // (v180) Koszt energii kupionej z sieci od polnocy — TA SAMA sciezka, co setAuto()
  // wyzej, i z tego samego powodu. Odbiorca jest JEDEN wiersz w module PRAD na ekranie
  // glownym (mainPvModule), wiec dokladanie piatego modelu do sygnatur render() /
  // paintFrame() / drawView() / drawV3 byloby czterema zmianami za jedna linie tekstu.
  void setCost(const struct CostModel* c) { cost_ = c; }

  // v126: modele POSREDNIE — gotowe wiersze/liczby dla dwoch ekranow, ktore do
  // v125 same siegaly po singletony w trakcie rysowania (patrz RoomData.h i
  // RadarData.h). Ten sam wzorzec przekazania co rooms_/boiler_/air_ powyzej:
  // wskaznik do struktury odswiezanej w loop(), a NIE kolejny parametr w
  // render()/paintFrame()/drawView(). Powod ten sam, co przy RoomHistory:
  // dwa modele dla dwoch widokow zasmiecilyby cztery sygnatury, przez ktore
  // musialyby przejechac.
  void setRoomModel(const struct RoomModel* r) { roomModel_ = r; }
  void setRadarModel(const struct RadarViewModel* r) { radarModel_ = r; }
  void viewState(int& cur, int& pin) const {
    cur = view_;
    pin = pinned_;
  }

  // Czy ekran `i` jest pomijany w rotacji (radar bez opadu, "w domu" bez czujnikow,
  // piec bez autoryzacji, powietrze bez danych z obu stacji, AUTO bez swiezej
  // wiadomosci MQTT, ZWROT bez danych I bez historii) — JEDYNE miejsce z tymi szescioma
  // warunkami, zeby definicja "pomijany" nie rozjechala sie miedzy rotacja, nawigacja
  // dotykiem i paskiem postepu V3. Statyczna celowo — v3ProgressPos() liczy z niej
  // pozycje "x z y" bez potrzeby stanu instancji, dlatego modele wchodza argumentem.
  // (v181) `cost` doszedl razem z ekranem ZWROT. Domyslnego nullptr NIE MA i nie bedzie:
  // wolajacy ma podac model swiadomie — pominiety argument znaczylby po cichu "ten ekran
  // nie ma danych", czyli dokladnie ten rodzaj bledu, ktory static_assert w Config.h
  // i straznik w capture_screens.py maja w tym projekcie wylapywac.
  static bool viewSkipped(int i, const struct AirModel* air, const struct AutoModel* au,
                          const struct CostModel* cost);

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

  // --- WIZUALNY test podswietlenia (v124) -----------------------------------
  // Sam podglad liczby w /api/diag nie rozstrzygal sporu "czy to w ogole dziala":
  // firmware pokazywal 45, a wlasciciel widzial jasny ekran. Trzeba bylo wyjsc
  // z API na SAM EKRAN — duza liczba PWM plus rampa w gore i w dol, zeby dalo sie
  // porownac to, co kod TWIERDZI, z tym, co oko WIDZI. Jesli liczba spada, a jasnosc
  // stoi — pin nie jest sterowany i zaden software tego nie naprawi.
  //
  // Ograniczony czasowo (jak testBacklight wyzej): urzadzenie nie ma klawiatury,
  // wiec tryb testowy MUSI sam sie skonczyc.
  void startBacklightSweep(uint32_t ms);
  bool backlightSweepActive(uint32_t nowMs) const {
    return blSweepUntil_ != 0 && static_cast<int32_t>(nowMs - blSweepUntil_) < 0;
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

  // rotacja widoków
  uint8_t view_ = 0;
  const struct RoomHistory* rooms_ = nullptr;
  const vi::Model* boiler_ = nullptr;
  const struct BurnerHistory* burner_ = nullptr;
  const struct AirModel* air_ = nullptr;
  const struct AutoModel* auto_ = nullptr;   // (v174) stan Tesli z MQTT (ekran AUTO)
  const struct CostModel* cost_ = nullptr;   // (v180) koszt zakupu z sieci (modul PRAD)
  // Gotowe modele dla W DOMU i RADAR (v126). nullptr = warstwa danych jeszcze ich
  // nie podpiela; rysowanie uzywa wtedy pustej struktury, czyli zachowuje sie tak,
  // jakby nie bylo czujnikow / klatek.
  const struct RoomModel* roomModel_ = nullptr;
  const struct RadarViewModel* radarModel_ = nullptr;
  int8_t pinned_ = -1;  // >=0: ekran zablokowany z panelu WWW
  uint8_t prevView_ = 0;
  // V3 "Pasmowy" (spec 7a): czas ostatniego STUKNIECIA (do powrotu na GLOWNY po
  // 60 s ciszy) i ostatniego SUROWEGO dotyku (do kropki feedbacku). 0 = brak.
  // Czytane WYLACZNIE przy theme==3 — V1/V2 ich nie dotykaja (patrz render()).
  uint32_t lastTouchMs_ = 0;
  uint32_t rawTouchMs_ = 0;
  // TRYB NOCNY "dotyk budzi ekran" (runtime, NIE zmienia nightStartH/EndH/blNight): true, gdy
  // render() rysuje TERAZ przygaszony zegar nocny i czeka na wybudzenie. Ustawia je render()
  // co klatke; czyta touchTapV3/touchDoubleV3 — pierwszy dotyk w nocy ma WYBUDZIC na Glowny
  // (jasnosc kNightWakeBl), a nie przeskoczyc ekranu. Patrz render()/isNightNow().
  bool nightAsleep_ = false;
  // (v158) true, gdy OSTATNIE pojedyncze stukniecie posluzylo do WYBUDZENIA ekranu
  // nocnego (a nie do nawigacji). Potrzebne, odkad touch::poll() zglasza SINGLE
  // natychmiast: dla gestu podwojnego przychodzi teraz SINGLE, a zaraz po nim
  // DOUBLE, wiec bez tej flagi podwojne stukniecie w nocy najpierw budzilo ekran,
  // a potem od razu wchodzilo w diagnostyke — czyli lamalo ustalenie wlasciciela
  // "pierwsza interakcja w nocy TYLKO wybudza". Kasowana przez touchDoubleV3()
  // (ktore wtedy nic wiecej nie robi) i przez kolejne stukniecie juz wybudzonego
  // ekranu. Sam bool wystarczy — DOUBLE moze przyjsc wylacznie w oknie kDoubleMs
  // po tym SINGLE, wiec nie ma czego przeterminowywac czasem.
  bool v3WokeByTap_ = false;
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
  // Wizualny test rampy (startBacklightSweep): do kiedy trwa i od kiedy liczymy faze.
  uint32_t blSweepUntil_ = 0;
  uint32_t blSweepStart_ = 0;

  // V3: sygnatura ostatnio narysowanej tresci. Gdy kolejna klatka ma te sama, render()
  // NIE przerysowuje ekranu (patrz komentarz w render()) — inaczej jasny bufor V3 byl
  // wypychany 20-30x/s i na ST7789 widac bylo ciagle odswiezanie. 0xFFFFFFFF = "wymus".
  uint32_t v3Sig_ = 0xFFFFFFFFu;

  // temperatura rdzenia ESP32-S3 (odczyt co 2 s)
  float cpuTempC_ = 0.f;
  uint32_t cpuTempAt_ = 0;

  // Rysowanie. Wszystkie te funkcje operują na GLOBALNYCH współrzędnych ekranu
  // (y=0..205) i nie wiedzą, w którym pasie są — przycina je viewport celu.
  void drawContentBg(TFT_eSPI& spr);

  // (v162) EKRANY RETRO (Mario, slot 0) i GODZINY (slot 2) SA SKASOWANE W CALOSCI —
  // razem z drawViewRetro()/drawViewRetroFooter(), paleta rcol::, fontem i sprite'ami
  // (RetroFont.h/RetroSprites.h — pliki usuniete). Byly nieosiagalne od v160: petla
  // kV3Loop ich nie zawiera, a funkcje rysujace GODZINY zniknely juz razem z motywami.
  // Numery 0 i 2 zostaly ZAREZERWOWANE, nie zwolnione — uzasadnienie przy cfg::VIEW_*
  // w Config.h (numer widoku wychodzi na zewnatrz przez /api/view).

  // Motyw V3 "Pasmowy" (WeatherUiV3.cpp). JEDEN dispatcher rysuje obszar sprite
  // (y=0..205), drugi — dolny pas (206..239) wprost na TFT, bo uklad V3 siega pelnej
  // wysokosci i nie ma stopki PV jak V1/V2 (patrz ThemeV3.h). Reszta ekranow to
  // file-static helpery w tamtym pliku; dostep do modeli (air_/roomModel_/...) idzie
  // przez argumenty tych dwoch metod, wiec naglowek nie puchnie o 13 deklaracji.
  void drawV3(TFT_eSPI& spr, uint8_t view, int ox, float t, const WeatherModel& w,
              const PvModel& pv, const PvHistory& hist, const FlightModel& fl,
              uint32_t nowMs, uint32_t heapNow);
  // (v164) `hist` takze tutaj: dolny pas ekranu PRAD liczy autokonsumpcje z tego
  // samego profilu doby, co paski bilansu w v3Pv — przekazujemy model przez argument
  // (jak wszystkie inne), a nie przez pole/cache, bo dolny pas rysuja tez zrzuty
  // BMP/JPEG z drugiego rdzenia i zaden stan wspolny nie moze tu mutowac.
  void drawV3Bottom(TFT_eSPI& tft, uint8_t view, const WeatherModel& w, const PvModel& pv,
                    const PvHistory& hist, const FlightModel& fl, uint32_t nowMs,
                    uint32_t heapNow);
  // Czy teraz noc "do zwiniecia ekranu" (ciemno == blTarget na poziomie blNight + pora nocna
  // z okna nightStartH/EndH). Definicja w WeatherUiV3.cpp. Metoda, a nie file-static tamtego
  // pliku, bo wolaja ja ZAROWNO drawV3/drawV3Bottom (zegar nocny) jak i render() (tryb nocny
  // "dotyk budzi ekran"). const — czysty odczyt time()/settings().
  bool isNightNow(uint8_t blTarget) const;
  // Pozycja biezacego widoku w petli V3 (kV3Loop) wsrod niepomijanych — zrodlo dla
  // paska postepu (2 px) rysowanego na gorze drawV3(). false = widok spoza petli
  // (diagnostyka) => paska nie rysujemy. Definicja w WeatherUi.cpp, gdzie zyje kV3Loop.
  bool v3ProgressPos(int& cur, int& total) const;
  // Plansza zdarzenia w stylu V3 (makiety 13/18/19): ciemne tlo, glif/trojkat po lewej,
  // tytul + tekst alertu po prawej, akcent kolorem alert_.color. Rysowana zamiast drawV3()
  // gdy alertActive_ (patrz paintFrame). `t` = postep wejscia planszy (260 ms).
  void drawV3Alert(TFT_eSPI& spr, float t);
  void drawBacklightSweep(TFT_eSPI& spr, uint32_t nowMs);
  uint32_t holdFor(uint8_t view) const;

  // V3: ustaw widok NATYCHMIAST (bez slajdu — V3 tnie, patrz paintFrame), zresetuj
  // liczniki wejscia i wymus przerysowanie. Zdejmuje pin z panelu (dotyk przejmuje
  // sterowanie) i gasi ewentualny alert (jak pinView).
  void setViewV3(uint8_t v);
};
