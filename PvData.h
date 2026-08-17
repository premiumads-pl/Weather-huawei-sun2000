#pragma once

#include <cstdint>
// <time.h>, nie <ctime>: potrzebujemy localtime_r, ktore jest funkcja POSIX,
// a nie czescia standardu C — <ctime> nie ma obowiazku jej deklarowac.
#include <time.h>

struct PvSnapshot {
  int32_t powerDcW = 0;
  int32_t powerAcW = 0;
  int32_t gridPowerW = 0;   // + oddawanie do sieci / - pobór
  int32_t houseLoadW = 0;   // wyliczone: AC - grid
  float energyTotalKwh = 0.f;
  float energyTodayKwh = 0.f;
  float pvVoltageV = 0.f;
  float inverterTempC = 0.f;
  float efficiencyPct = 0.f;
  uint16_t statusCode = 0;  // rejestr 32089
  bool meterOk = false;

  // --- (v165) LICZNIKI ENERGII MIERNIKA, NARASTAJACE OD ZAWSZE [kWh] ----------
  // Rejestry 37119 i 37121, int32 ze znakiem, gain 100 — ta sama skala co
  // 32106/32114. Numery i KIERUNEK rozpoznane empirycznie na zywym falowniku
  // (dowod w komentarzu przy odczycie w PvClient.cpp), bo mapy miernikow roznia
  // sie miedzy modelami, a nazwa "positive active energy" nie mowi, w ktora
  // strone plynie prad.
  //
  // WARTOWNIK -1 (a nie 0) I TO JEST ISTOTNE. Zero jest tu PRAWDZIWA, mozliwa
  // wartoscia licznika (swiezo wymieniony miernik), wiec po zerze nie da sie
  // odroznic "nie doszlo" od "doszlo i wynosi zero" — a wtedy nieudany odczyt
  // wyszedlby na ekran jako pomiar. Dokladnie ta pulapka, ktora PvClient.cpp
  // opisuje przy pvVoltageV/inverterTempC/efficiencyPct ("dwa wartowniki
  // z pieciu"); tamtych tu NIE naprawiamy, ale nowych dziur nie dokladamy.
  float meterExportKwh = -1.f;  // rej. 37119 — ODDANE do sieci, narastajaco
  float meterImportKwh = -1.f;  // rej. 37121 — POBRANE z sieci, narastajaco
  // Czy TEN cykl przyniosl swiezy odczyt obu licznikow. Gdy false, a wartosci
  // sa >= 0, to znaczy, ze zostaly przeniesione z poprzedniego udanego odczytu
  // (netTask, zasada z v161: nieudany odczyt nie kasuje modelu). Roznica dla
  // energii dobowej jest ponizej 0,03 kWh (0,3 kW x 5 min nocnej kadencji),
  // wiec to bezpieczne — ale wiek ma byc widoczny w /api/diag, nie zamiatany.
  bool meterEnergyFresh = false;

  // --- (v165) "DZIS" z licznikow: biezacy odczyt minus baza z polnocy --------
  // Liczone w netTask przez pvMeterUpdate() (nizej) i wstawiane TU, a nie
  // liczone w UI — dzieki temu ekran, /api/diag i ewentualni przyszli
  // konsumenci widza JEDNA liczbe, a sygnatury funkcji rysujacych nie musza
  // wozic dodatkowej struktury przez caly dispatcher.
  // meterTodayOk == false => tych dwoch pol NIE WOLNO pokazac (baza niepelna,
  // brak NTP, licznik sie cofnal, miernik nigdy nie odpowiedzial). Ekran wraca
  // wtedy do calki z profilu doby, czyli do zachowania v164.
  float meterTodayExportKwh = 0.f;
  float meterTodayImportKwh = 0.f;
  bool meterTodayOk = false;

  bool valid = false;
};

struct PvModel {
  PvSnapshot data{};
  char errorMsg[48] = {};
  bool online = false;
  // Noc: falownik nie odpowiada, ale ma do tego prawo (Huawei wyłącza Modbus TCP
  // po zachodzie). Stan neutralny — szary, nie czerwony. Patrz pvMayBeAsleep().
  bool asleep = false;
};

// ================= (v165) BAZA LICZNIKOW Z OSTATNIEJ POLNOCY ==================
// Liczniki miernika sa NARASTAJACE od zawsze, wiec "dzis" = odczyt biezacy minus
// odczyt z ostatniej polnocy. Ta baza musi przezyc RESTART **oraz ZANIK
// ZASILANIA**, dlatego idzie do NVS, a NIE do pamieci RTC: 16.08.2026 wlasciciel
// wylaczyl zasilanie i wszystkie zbiory RTC wystartowaly od zera. RTC przezywa
// restart i OTA, nie przezywa odciecia napiecia.
//
// DLACZEGO ROK, A NIE SAM tm_yday: sam tm_yday jest dwuznaczny na przelomie roku
// i przy dluzszej przerwie. 1 stycznia yday wraca do 0 — porownanie "!=" wykryje
// zmiane dnia poprawnie, ale baza sprzed ROKU (urzadzenie lezalo w szufladzie)
// trafilaby w ten sam yday i roznica wyszlaby jako roczne zuzycie podpisane
// slowem "dzis". Rok to 4 bajty, ktore ten przypadek wykluczaja.
//
// `minute` (minuta doby lokalnej, w ktorej baze zlapano) jest tu po to, zeby
// odroznic baze ZLAPANA O POLNOCY od zlapanej w srodku dnia. Bez tego pola
// pierwszy odczyt po porannym starcie urzadzenia ustawilby baze o 08:00 i
// "z sieci dzis" po cichu pomijaloby cala noc — czyli DOKLADNIE ten blad, ktory
// to wydanie usuwa, tylko innym mechanizmem.
// Co zrobila pvMeterUpdate() z baza — do zalogowania i do decyzji o zapisie NVS.
// (v169) Definicja WYZEJ niz PvMeterBase, bo baza pamieta teraz zdarzenie, ktore ja
// ustawilo (pole `event`) — inaczej po dobie nie da sie powiedziec, czy stoi tam
// normalne przestawienie o polnocy, czy pierwsza baza po aktualizacji.
enum class PvBaseEvent : uint8_t {
  NONE,       // baza bez zmian (typowy cykl w srodku doby)
  SET_FIRST,  // baza nie istniala — pierwszy start po aktualizacji
  ROLLED,     // zmiana daty: normalne przestawienie bazy na nowa dobe
  WENT_BACK,  // licznik zmalal (wymiana miernika, reset, przepelnienie)
};

struct PvMeterBase {
  int32_t year = 0;         // tm_year + 1900
  int32_t yday = -1;        // tm_yday dnia, KTOREGO dotyczy baza
  float importKwh = -1.f;   // stan rej. 37121 o polnocy
  float exportKwh = -1.f;   // stan rej. 37119 o polnocy
  int16_t minute = -1;      // minuta doby lokalnej zlapania bazy (0..1439)
  // (v169) ODLEGLOSC OD POLNOCY W MINUTACH, ZE ZNAKIEM. Ujemna = baze wziesto
  // z odczytu SPRZED polnocy (wczoraj o 23:56 => -4), dodatnia = z odczytu po
  // polnocy (02:23 => +143). Samo `minute` tego nie rozstrzyga: 1436 to zarowno
  // "wczoraj tuz przed polnoca" (baza bardzo dobra), jak i "dzis o 23:56" (baza
  // bezuzyteczna) — a to jest DOKLADNIE ta roznica, ktora decyduje o `full`.
  int16_t offsetMin = 0;
  bool valid = false;       // baza w ogole istnieje
  // Czy baze zlapano dosc blisko polnocy, zeby roznica opisywala CALA dobe.
  // false => ekran NIE pokazuje liczb z miernika i wraca do calki z v164.
  bool full = false;
  // Zdarzenie, ktore ustawilo te baze (PvBaseEvent jako uint8, zeby blob NVS mial
  // jawny rozmiar pola). Tylko do diagnostyki — logika nigdzie po tym nie rozgalezia.
  uint8_t event = 0;

  // --- (v169) OSTATNI UDANY ODCZYT LICZNIKOW — SERCE NAPRAWY Z TEGO WYDANIA ------
  // Do v168 baza mogla powstac WYLACZNIE z odczytu wykonanego JUZ PO polnocy, bo
  // pvMeterUpdate() jest wolana tylko z galezi `if (ok)` w netTask, czyli po w pelni
  // udanym cyklu Modbus. Kazdy nieudany cykl przesuwal wiec baze w glab nocy, a raz
  // ustawiona baza z `full == false` zostawala taka do NASTEPNEJ polnocy — jedna
  // spozniona chwila psula CALA dobe. Zmierzone na urzadzeniu 17.08.2026: baza
  // o 02:23, czyli 143 minuty za progiem 30 min, i `today_ok: false` przez caly dzien.
  //
  // Liczniki miernika sa NARASTAJACE, wiec odczyt sprzed polnocy jest rownie dobra
  // baza, co odczyt po polnocy — a zwykle LEPSZA, bo wypada blizej polnocy.
  // Zapamietujemy go przy KAZDYM udanym odczycie i przy przelomie doby wybieramy
  // to z dwoch, co jest BLIZEJ polnocy.
  //
  // CO KOMU PRZYPISUJEMY: energia zuzyta miedzy odczytem-baza a sama polnoca ladu-
  // je w dobie NOWEJ (bo od tego punktu liczymy "dzis"). Przy nocnym poborze domu
  // 0,2-0,3 kW i progu 30 min to najwyzej 0,15 kWh na ~13 kWh doby, czyli ~1%.
  // Dla ODDANIA do sieci blad wynosi DOKLADNIE ZERO: noca nie ma produkcji, wiec
  // licznik oddania stoi i jest obojetne, w ktorej minucie nocy go odczytamy.
  uint32_t lastEpoch = 0;       // epoch ostatniego udanego odczytu; 0 = brak
  float lastImportKwh = -1.f;   // rej. 37121 z tamtej chwili
  float lastExportKwh = -1.f;   // rej. 37119 z tamtej chwili
};

// Prog "baza jest pelna". Nocny pobor domu to ~0,2-0,3 kW (zmierzone: rej. 37113
// pokazywal 184-303 W), wiec 30 minut to najwyzej 0,15 kWh bledu przy dobowym
// poborze rzedu 13 kWh — okolo 1%. W normalnej pracy baza laduje sie duzo
// ciasniej: falownik odpowiada takze noca, a kadencja odpytywania po udanym
// odczycie wynosi 30 s (cfg::PV_REFRESH_MS), wiec `minute` wychodzi 0 albo 1.
// Prog istnieje dla przypadku, w ktorym falownik JEDNAK zamilkl na cala noc —
// wtedy baza wypada rano, jest jawnie niepelna i ekran uczciwie wraca do calki.
// (v169) PROG ZOSTAJE PRZY 30 MINUTACH i to jest decyzja, nie przeoczenie.
// Kusilo, zeby go poluzowac (baza o 02:23 zmiescilaby sie przy progu 150 min), ale
// to leczyloby OBJAW: przy nocnym poborze 0,3 kW prog 150 min wpisuje 0,75 kWh
// cudzej doby w "dzis", czyli ~6% dobowego poboru, i robi to po cichu. Naprawiony
// zostal MECHANIZM (patrz `lastEpoch` w PvMeterBase): baza siega teraz po odczyt
// sprzed polnocy, wiec przy falowniku odpowiadajacym noca odleglosc od polnocy to
// najwyzej jedna kadencja — 30 s w dzien, 5 min noca. Prog 30 min jest wiec z
// ogromnym zapasem, a gdy falownik NAPRAWDE przespi cala noc, ekran ma uczciwie
// zejsc do calki zamiast pokazac liczbe z 6-procentowym, niewidocznym bledem.
constexpr int PV_BASE_FULL_MIN = 30;

// Aktualizuje baze i wpisuje do `s` wartosci "dzis". Czysta logika: zadnego NVS,
// zadnego logowania, zadnej sieci — zapis i log robi wolajacy (netTask), bo tylko
// on wie, czy wolno mu teraz dotknac flash. Dzieki temu funkcja jest w naglowku
// i da sie ja przeczytac w calosci obok struktury, ktorej dotyczy.
inline PvBaseEvent pvMeterUpdate(PvSnapshot& s, PvMeterBase& b, time_t now) {
  s.meterTodayOk = false;
  s.meterTodayExportKwh = 0.f;
  s.meterTodayImportKwh = 0.f;

  // Nigdy nie dostalismy licznikow (wartownik -1) — nie ma z czego liczyc i nie
  // ma czym przestawiac bazy. Baza zostaje nietknieta, bo jest lepsza niz nic.
  if (s.meterExportKwh < 0.f || s.meterImportKwh < 0.f) {
    return PvBaseEvent::NONE;
  }
  // BEZ CZASU Z NTP NIE WOLNO RUSZAC BAZY. Po zimnym starcie time(nullptr) daje
  // czas z 1970 roku; zapisana wtedy baza mialaby yday z 1 stycznia 1970 i przy
  // pierwszym prawdziwym odczycie zegara wygladalaby jak "inna doba", wiec
  // przestawilaby sie po raz drugi — a w miedzyczasie roznica liczylaby sie od
  // zlego punktu. Ten sam prog 1700000000, co w reszcie projektu.
  if (now < 1700000000) {
    return PvBaseEvent::NONE;
  }

  struct tm tmv{};
  localtime_r(&now, &tmv);
  const int32_t yr = tmv.tm_year + 1900;
  const int32_t yd = tmv.tm_yday;
  const int16_t mi = static_cast<int16_t>(tmv.tm_hour * 60 + tmv.tm_min);
  // Polnoc OTWIERAJACA biezaca dobe lokalna, w epochu. Liczona przez odjecie
  // sekund od poczatku doby, a NIE przez mktime() z wyzerowana godzina: mktime()
  // na przelomie czasu letniego/zimowego potrafi oddac godzine obok, a tutaj
  // wynik jest odejmowany od stempli i blad o godzine przesunalby CALA dobe.
  const uint32_t midnight = static_cast<uint32_t>(now) -
                            static_cast<uint32_t>(tmv.tm_hour * 3600 + tmv.tm_min * 60 +
                                                  tmv.tm_sec);

  // (v169) Kandydat SPRZED polnocy: ostatni udany odczyt z poprzedniej doby.
  // `prevOff` jest ujemne, gdy tamten odczyt padl przed ta polnoca.
  const bool havePrev = b.lastEpoch >= 1700000000UL && b.lastImportKwh >= 0.f &&
                        b.lastExportKwh >= 0.f;
  const int32_t prevOff =
      havePrev ? static_cast<int32_t>((static_cast<int64_t>(b.lastEpoch) -
                                       static_cast<int64_t>(midnight)) / 60)
               : 0;

  PvBaseEvent ev = PvBaseEvent::NONE;
  if (!b.valid) {
    ev = PvBaseEvent::SET_FIRST;
  } else if (b.year != yr || b.yday != yd) {
    ev = PvBaseEvent::ROLLED;
  } else if (s.meterImportKwh < b.importKwh || s.meterExportKwh < b.exportKwh) {
    // Licznik zmalal w OBREBIE tej samej doby. Roznica wyszlaby ujemna, a ujemnych
    // kWh nie pokazujemy — przestawiamy baze na biezaca wartosc i na ten dzien
    // wracamy do calki. Wymiana miernika trafia zwykle w galaz ROLLED (inna doba)
    // i tam tez konczy sie przestawieniem bazy, wiec obie drogi sa bezpieczne.
    ev = PvBaseEvent::WENT_BACK;
  }

  if (ev != PvBaseEvent::NONE) {
    // (v169) WYBOR ODCZYTU NA BAZE: ten z dwoch, ktory jest BLIZEJ polnocy.
    // Domyslnie biezacy (offset = minuty PO polnocy, zawsze >= 0). Odczyt sprzed
    // polnocy wygrywa tylko przy ROLLED (przelom doby) i tylko wtedy, gdy naprawde
    // lezy blizej — porownanie jest na wartosciach bezwzglednych, wiec odczyt sprzed
    // dwoch dob (urzadzenie stalo) przegrywa sam z siebie i nie trzeba go osobno
    // odsiewac data.
    //
    // SET_FIRST i WENT_BACK celowo NIE siegaja po odczyt sprzed polnocy: przy
    // SET_FIRST nie wiemy, czy tamten odczyt w ogole dotyczy tej samej instalacji
    // (pierwszy start po aktualizacji), a przy WENT_BACK licznik wlasnie sie cofnal,
    // wiec starsza wartosc jest TYM WIEKSZYM klamstwem.
    int32_t off = mi;
    float impKwhBase = s.meterImportKwh;
    float expKwhBase = s.meterExportKwh;
    int16_t at = mi;
    if (ev == PvBaseEvent::ROLLED && havePrev && prevOff < 0 && -prevOff < off) {
      off = prevOff;
      impKwhBase = b.lastImportKwh;
      expKwhBase = b.lastExportKwh;
      at = static_cast<int16_t>(1440 + prevOff);   // prevOff w (-1440, 0) — patrz warunek
    }
    b.year = yr;
    b.yday = yd;
    b.minute = at;
    b.offsetMin = static_cast<int16_t>(off < -1440 ? -1440 : (off > 1439 ? 1439 : off));
    b.importKwh = impKwhBase;
    b.exportKwh = expKwhBase;
    b.valid = true;
    b.event = static_cast<uint8_t>(ev);
    // PELNA jest WYLACZNIE baza przestawiona na przelomie doby i dosc blisko
    // polnocy. SET_FIRST (pierwszy start po aktualizacji) pelna nie jest NIGDY,
    // nawet gdy wypadnie o 00:05: nie znamy wtedy stanu licznika z tej polnocy
    // — znamy stan sprzed chwili. Ta doba dojezdza na calce z v164, a miernik
    // przejmuje ekran od nastepnej polnocy. WENT_BACK tez nie jest pelna.
    // (v169) Wartosc BEZWZGLEDNA offsetu: baza sprzed polnocy jest tak samo dobra
    // jak baza po polnocy, liczy sie odleglosc, nie strona.
    const int32_t dist = off < 0 ? -off : off;
    b.full = (ev == PvBaseEvent::ROLLED) && (dist <= PV_BASE_FULL_MIN);
  }

  // (v169) Zapamietanie BIEZACEGO odczytu jako "ostatniego" — PO uzyciu poprzedniego
  // do wyboru bazy, inaczej kandydat sprzed polnocy zostalby nadpisany, zanim ktos
  // po niego siegnie. Leci przy KAZDYM udanym odczycie, takze gdy zdarzenia nie bylo:
  // to jest wlasnie ten odczyt, ktory za kilkanascie godzin bedzie "ostatnim przed
  // polnoca".
  b.lastEpoch = static_cast<uint32_t>(now);
  b.lastImportKwh = s.meterImportKwh;
  b.lastExportKwh = s.meterExportKwh;

  if (!b.full) {
    return ev;
  }
  // Nazwy `impKwh`/`expKwh`, a NIE `imp`/`exp`: `exp` to funkcja z <math.h>,
  // a ten naglowek jest wciagany do kilku jednostek kompilacji. Przeslanianie
  // nazwy z biblioteki standardowej w pliku naglowkowym nie daje dzis ostrzezenia
  // (-Wshadow jest wylaczone), ale kosztuje przy pierwszym czytaniu.
  const float impKwh = s.meterImportKwh - b.importKwh;
  const float expKwh = s.meterExportKwh - b.exportKwh;
  // Pas bezpieczenstwa: gdyby mimo wszystko wyszlo ujemnie (baza pelna, a licznik
  // nizej niz o polnocy), nie pokazujemy ujemnych kWh — ekran schodzi do calki.
  // Przestawienie bazy zalatwia galaz WENT_BACK w NASTEPNYM cyklu; tutaj tylko
  // nie klamiemy.
  if (impKwh < 0.f || expKwh < 0.f) {
    return ev;
  }
  s.meterTodayImportKwh = impKwh;
  s.meterTodayExportKwh = expKwh;
  s.meterTodayOk = true;
  return ev;
}

// Profil dnia bieżącego: 1 próbka co 10 minut (144 sloty).
// Dwie serie, żeby wykres pokazywał nie tylko ile wyprodukowaliśmy, ale też ile
// z tego zużyliśmy na miejscu i kiedy musieliśmy dobrać z sieci.
//
// TRWALOSC: ta struktura jest utrwalana w NVS - patrz pvHistoryLoad/Save
// w Settings.cpp. NIE jest zapisywana bajt w bajt: idzie przez wlasna strukture
// PvProfileBlob pod kluczem "prof2", z polem wersji. Kazda zmiana ukladu ALBO
// znaczenia `watts`/`load` (np. przejscie z watow na dziesiatki watow - rozmiar
// zostaje ten sam!) MUSI podbic PV_PROF_VER albo klucz, inaczej stary profil
// wczyta sie jako nowy i wykres pokaze nieprawde bez zadnego ostrzezenia.
// (v169) Do NVS probka idzie w JEDNYM BAJCIE, po skali nieliniowej — w RAM
// zostaje pelne uint16 [W]. Kwantyzacja dotyczy wylacznie tego, co przezylo
// restart; szczegoly i blad w pikselach przy pvWattValue() w Settings.cpp.
struct PvHistory {
  static constexpr int SLOTS = 144;
  uint16_t watts[SLOTS] = {};  // produkcja PV [W]
  uint16_t load[SLOTS] = {};   // pobór domu [W]
  bool filled[SLOTS] = {};
  int day = -1;  // tm_yday, do resetu o północy

  void reset(int yday) {
    for (int i = 0; i < SLOTS; ++i) {
      watts[i] = 0;
      load[i] = 0;
      filled[i] = false;
    }
    day = yday;
  }

  static uint16_t clampW(int32_t w) {
    if (w < 0) return 0;
    return static_cast<uint16_t>(w > 65535 ? 65535 : w);
  }

  void push(int yday, int hour, int minute, int32_t w, int32_t loadW) {
    if (yday != day) {
      reset(yday);
    }
    const int slot = (hour * 60 + minute) / 10;
    if (slot < 0 || slot >= SLOTS) {
      return;
    }
    watts[slot] = clampW(w);
    load[slot] = clampW(loadW);
    filled[slot] = true;
  }

  // Szczyt z OBU serii — pobór potrafi przewyższyć produkcję (dobieramy z sieci),
  // a wtedy słupek poboru musi się zmieścić w tej samej skali co słupek produkcji.
  // (v167) Bez nazw kolorów: te same dwie serie są od v167 rysowane jako TRZY
  // rozłączne pasma (autokonsumpcja / oddane / z sieci) i przypisanie „seria =
  // kolor” przestało tu obowiązywać — patrz kontrakt kolorów w ThemeV3.h.
  uint16_t peak() const {
    uint16_t m = 0;
    for (int i = 0; i < SLOTS; ++i) {
      if (!filled[i]) continue;
      if (watts[i] > m) m = watts[i];
      if (load[i] > m) m = load[i];
    }
    return m;
  }
};

// Kody stanu falownika Huawei (rej. 32089) -> etykieta PL.
inline const char* pvStatusLabel(uint16_t code) {
  switch (code) {
    case 0x0000:
    case 0x0001:
    case 0x0002:
    case 0x0003:
      return "Czuwanie";
    case 0x0100:
      return "Start";
    case 0x0200:
      return "Praca";
    case 0x0201:
      return "Praca (limit)";
    case 0x0202:
      return "Praca (derating)";
    case 0x0300:
      return "AWARIA";
    case 0x0301:
    case 0x0302:
    case 0x0303:
    case 0x0304:
    case 0x0305:
    case 0x0306:
    case 0x0307:
    case 0x0308:
      return "Wyłączony";
    case 0x0500:
    case 0x0600:
    case 0x0700:
    case 0x0800:
    case 0x0900:
      return "Test";
    case 0xA000:
      return "Brak słońca";
    default:
      // NIE "Praca". Zaszyta odpowiedz na "nie wiem" zamieniala KAZDY nieznany kod
      // w uspokajajacy komunikat: falownik mogl zglaszac stan, ktorego nie znamy,
      // a ekran twierdzil, ze wszystko gra. "Stan nieznany" mozna wygooglowac
      // (numer rejestru jest w /api/state), "Praca" nie da sie podwazyc.
      //
      // Zwracamy literal, NIE bufor statyczny. Kusi, zeby wypisac tu kod szesnastkowo
      // przez `static char[]` — to bylby wyscig miedzyrdzeniowy: ta funkcja jest
      // wolana z MqttClient.cpp:553 (netTask, rdzen 0) i z WeatherUi.cpp:990
      // (renderowanie, rdzen 1). Dwa watki, jeden bufor, rwany napis.
      return "Stan nieznany";
  }
}

inline bool pvStatusIsFault(uint16_t code) {
  return code == 0x0300;
}

inline bool pvStatusIsRunning(uint16_t code) {
  return code >= 0x0200 && code <= 0x0202;
}
