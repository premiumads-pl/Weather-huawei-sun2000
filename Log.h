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
  // Bez sekretow: LDR to jasnosc otoczenia, PIR to obecnosc — zadnych tokenow ani IP.
  // (Uwaga: to zdanie mowilo kiedys "nic prywatnego" i po dolozeniu PirRtc::byHour
  //  przestalo byc prawda — patrz nota o prywatnosci przy PirRtc nizej.)
  // Od v103 ldrMv NIE jest juz samym podgladem: steruje podswietleniem (progi w Config.h).
  // Oba pola sa usredniane z 8 odczytow w tej samej petli — do v102 usredniany byl TYLKO
  // ldrRaw, a ldrMv bral pojedyncza probke, wiec dwa pola opisujace ten sam pomiar
  // pochodzily z roznych chwil.
  uint16_t ldrRaw = 0;       // surowy ADC1 z GPIO1 (0-4095), usredniony z 8
  uint16_t ldrMv = 0;        // to samo w mV (kalibracja eFuse), usredniony z 8 — jasno = wyzej
  bool pirState = false;     // biezacy stan AM312 na GPIO13 (HIGH = ruch); odswieza loop()
  // volatile, bo od v101 pisze to ISR, a nie loop() — czytelnik jest w innym watku.
  //
  // ZOSTAJE W DRAM, a nie w RTC razem z reszta pomiaru PIR (patrz PirRtc nizej) — i to
  // nie przeoczenie. To ZNACZNIK CZASU w millis(), a millis() zeruje sie przy kazdym
  // restarcie. Przeniesiony do RTC przezylby OTA jako liczba bez znaczenia: po restarcie
  // "ostatni ruch" wypadlby w przyszlosci wzgledem nowego millis() i pir_last_s
  // pokazywaloby ujemne albo 49-dniowe bzdury. Do RTC ida tylko RÓŻNICE czasu
  // (szerokosci, przerwy, sumy), ktore restart znosi bez szkody.
  volatile uint32_t pirLastAt = 0;  // millis() ostatniego zbocza W GORE (0 = od startu nic)
};

Diag& diag();

// --- PIR: pomiar dlugoterminowy, PRZEZYWA OTA (pamiec RTC) ---
// PO CO W OGOLE: nie wiemy, jak ten czujnik realnie zachowuje sie w TEJ lazience —
// kto i kiedy z niej korzysta i czy AM312 to wiernie oddaje. PIR NIE steruje niczym
// i sterowac nie bedzie: podswietleniem rzadzi sam LDR (v103+, zegar NIGHT_FROM_H
// USUNIETY — patrz Config.h) i to zostaje. Rozwazany "PIR gasi ekran w pustej
// lazience" zostal ODRZUCONY, bo nie zmienilby ANI JEDNEJ wartosci jasnosci: pusta
// ciemna lazienka ma miec 45 i obecny czlowiek w ciemnej lazience tez ma miec 45.
// To jest wiec pomiar i tylko pomiar.
//
// DLACZEGO RTC, A NIE ZWYKLY RAM: liczniki zyly tyle, co uptime, a KAZDE wydanie OTA
// je kasowalo — przez ostatnia dobe wyszlo piec wersji. Docelowy horyzont to TYDZIEN,
// wiec tydzien bez wydania jest nierealny i pomiar nigdy by sie nie domknal.
// RTC_NOINIT_ATTR przezywa restart programowy (OTA), panic i watchdog; ginie dopiero
// przy odlaczeniu zasilania. Tego NIE DA sie zalatwic przez /api/log: to bufor KOLOWY
// 3072 B (Log.cpp:7), czyli ~47 linii — okno rzedu SZESCIU MINUT.
//
// GDZIE TO NAPRAWDE LEZY — sprawdzone w linkerze i w ELF-ie, nie "z poradnika":
// .rtc_noinit idzie do regionu `rtc_data_location` (sections.ld), a ten jest aliasem na
// `rtc_slow_seg` (memory.ld:107), bo w sdkconfig stoi
// "# CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM is not set". Czyli RTC **SLOW** @ 0x50000000+512,
// dlugosc 0x2000-512 = 7680 B — a NIE rtc_iram_seg (RTC FAST @ 0x600fe000), jak sie
// powszechnie powtarza i jak zakladalismy, zabierajac sie za te zmiane. Potwierdza to
// gotowy obraz: `nm` daje "50000200 000000c0 B gPir", a sekcje .rtc.text i
// .rtc.force_fast pod 0x600fe000 maja ROZMIAR ZERO. Ta struktura ma 192 B, czyli 2,5%
// regionu — miejsca jest az nadto, wiec praktycznie nic sie przez to nie zmienia.
// Region jest ODDZIELNY od .dram0, wiec przeniesienie tych pol z Diag ZWALNIA RAM
// (potwierdzone kompilacja: 73112 -> 73040 B, zapas do progu 76000 rosnie do 2960 B).
//
// CO DOKLADNIE TO PRZEZYWA — i dlaczego akurat .rtc_noinit, a nie RTC_DATA_ATTR:
// .rtc_noinit jest sekcja NOLOAD, wiec nie ma jej w obrazie i NIKT jej nie laduje ani
// nie zeruje. Startowy memset dotyczy zakresu _rtc_bss_start.._rtc_bss_end, a ten jest
// w tym obrazie PUSTY (oba symbole = 0x50000200), wiec obejmuje zero bajtow — gPir lezy
// tuz za nim i nie da sie go tym trafic. RTC_DATA_ATTR (.rtc.data) by NIE zadzialalo:
// tamto jest w obrazie i bootloader odtwarza je z flasha przy zwyklym resecie, czyli
// dokladnie przy OTA — liczniki wracalyby do zera i caly ten zabieg bylby na nic.
// Przezywa wiec: OTA, ESP.restart(), panic, watchdog, brownout.
// NIE przezywa: odlaczenia zasilania (i wtedy magic to wylapie, patrz nizej).
//
// CZY ISR MOZE TU PISAC — sprawdzone DEASEMBLACJA, nie zalozone:
//  * BEZPIECZENSTWO. RTC SLOW to zwykla pamiec pod stalym adresem, NIE okno cache'a
//    flasha. objdump pirIsr() pokazuje jeden `l32r a7, (50000200 <gPir>)` (baza raz do
//    rejestru), a potem same `l32i.n/s32i.n a8, a7, offset` — DOKLADNIE te same formy
//    instrukcji, ktorymi ten sam ISR adresuje globale w DRAM (gPirFallAt @ 3fca5880).
//    Zadnego wywolania, zadnej funkcji pomocniczej, zadnej sciezki przez sterownik.
//    Nie ma tu wiec ryzyka "skoku we flash przy zimnym cache", ktore rzadzi CALA reszta
//    bezpieczenstwa tego ISR-a (patrz dlugi komentarz przy pirIsr()). `memw` wokol
//    dostepow bierze sie z `volatile`, a nie z RTC — stoi tak samo przy globalach DRAM.
//  * KOSZT. RTC SLOW faktycznie siedzi na wolniejszej szynie niz DRAM i tej latencji
//    NIE zmierzylismy (urzadzenie wisi w lazience, tylko OTA). Nie trzeba: liczbe
//    dostepow ogranicza kod (z deasemblacji: <=13 slow na zbocze opadajace), a czestosc
//    zbocz ogranicza FIZYKA czujnika — AM312 to impuls ~2 s plus okno martwe ~2 s, wiec
//    nawet przy CIAGLYM ruchu to najwyzej ~0,5 zbocza na sekunde. Przy absurdalnie
//    pesymistycznym 1 us na dostep (240 taktow!) daje to 13 us * 0,5/s = ~7 us na
//    sekunde pracy rdzenia, czyli ~0,0007%. Petla renderu ma 33 ms budzetu na klatke.
//    Niepewnosc latencji RTC po prostu nie ma jak urosnac do czegokolwiek istotnego.
//  * WYROWNANIE. Wszystkie pola to uint32 pod adresem podzielnym przez 4 (struktura
//    startuje na 0x50000200), wiec zadnego dostepu bajtowego ani niewyrownanego.
//
// WSPOLBIEZNOSC: liczniki pisze WYLACZNIE ISR, `byHour` i pola ciaglosci WYLACZNIE
// loop(); czyta Portal (webTask). Zadnych blokad — mutex w ISR jest zabroniony, a
// wszystko to wyrownane uint32, wiec pojedynczy odczyt jest atomowy. Licznik
// rozjechany o jeden impuls nikogo nie boli.
//
// PRYWATNOSC — to sie ZMIENILO wraz z byHour i nie wolno tego przemilczec. Do tej wersji
// przy tych polach stalo "same liczby o ruchu, bez znacznikow czasu — 'kto i kiedy byl w
// lazience' z tego nie wychodzi". Po dolozeniu byHour to juz NIEPRAWDA: rozklad godzinowy
// z lazienki to wprost rytm dnia domownikow — kiedy wstaja, kiedy wracaja, kiedy ich nie
// ma. Ocena: w domowej sieci to nie jest sekret i pomiar jest tego wart. ALE /api/diag
// BYWA WKLEJANE DO ZGLOSZEN BLEDOW, a tam trafia do publicznego repo — i to jest jedyne
// realne ryzyko, bo pusty pas godzin 8-16 mowi obcemu, kiedy w domu nikogo nie ma.
// Dlatego swiadomie: zadnych dat, zadnych znacznikow pojedynczych zdarzen, tylko 24 sumy
// bez dnia — z tego nie da sie odtworzyc konkretnej wizyty ani konkretnej doby, a do
// pytania "jak to realnie wyglada" w zupelnosci wystarcza. Przed wklejeniem /api/diag
// gdziekolwiek publicznie warto wyciac sensors.pir_by_hour — reszta pol jest niegrozna.
// Znacznik waznosci pamieci RTC. Dolny bajt to WERSJA UKLADU pol PirRtc — PODBIJ GO przy
// KAZDEJ zmianie tej struktury. Inaczej po OTA nowy kod odczytalby stary obraz przesuniety
// o pole i wyszlyby z tego liczby wygladajace sensownie, a bedace smieciem. To jedyny
// przypadek, w ktorym pomiar traci sie swiadomie i slusznie.
inline constexpr uint32_t PIR_RTC_MAGIC = 0x9147A501;

struct PirRtc {
  // Po odlaczeniu pradu RTC zawiera SMIECI i bez tej proby wczytalibysmy je jako dane
  // (np. 4 mld impulsow i przerwy z kosmosu). Rozstrzyga znacznik, a nie "czy liczby
  // wygladaja rozsadnie" — smiec potrafi wygladac rozsadnie.
  uint32_t magic;

  // --- ciaglosc pomiaru: "zbieram od" (NIE mylic z uptime) ---
  // Po OTA uptime leci od zera, a pomiar leci dalej — te trzy pola sa jedynym miejscem,
  // z ktorego widac PRAWDZIWY horyzont zebranych danych.
  uint32_t startedEpoch;  // epoch POCZATKU zbierania (0 = NTP jeszcze nie dal czasu)
  uint32_t collectedS;    // sumaryczne sekundy REALNEGO zbierania, doliczane przyrostami
                          // w loop(). To NIE to samo, co (teraz - startedEpoch): tamto
                          // liczy takze czas, gdy urzadzenie sie restartowalo i NIC nie
                          // mierzylo. Roznica obu = ile pomiaru zjadly restarty, i wlasnie
                          // dlatego trzymamy OBA, a nie jedno.
  uint32_t boots;         // ile razy wystartowalismy na tym komplecie danych (OTA/panic)

  // --- liczniki z pirIsr() ---
  volatile uint32_t pulses;   // pelne impulsy (zbocze w gore -> zbocze w dol)
  volatile uint32_t edges;    // WSZYSTKIE zbocza. Kontrola czystosci sygnalu:
                              // czysto => edges == 2*pulses (+1 gdy impuls trwa).
                              // Nadmiar = drgania/szum i wtedy reszta liczb klamie.
  volatile uint32_t rises;    // same zbocza W GORE = "wyzwolenia". Osobno od edges, bo to
                              // one sa jednostka rytmu doby (byHour) i tylko z nich da sie
                              // policzyc przyrost w loop() bez zgadywania.
  volatile uint32_t lastMs;   // szerokosc OSTATNIEGO impulsu [ms] — do machania reka
  volatile uint32_t minMs;    // najkrotszy impuls (0xFFFFFFFF = nie bylo zadnego)
  volatile uint32_t maxMs;    // najdluzszy impuls
  volatile uint32_t totalMs;  // suma czasu HIGH — jaki UDZIAL doby "cos sie rusza".
                              // uint32 przepelnia sie po 49 dniach SAMEGO HIGH.

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
  volatile uint32_t widthHist[6];

  // Przerwy MIEDZY impulsami (od opadniecia do nastepnego narastania), kosze [ms]:
  // <2k | 2k-5k | 5k-15k | 15k-60k | 60k-300k | 300k-1800k | >=1800k.
  // TO JEST POLE, KTORE WYBIERZE TIMEOUT, gdyby kiedys jakis timeout byl potrzebny.
  // Ani pulses, ani total_s, ani min/max tego nie daja — potrzebny jest rozklad przerw.
  // Pierwszy kosz (<2 s) to w duzej mierze okno martwe AM312, nie zachowanie czlowieka.
  volatile uint32_t gapHist[7];

  // Rytm doby: ile WYZWOLEN (zbocz w gore) w kazdej godzinie czasu LOKALNEGO
  // (TZ=CET/CEST ustawiane w connectWifi()). Sam histogram przerw mowi, JAK dlugo trwaja
  // epizody, ale nie mowi KIEDY — a wlasnie to jest pytanie wlasciciela ("ja chodze,
  // dzieci chodza, czasem ktos nie zamknie drzwi").
  // Bucketowanie robi loop(), NIGDY ISR: localtime_r() bierze locka newlib i siega do
  // danych strefy we flashu — w przerwaniu to zakaz.
  // Liczone tylko, gdy NTP dal czas (> 1700000000). Bez czasu godzina jest nieznana i
  // wyzwolenie przepada — lepiej zgubic kilka z pierwszych sekund po starcie, niz
  // wsypac je wszystkie do godziny 0 i zmyslic pik o polnocy.
  uint32_t byHour[24];
};

// Definicja (z RTC_NOINIT_ATTR) siedzi w pogoda-gdynia.ino — w tej samej jednostce
// kompilacji, co pirIsr(), zeby ISR siegal po adres absolutny, a nie przez skok.
extern PirRtc gPir;

// Sprawdza znacznik waznosci i zeruje wszystko po zimnym starcie. Wolac w setup()
// PRZED attachInterrupt() — inaczej pierwsze zbocze trafiloby w niezainicjowane smieci.
void pirRtcBegin();

// =====================================================================================
// LDR: pomiar dlugoterminowy, PRZEZYWA OTA (pamiec RTC) — v108
// =====================================================================================
//
// PO CO TO ISTNIEJE. W tej lazience sa DWA zrodla swiatla i pomiar na zywo (noc, 8 minut
// stabilnego odczytu) pokazal, ze traktujemy je jak jedno, a nie sa:
//     oba zgaszone, ustalone       4-5 mV     (R ~6,5 MOhm)
//     tylko prysznic, ustalone   603-617 mV   (R ~35 kOhm)
//     oba / sam kontroler       2576-3164 mV
// Prog LDR_DIM_UP_MV = 650, wiec samo swiatlo pod prysznicem NIE PODNOSI ekranu z poziomu
// 0 — brakuje mu ~35 mV. Wlasciciel chce, zeby podnosilo (poziom 1, BL_DIM = 130).
//
// DLACZEGO MIMO TO NIE RUSZAMY TU ZADNEGO PROGU. Bo dokladnie tak powstal blad v103:
// prog wykrywania awarii LDR postawiono na JEDNYM pomiarze zrobionym w locie (251 mV
// wziete za "ciemnosc", a to byl zmierzch — prawdziwa ciemnosc ma 17-26 mV). Skutek:
// dzialajacy czujnik uznawany za zepsuty, ekran swiecil 130 zamiast 45, a wlasciciel
// znalazl to siedzac po ciemku i czekajac, az ekran przygasnie. v104 wywalilo cala
// funkcje. Te 8 minut pomiaru to nadal JEDEN wieczor przy JEDNYM ustawieniu drzwi,
// lustra i firanki — czyli dokladnie ta sama klasa dowodu, co feralne 251 mV.
// TA WERSJA TYLKO OBSERWUJE. Prog postawimy po tygodniu, na rozkladzie.
//
// CO ROZSTRZYGNIE HISTOGRAM — bo to jest jedyny powod, dla ktorego ma 16 koszy zamiast
// szesciu: czy "tylko prysznic" tworzy CZYSTY PIK przy 603 (wtedy prog 450 jest bezpieczny
// i zostaje 150 mV zapasu w kazda strone), czy ROZMAZUJE SIE od 450 do 750 (wtedy zaden
// staly prog nie zadziala, bo zachodzi na zmierzch, i trzeba myslec inaczej). Szerokosc
// piku JEST odpowiedzia. Dlatego kosze sa zageszczone w strefie spornej 256-768 mV —
// tam mieszka i "swiezo ciemno" (251), i prysznic (603), i dzisiejszy prog (650). Poza ta
// strefa rozdzielczosc jest nieciekawa: 2576 kontra 3164 to i tak "swiatlo zapalone".
// min/max by tu NIE wystarczylo — to ta sama lekcja, co przy pvFailHist i widthHist:
// decyzja jest o ROZKLADZIE, a histogram pozwala policzyc OFFLINE dowolnego kandydata
// na prog, bez wgrywania nowej wersji na kazdy pomysl.
//
// GDZIE TO LEZY: RTC SLOW, tym samym mechanizmem co gPir wyzej (.rtc_noinit, NOLOAD,
// przezywa OTA/panic/watchdog, ginie przy zaniku zasilania). Cale uzasadnienie — czemu
// SLOW a nie FAST, czemu .rtc_noinit a nie RTC_DATA_ATTR — stoi przy PirRtc i nie bede
// go tu powtarzal. Dwa powody, dla ktorych to NIE moze byc zwykly DRAM:
//  1. Zapas statycznego RAM-u to 2960 B, a tools/release.sh blokuje wydanie przy 76000 B.
//     RTC SLOW to OSOBNA pamiec (7680 B, gPir zajmuje 192 B) — nie dotyka tej bariery.
//  2. Zbieramy TYDZIEN, a w tym czasie wyjdzie v109. Bez RTC pierwsze OTA kasuje caly
//     zbior i pomiar nigdy sie nie domknie. gPir juz to udowodnil w praktyce:
//     "boots: 3, gap_s: 5" — trzy restarty przezyte, stracone 5 sekund.
//
// PRYWATNOSC: /api/diag bywa wklejane do zgloszen bledow, a repo jest publiczne. Histogram
// i sekundy na poziomach to sumy bez dnia i bez godziny — nie da sie z nich odtworzyc
// zadnej konkretnej doby. ldr_events[] jest INNE: ma start_epoch, czyli WPROST date i
// godzine. To swiadomy koszt (bez znacznika czasu zdarzenie jest bezuzyteczne — nie da
// sie go zestawic z niczym), ale przed publicznym wklejeniem /api/diag wytnij
// sensors.ldr_events tak samo, jak sensors.pir_by_hour.

// Znacznik waznosci pamieci RTC dla LdrRtc. OSOBNY od PIR_RTC_MAGIC — i to jest cala
// pointa: gdy podbije sie uklad pol LDR-a, pomiar PIR-a ma PRZEZYC, i odwrotnie. Jeden
// wspolny magic kasowalby oba pomiary przy kazdej zmianie ktoregokolwiek.
// Dolny bajt to WERSJA UKLADU pol LdrRtc — PODBIJ GO przy KAZDEJ zmianie tej struktury.
// Inaczej po OTA nowy kod odczyta stary obraz przesuniety o pole i wyjda z tego liczby
// wygladajace sensownie, a bedace smieciem.
inline constexpr uint32_t LDR_RTC_MAGIC = 0x1DA70801;

// --- prog detektora "zostawione swiatlo" [mV] ---
// SUROWY prog na ldr_mv, CELOWO NIE `blLevel >= 1` i CELOWO nie cfg::LDR_DIM_DOWN_MV,
// mimo ze ta stala ma dzis przypadkiem te sama wartosc 400. Powod jest twardy: progi
// podswietlenia to DOKLADNIE TO, co jest przedmiotem sporu i co za tydzien zmienimy na
// podstawie histogramu powyzej. Gdyby detektor czytal cfg::LDR_DIM_DOWN_MV, to w chwili
// przesuniecia progu podswietlenia zmienilaby sie po cichu DEFINICJA zdarzenia — i tydzien
// zebranych zdarzen przestalby byc porownywalny z nastepnym tygodniem, bez ani jednej
// linijki zmiany w tym pliku. Detektor pomiarowy nie moze zalezec od wielkosci, ktora
// pomiar ma dopiero rozstrzygnac. Ta stala zyje wlasnym zyciem i ma stac.
//
// Skad 400: lapie prysznic (603) i kontroler (2576-3164), a odcina ciemnosc (4-5 mV).
// CO TO OBALA — 400 kontra "swiezo ciemno" 251 mV to tylko 1,6x zapasu i to jest MALO.
// Dlaczego mimo to OK: wymog 20 minut CIAGLOSCI daje odpornosc, ktorej chwilowy prog dac
// nie moze. Przelot przez 251 mV w drodze z 3164 do 4 mV trwa kilkanascie sekund i nigdy
// nie utrzyma sie przez 20 minut. TRWALOSC ZASTEPUJE TU PRECYZJE. Nie "poprawiaj" tego na
// wyzszy prog — wyzszy prog zgubilby prysznic (603), czyli dokladnie ten przypadek, dla
// ktorego ten detektor powstal.
inline constexpr uint16_t LDR_EVENT_ON_MV = 400;

// Ile sekund ciaglosci robi ze "swieci sie" ZDARZENIE. 20 min: dluzej, niz trwa
// najdluzszy sensowny prysznic, i dluzej, niz jakikolwiek przelot przez strefe sporna.
// To jest liczba WZIETA Z SUFITU i tak ma byc traktowana — po tygodniu ldr_events[] samo
// powie, czy 20 minut lapie realne "zostawione swiatlo", czy tylko dlugie kapiele.
inline constexpr uint32_t LDR_EVENT_MIN_S = 20 * 60;

// Zdarzenie "swiatlo swieci, a nikogo nie ma".
struct LdrEvent {
  uint32_t startEpoch;      // epoch poczatku CIAGLOSCI (nie momentu przekroczenia 20 min).
                            // 0 = ciaglosc zaczela sie, zanim NTP dal czas — wtedy godziny
                            // NIE ZNAMY i /api/diag pokaze null, zamiast zmyslac polnoc.
  uint32_t durS;            // dlugosc ciaglosci w sekundach REALNEGO pomiaru (patrz nizej)
  uint32_t lastPirBeforeS;  // ile sekund przed poczatkiem ciaglosci PIR widzial ruch ostatni
                            // raz. TO JEST POLE, KTORE ODROZNIA "wyszedl i zostawil" od
                            // "nigdy go tu nie bylo": male => ktos wyszedl i nie zgasil,
                            // duze => swiatlo palilo sie w pustej lazience juz wczesniej.
                            // 0xFFFFFFFF = od startu urzadzenia nie bylo ANI JEDNEGO ruchu,
                            // czyli "nie wiem" — /api/diag pokaze null.
};

struct LdrRtc {
  uint32_t magic;   // po zaniku zasilania RTC to SMIECI; rozstrzyga znacznik, a nie
                    // "czy liczby wygladaja rozsadnie" — smiec potrafi wygladac rozsadnie

  // --- ciaglosc pomiaru: dokladnie ta sama mechanika, co w PirRtc i z tego samego powodu ---
  // Osobne liczniki, a nie wspoldzielone z gPir: oba pomiary maja NIEZALEZNIE przezywac
  // podbicie cudzego magica (patrz nota przy LDR_RTC_MAGIC). Po zimnym starcie beda
  // identyczne — i to jest w porzadku, one nie sa od tego, zeby sie roznic, tylko od tego,
  // zeby sie NIE ZEROWAC nawzajem.
  uint32_t startedEpoch;  // epoch POCZATKU zbierania (0 = NTP jeszcze nie dal czasu)
  uint32_t collectedS;    // sekundy REALNEGO zbierania. To NIE to samo, co
                          // (teraz - startedEpoch): tamto liczy TAKZE czas, gdy urzadzenie
                          // sie restartowalo i NIC nie mierzylo. Roznica obu = ile pomiaru
                          // zjadly restarty, i dlatego /api/diag pokazuje OBA.
  uint32_t boots;         // ile startow przezyl ten komplet danych (OTA/panic/watchdog)

  // --- 1. histogram jasnosci ---
  // Zliczany w istniejacym bloku probkowania co 250 ms, czyli 4 probki/s. Po tygodniu:
  // 7*86400*4 = 2,4 mln zliczen — uint32 (4,3 mld) ma tu ~1800x zapasu, przepelnic sie
  // nie ma jak nawet gdyby pomiar chodzil lata.
  // Kosze [mV], krawedzie zageszczone w strefie spornej 256-768 (uzasadnienie wyzej):
  //   <8 | 8-16 | 16-32 | 32-64 | 64-128 | 128-256 | 256-384 | 384-512 | 512-640 |
  //   640-768 | 768-1024 | 1024-1536 | 1536-2048 | 2048-2560 | 2560-3072 | >=3072
  uint32_t hist[16];

  // --- 2. sekundy na poziomach podswietlenia 0/1/2 ---
  // PO CO: nie wiem, czy poziom "polmrok" (BL_DIM) w ogole WYSTEPUJE w tej lazience dluzej
  // niz ulamek sekundy przy pstryknieciu wlacznika. Pasmo 650-2200 mV jest szerokie na
  // papierze, ale zmierzone stany to 4-5 (ciemno), 603-617 (prysznic — ponizej 650, czyli
  // nadal poziom 0!) i 2576-3164 (swiatlo, czyli poziom 2). MIEDZY nimi nie ma nic, co by
  // tam siedzialo na stale. Jesli po tygodniu ldr_level_s[1] wyjdzie ~0 s, to znaczy, ze
  // BL_DIM = 130 jest MARTWYM KODEM, ktory sam sobie wpisalem, i trzeba go usunac razem
  // z cala srodkowa gałęzią kaskady. Jesli wyjdzie duzo — poziom zyje i zostaje.
  // Tego nie da sie wyczytac z logu: LOG leci tylko przy ZMIANIE poziomu i siedzi w
  // buforze KOLOWYM 3072 B (~47 linii, okno ~6 minut).
  uint32_t levelS[3];

  // --- 3. pierscien zdarzen "zostawione swiatlo" ---
  // 8 slotow. Pierscien, a nie licznik: sam licznik ("bylo 12 zdarzen") nie odpowiada na
  // pytanie, ktore ma sens ("o ktorej i jak dlugo") — a pelnej listy nie ma gdzie trzymac.
  LdrEvent events[8];
  uint32_t evHead;   // nastepny slot do nadpisania (pierscien nadpisuje NAJSTARSZE)
  uint32_t evCount;  // ile zdarzen OD POCZATKU. Moze byc > 8 i wtedy wiadomo, ze pierscien
                     // sie zawinal i najstarsze przepadly — bez tego pola pelny pierscien
                     // wygladalby identycznie przy 8 zdarzeniach i przy 80.

  // --- stan kandydata na zdarzenie (w RTC, zeby OTA nie gubilo trwajacego zdarzenia) ---
  // "Kandydat" = trwajaca ciaglosc swiatla bez ruchu, ktora jeszcze nie dobila 20 minut.
  // Gdyby to bylo w DRAM, kazde wydanie w srodku zdarzenia zerowaloby licznik ciaglosci
  // i 19-minutowa ciaglosc zaczynalaby sie od nowa — czyli detektor gubilby dokladnie te
  // zdarzenia, ktore trwaja najdluzej. To ten sam blad, co trzymanie collectedS w DRAM.
  uint32_t candStartS;      // collectedS w chwili startu ciaglosci — dlugosc liczymy jako
                            // ROZNICE collectedS, a nie epochow. To istotne: collectedS nie
                            // tyka podczas restartu, wiec 20 s przerwy na OTA nie zostanie
                            // policzone jako 20 s swiecenia, ktorego NIE WIDZIELISMY.
  uint32_t candStartEpoch;  // epoch tej samej chwili, tylko do pokazania czlowiekowi (0 = brak NTP)
  uint32_t candLastPirS;    // migawka "ile temu byl ruch" z chwili startu ciaglosci
  uint32_t candActive;      // 0/1. uint32, a nie bool, WYLACZNIE dla wyrownania — cala
                            // struktura ma byc z pol 4-bajtowych pod adresami /4 (patrz
                            // nota o wyrownaniu przy PirRtc)
  uint32_t evSlot;          // slot juz otwartego zdarzenia (dopisujemy do niego durS),
                            // 0xFFFFFFFF = ciaglosc trwa, ale jeszcze nie dobila 20 minut
};

// UCZCIWOSC POMIARU — jedna dziura, ktorej NIE zalatalem i ktorej nie warto latac:
// jesli w trakcie kandydata wypadnie restart (OTA ~20 s), to swiatlo mogloby w tym czasie
// zgasnac i zapalic sie z powrotem, a my scalimy to w jedna ciaglosc. Realnie: okno ma
// kilkanascie sekund, a scenariusz wymaga, zeby ktos pstryknal wlacznikiem dokladnie
// wtedy i NIE wszedl do lazienki (bo ruch i tak zamyka kandydata). Skala bledu jest
// widoczna wprost w ldr_meas.gap_s — u gPir wyszlo 5 s na 3 restarty. Zaslanianie tego
// heurystyka ("po restarcie odrzuc kandydata") kosztowaloby wiecej zdarzen, niz ratuje.
extern LdrRtc gLdr;

// Sprawdza znacznik waznosci, zeruje po zimnym starcie, seeduje liczniki DRAM-owe.
// Wolac w setup() razem z pirRtcBegin().
void ldrRtcBegin();

// Kubelek histogramu dla ldr_mv. W naglowku, bo Portal.cpp opisuje TE SAME krawedzie
// tekstem w /api/diag i chce miec je w zasiegu wzroku obok siebie.
uint8_t ldrHistBucket(uint16_t mv);
