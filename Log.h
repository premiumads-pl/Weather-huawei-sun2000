#pragma once

#include <Arduino.h>

// Bufor kołowy logów w RAM. Urządzenie wisi na ścianie bez USB, więc Serial
// jest ślepy — logi czytamy przez GET /api/log.
void logPrintf(const char* fmt, ...);
String logDump();

#define LOG(...) logPrintf(__VA_ARGS__)

// --- migawka stanu dla GET /api/diag ---
struct Diag {
  uint32_t weatherOkAt = 0;
  uint32_t pvOkAt = 0;
  uint32_t radarOkAt = 0;
  uint32_t flightOkAt = 0;
  uint32_t mqttOkAt = 0;   // ostatnia udana publikacja

  char weatherErr[48] = {};
  char pvErr[48] = {};
  char radarErr[48] = {};
  char flightErr[48] = {};
  char otaMsg[48] = {};
  char mqttErr[48] = {};

  uint32_t mqttConnects = 0;
  uint32_t mqttPublished = 0;

  uint8_t radarLevel = 0;
  uint32_t radarAgeSec = 0;
  int flightsTotal = 0;
  int otaRemote = 0;
  uint32_t otaOkAt = 0;
  uint32_t wifiConnects = 0;
  uint32_t minHeap = 0xFFFFFFFF;
  uint32_t stackNet = 0;   // zapas stosu netTask (B)
  uint32_t stackWeb = 0;   // zapas stosu webTask (B)
  uint32_t wifiRoams = 0;  // ile razy przenieslismy sie na mocniejszy punkt
  uint32_t frameDrawUs = 0;   // rysowanie klatki do bufora (PSRAM)
  uint32_t framePushUs = 0;   // wypchniecie bufora na SPI
  uint32_t framePeriodUs = 0; // RZECZYWISTY okres klatki (z pauzami)
  uint32_t viOkAt = 0;        // piec: ostatni udany odczyt
  char viErr[56] = {};
  float viDhwC = 0.f;
  float viSupplyC = 0.f;

  // --- piec: SUROWE liczniki, tak jak przyszly z API ---
  // Po co: nie wiemy, jaka rozdzielczosc ma naprawde `hours` i `starts` u TEGO pieca,
  // a to rozstrzyga, ktory ksztalt wykresu palnika w ogole moze zadzialac. Jesli
  // hours rusza sie o 0,01 — mozna liczyc czas palenia miedzy odpytami. Jesli o 0,1 —
  // rozdzielczosc to 6 minut i wykres 10-minutowy jest na granicy. Jesli nie rusza
  // sie wcale, cala droga przez liczniki jest slepa.
  // Tego NIE DA sie wyczytac z /api/log: bufor kolowy 3072 B miesci okolo szesciu
  // minut, a pomiar wymaga doby. Te pola zyja tyle, co uptime.
  // Bez sekretow: same liczby z pieca, zaden token ani clientId.
  float viBurnerHours = 0.f;      // Z ULAMKIEM — bez tego caly pomiar jest bezcelowy
  uint32_t viBurnerStarts = 0;
  int viModulation = 0;           // ostatnia ZLAPANA modulacja (tylko gdy przyszla)
  bool viBurnerActive = false;
  float viGasDayM3 = 0.f;         // summary.dhw + summary.heating, currentDay
  // "Zero" kontra "nie przyszlo" — ta sama lekcja co vi::Model::has*. Bez tych flag
  // nieruchome hours=0,0 znaczy naraz "licznik stoi" i "cechy nie ma w odpowiedzi",
  // czyli dokladnie te dwie rzeczy, ktore ten pomiar ma rozroznic.
  // Osobno dla hours i starts: przy jednej fladze na OR polowiczna odpowiedz
  // (samo `starts`) meldowalaby "statystyki sa" przy burner_hours = 0,0.
  bool viHasBurnerHours = false;
  bool viHasBurnerStarts = false;
  bool viHasGas = false;
  int radarFrame = -1;        // ktora klatka animacji radaru jest rysowana
  int radarFrameMin = 0;      // jej przesuniecie czasowe (min)
  uint32_t radarSkips = 0;   // ile razy radar odpuścił z braku pamięci

  // --- OTA: okres próbny i rollback (patrz OtaGuard.h) ---
  uint8_t otaTrial = 0;      // 0 = stabilna, 1 = próbna, 2 = potwierdzona
  uint32_t otaConfirmAt = 0; // millis() potwierdzenia wersji próbnej
  int otaBadVersion = 0;     // wersja odrzucona po rollbacku (0 = brak)

  // --- restarty: dziś nie wiemy, czy urządzenie się wywala ---
  uint8_t resetReason = 0;      // esp_reset_reason() z bieżącego startu
  uint8_t prevResetReason = 0;  // to samo z poprzedniego startu (z NVS)
  uint16_t panicCount = 0;      // ile razy panic/watchdog/brownout — od zawsze

  // --- falownik ---
  bool pvAsleep = false;     // noc: Huawei wyłącza Modbus TCP, to nie awaria

  // Kumulacyjny licznik odczytów Modbusa — odpowiedź na pytanie "jak często pada
  // dany rejestr". Nie da się jej wyczytać z /api/log: to bufor KOŁOWY 3072 B
  // (Log.cpp:7), czyli ~47 linii, a przy zdrowej pracy (PV co 30 s, loty co 15 s)
  // okno rzędu SZEŚCIU MINUT. `grep -c` po /api/log mierzy ostatnie kilka minut,
  // nawet gdy urządzenie chodzi od tygodnia. Te pola żyją tyle, co uptime, więc
  // po dobie mówią prawdę o dobie.
  // Bez sekretów: same liczby, żadnego IP falownika — /api/diag bywa wklejane
  // do zgłoszeń błędów.
  uint32_t pvCycles = 0;      // ile razy fetch() doszedł do czytania rejestrów
  uint32_t pvFails = 0;       // suma porażek w piątce "starej" (32064/32080/37113/32106/32114)
  uint32_t pvExtraFails = 0;  // suma porażek w piątce "nowej" (32016/32087/32086/32089/37100)

  // Rozkład porażek NA CYKL; indeks = ile padło w jednym cyklu (0..5).
  // Suma sama w sobie nie wystarcza, bo próg jest decyzją PER CYKL: 1000 porażek
  // rozłożone po jednej na 1000 cykli nie przekroczyłoby progu ANI RAZU, a te same
  // 1000 porażek jako 200 cykli po pięć przekraczałoby go w każdym z nich. Histogram
  // te przypadki rozróżnia i pozwala policzyć offline dowolny kandydat na próg.
  uint32_t pvFailHist[6] = {};
  uint32_t pvExtraHist[6] = {};

  // --- czujniki: LDR (GPIO1, ADC1) + PIR AM312 (GPIO13) ---
  // Bez sekretow: LDR to jasnosc otoczenia, PIR to obecnosc — nic prywatnego.
  // Od v103 ldrMv NIE jest juz samym podgladem: steruje podswietleniem (progi w Config.h).
  // Oba pola sa usredniane z 8 odczytow w tej samej petli — do v102 usredniany byl TYLKO
  // ldrRaw, a ldrMv bral pojedyncza probke, wiec dwa pola opisujace ten sam pomiar
  // pochodzily z roznych chwil.
  uint16_t ldrRaw = 0;       // surowy ADC1 z GPIO1 (0-4095), usredniony z 8
  uint16_t ldrMv = 0;        // to samo w mV (kalibracja eFuse), usredniony z 8 — jasno = wyzej
  bool pirState = false;     // biezacy stan AM312 na GPIO13 (HIGH = ruch); odswieza loop()
  // volatile, bo od v101 pisze to ISR, a nie loop() — czytelnik jest w innym watku.
  volatile uint32_t pirLastAt = 0;  // millis() ostatniego zbocza W GORE (0 = od startu nic)

  // --- PIR: pomiar zachowania AM312 w lazience (liczniki z pirIsr()) ---
  // PO CO: nie wiemy, jak ten czujnik realnie zachowuje sie w TYM pomieszczeniu, a od
  // tego zalezy DRUGA polowa logiki podswietlenia. Pierwsza polowa jest juz zrobiona:
  // od v103 poziom jasnosci wybiera LDR (zegar NIGHT_FROM_H/TO_H zostal USUNIETY,
  // patrz Config.h). PIR ma dolozyc to, czego swiatlo nie powie: gaszenie ekranu do
  // ZERA, gdy nikogo nie ma, i budzenie, gdy ktos wejdzie po ciemku.
  // Dwa pytania do rozstrzygniecia: (1) ile wynosi timeout "nikogo nie ma" — pierwszy
  // pomiar (33 min) dal DZIEWIEC przerw w pasmie 60-300 s, wiec 60 s byloby bledem,
  // (2) czy para z prysznica go wyzwala.
  // Tego NIE DA sie zmierzyc przez /api/log: to bufor KOLOWY 3072 B (Log.cpp:7), czyli
  // ~47 linii — okno rzedu SZESCIU MINUT. Czujnik wyzwalany co chwile zalalby log i
  // zniszczyl pomiar, dla ktorego go wlaczamy (a przy okazji wymiotl wszystko inne).
  // Te pola zyja tyle, co uptime, wiec po dobie mowia prawde o dobie.
  //
  // WSPOLBIEZNOSC: pisze WYLACZNIE ISR, czyta wylacznie loop()/Portal. Zadnych blokad —
  // mutex w ISR jest zabroniony, a licznik rozjechany o jeden impuls nikogo nie boli.
  // Bez sekretow: same liczby o ruchu, bez znacznikow czasu — "kto i kiedy byl w
  // lazience" z tego nie wychodzi. /api/diag bywa wklejane do zgloszen bledow.
  volatile uint32_t pirPulses = 0;   // pelne impulsy (zbocze w gore -> zbocze w dol)
  volatile uint32_t pirEdges = 0;    // WSZYSTKIE zbocza. Kontrola czystosci sygnalu:
                                     // czysto => edges == 2*pulses (+1 gdy impuls trwa).
                                     // Nadmiar = drgania/szum i wtedy reszta liczb klamie.
  volatile uint32_t pirLastMs = 0;   // szerokosc OSTATNIEGO impulsu [ms] — do machania reka
  volatile uint32_t pirMinMs = 0xFFFFFFFF;  // najkrotszy impuls (0xFFFFFFFF = nie bylo zadnego)
  volatile uint32_t pirMaxMs = 0;    // najdluzszy impuls
  volatile uint32_t pirTotalMs = 0;  // suma czasu HIGH — jaki UDZIAL doby "cos sie rusza".
                                     // uint32 przepelnia sie po 49 dniach SAMEGO HIGH,
                                     // czyli nigdy: to wiecej, niz millis() w ogole umie.

  // Histogramy, nie same min/max — dokladnie ta sama lekcja co pvFailHist wyzej.
  // min/max psuje JEDEN wyskok: pojedynczy 3-milisekundowy szum przykleja min do 3 i
  // czyta sie to jak "drgania styków", a jeden dlugi epizod przykleja max do minut.
  // Decyzja jest o ROZKLADZIE, nie o skrajnosciach, a histogram pozwala policzyc
  // OFFLINE dowolnego kandydata na prog — bez wgrywania nowej wersji na kazdy pomysl.
  //
  // Szerokosci impulsow, kosze [ms]: <100 | 100-1k | 1k-3k | 3k-10k | 10k-60k | >=60k.
  // To jest empiryczna odpowiedz na "jaki to naprawde modul": AM312 to jednorazowka
  // ~2 s (wszystko powinno siedziec w koszu 1k-3k), SR505 ~8 s (kosz 3k-10k), a modul
  // retrigerujacy rozmaze sie po koszach wyzszych. Kosz <100 ms to nie PIR, tylko szum.
  volatile uint32_t pirWidthHist[6] = {};

  // Przerwy MIEDZY impulsami (od opadniecia do nastepnego narastania), kosze [ms]:
  // <2k | 2k-5k | 5k-15k | 15k-60k | 60k-300k | 300k-1800k | >=1800k.
  // TO JEST POLE, KTORE WYBIERZE TIMEOUT PODSWIETLENIA. Timeout to prog NA PRZERWIE:
  // ma byc dluzszy niz typowa przerwa "ktos tu wciaz jest" i krotszy niz "wyszedl".
  // Ani pulses, ani total_s, ani min/max tego nie daja — potrzebny jest rozklad przerw.
  // Pierwszy kosz (<2 s) to w duzej mierze okno martwe AM312, nie zachowanie czlowieka.
  volatile uint32_t pirGapHist[7] = {};
};

Diag& diag();
