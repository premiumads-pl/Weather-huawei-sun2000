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

  // --- czujniki v100 (surowy odczyt do testu; jeszcze bez logiki) ---
  // Bez sekretow: LDR to jasnosc otoczenia, PIR to obecnosc — nic prywatnego.
  uint16_t ldrRaw = 0;       // surowy ADC1 z GPIO1 (0-4095), usredniony
  uint16_t ldrMv = 0;        // to samo w mV (kalibracja eFuse) — jasno = wyzej
  bool pirState = false;     // biezacy stan SR505 na GPIO13 (HIGH = ruch)
  uint32_t pirLastAt = 0;    // millis() ostatniego HIGH (0 = od startu nic)
};

Diag& diag();
