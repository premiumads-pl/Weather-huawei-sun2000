#pragma once

#include <cstdint>

// (v180) Koszt energii KUPIONEJ Z SIECI od polnocy — druga (po AutoData.h) porcja
// danych, ktore w tym projekcie PRZYCHODZA po MQTT, zamiast byc przez nas odpytane.
// Publikuje je Home Assistant na temat <prefix>/dom/stan co 60 s, BEZ retained;
// MqttClient.cpp to subskrybuje i parsuje.
//
// DLACZEGO NIE LICZYMY TEGO SAMI: firmware zna moc chwilowa z sieci (PvClient czyta
// rejestry miernika), ale NIE zna STAWKI — w taryfie G12w cena zalezy od strefy,
// czyli od godziny i dnia tygodnia, a same strefy zmieniaja sie razem z umowa.
// Policzenie kosztu tutaj oznaczaloby wpisanie cennika do firmware'u tylko-OTA
// i pilnowanie go przy kazdej zmianie taryfy. Home Assistant sumuje to minuta po
// minucie i ma cennik w JEDNYM miejscu — my dostajemy gotowa liczbe.
//
// TEMAT NAZYWA SIE "dom/stan", A NIE "koszt": to jest stan domu, a `zl` jest jego
// PIERWSZYM polem. Kolejne (np. koszt oddania, licznik wody) maja dochodzic tutaj,
// bez nowego tematu i bez trzeciej subskrypcji — dlatego parser w MqttClient.cpp
// czyta pola po nazwie i nie wymaga kompletu.
struct CostModel {
  // millis() ODBIORU wiadomosci (0 = nie przyszla ani jedna od uruchomienia).
  // DOKLADNIE ten sam idiom, co AutoModel::atMs: swiezosc liczy freshMs()
  // z WeatherUiV3.cpp, czyli roznica ZE ZNAKIEM na int32 — millis() przekreca sie
  // po ~49 dniach pracy, a pisze go INNY watek (netTask) niz ten, ktory czyta
  // (petla rysowania). Prog: cfg::COST_STALE_MS.
  uint32_t atMs = 0;

  // Koszt energii pobranej z sieci OD POLNOCY [PLN]. Zeruje sie o polnocy PO
  // STRONIE Home Assistanta — my tej granicy nie znamy, wiec jej nie zgadujemy
  // i nigdy sami nie zerujemy tego pola.
  float zl = 0.f;

  // (v181) SKUMULOWANA korzysc z fotowoltaiki od uruchomienia instalacji [PELNE PLN].
  // DRUGIE pole tego ladunku i pierwszy dowod, ze temat "dom/stan" mial byc
  // rozszerzalny (patrz naglowek pliku) — nowy temat ani trzecia subskrypcja nie byly
  // potrzebne. Zasila ekran ZWROT (cfg::VIEW_PAYBACK).
  //
  // NARASTAJACO, nie "dzisiaj": w odroznieniu od `zl` wyzej ta liczba NIGDY sie nie
  // zeruje i tylko rosnie. Licznik zycia instalacji trzyma Home Assistant, my go
  // wylacznie wyswietlamy.
  //
  // int32_t, a nie float: to sa PELNE ZLOTE (grosze przy kwocie rzedu 13 000 zl sa
  // szumem, a na ekranie i tak ich nie widac), a typ ze znakiem bierze sie stad, ze
  // roznica (cfg::PV_KOSZT_PLN - pvPln) ma prawo zejsc PONIZEJ ZERA w dniu splaty.
  // 4 B zamiast 2: uint16_t skonczylby sie na 65 535 zl, czyli — przy tempie ~334
  // zl/mies. — okolo roku 2039, a ta instalacja ma zyc dluzej niz to przepelnienie.
  int32_t pvPln = 0;

  // (v182) TRZECIE pole ladunku "dom/stan": STREFA TARYFY G12w w tej chwili.
  //   -1 = NIE WIEM (wartosc poczatkowa i jedyna, przy ktorej plakietki NIE MA)
  //    0 = strefa TANIA
  //    1 = strefa DROGA
  //
  // TARYFY NIE LICZYMY NA URZADZENIU I TO JEST DECYZJA, NIE LENISTWO. Godzine mamy
  // (NTP), wiec pokusa "przeciez to jest jeden if na godzine" jest realna — i falszywa.
  // Strefa G12w zalezy od TRZECH rzeczy naraz: godziny, DNIA TYGODNIA (soboty i
  // niedziele sa w calosci tanie) oraz SWIAT USTAWOWYCH, ktore chodza po kalendarzu
  // (Wielkanoc, Boze Cialo) i ktorych nie da sie zapisac tablica dat bez corocznej
  // aktualizacji firmware'u tylko-OTA. Kalendarz swiat siedzi juz w Home Assistancie
  // (integracja Workday, kraj PL) i jest tam UTRZYMYWANY. Wyswietlacz ma pokazac
  // plakietke, a nie odtwarzac drugi, gorszy kalendarz — dokladnie ta sama zasada,
  // ktora kazala liczyc `zl` po stronie HA (patrz naglowek tego pliku).
  //
  // int8_t, a nie bool: bool ma DWA stany, a my mamy TRZY i trzeci ("nie wiem") jest
  // tym, na ktorym zalezy najbardziej. Zla plakietka jest GORSZA niz jej brak, bo
  // wlasciciel podejmuje na jej podstawie decyzje o ladowaniu auta — brak informacji
  // kaze sprawdzic, blednie zielona plakietka kaze wlaczyc ladowarke w szczycie.
  // Bajt jest darmowy: bez niego struktura ma 12 B, z nim 13 B dopelnione do 16 B,
  // czyli sizeof NIE ROSNIE ani o piksel RAM-u (patrz straznik nizej).
  int8_t tariff = -1;

  // (v192) CZWARTE pole: PRZYCHOD ZE SPRZEDAZY energii od polnocy, W GROSZACH.
  // Rysowane na ekranie GLOWNYM po LEWEJ od "zakup dziś", w tej samej linii.
  //
  // GROSZE NA uint16_t, A NIE ZLOTOWKI NA float — I TO NIE JEST MIKROOPTYMALIZACJA.
  // Straznik pod ta struktura zapowiedzial wprost, ze czwarte pole 4-bajtowe ma sie
  // ZATRZYMAC i wymusic decyzje: po `tariff` zostaly DOKLADNIE 3 BAJTY DOPELNIENIA.
  // float wypchnalby sizeof z 16 B na 20 B w DWOCH kopiach struktury naraz. uint16_t
  // wchodzi w to dopelnienie, wiec sizeof NIE DRGNAL. Straznik zadzialal drugi raz
  // z rzedu dokladnie tak, jak byl pomyslany — dlatego progu nadal nie ruszamy.
  //
  // BEZ ZNAKU, bo dobowy licznik przychodu nie ma prawa zejsc ponizej zera (to suma
  // energii ODDANEJ, a nie bilans) — w odroznieniu od pvPln wyzej, ktory zejsc moze
  // i wlasnie dlatego jest ze znakiem.
  //
  // SUFIT: 655,35 zl na dobe. Instalacja o mocy ~10 kWp wyprodukuje najwyzej okolo
  // 60 kWh dziennie, co przy NAJWYZSZEJ stawce RCEm, jaka pojawila sie na fakturach
  // (0,55196 zl/kWh netto, styczen), daje maksimum rzedu 41 zl. Zapas ponad
  // pietnastokrotny; sufit jest fizycznie nieosiagalny, a nie "raczej wystarczy".
  //
  // WARTOSC JEST JUZ PO KOREKCIE 0,956 — mnozenie robi Home Assistant, bo wspolczynnik
  // siedzi w input_number.korekta_rozliczenia i ma byc poprawiany bez wgrywania nowej
  // wersji firmware'u. Wyswietlacz dostaje liczbe gotowa, tak samo jak `zl`.
  //
  // WIEKU NIE MA WLASNEGO i mieć nie może: przychodzi TA SAMA wiadomoscia, co `zl`,
  // wiec dzieli z nim atMs. Dwie liczby z jednego ladunku nie moga miec dwoch roznych
  // wiekow (ta sama zasada, co przy pvPln — opis w Config.h przy COST_STALE_MS).
  uint16_t sellGr = 0;
};

// Straznik budzetu, nie ozdobnik — ta sama rola, co static_assert przy AutoModel.
// Model zyje w DWOCH kopiach (odbiorczej w MqttClient.cpp i ekranowej uiCost
// w pogoda-gdynia.ino), wiec kazdy dolozony bajt kosztuje DWA bajty statycznego
// RAM-u przy barierze 76 000 B (tools/release.sh). (v181) Dzis 12 B — doszlo pvPln
// (4 B) i z zapasu, ktory prog 16 B mial dac "na jeszcze jedno-dwa pola", zostalo
// dokladnie JEDNO 4-bajtowe pole. Progu NIE PODNOSIMY razem z rozmiarem: straznik,
// ktory ustepuje przy kazdym dolozeniu, nie pilnuje juz niczego. Trzecie pole ma sie
// tu ZATRZYMAC i wymusic decyzje, a nie przejsc mimochodem.
//
// (v182) Trzecie pole PRZYSZLO i straznik zadzialal tak, jak mial: `tariff` weszlo
// jako int8_t WLASNIE dlatego, ze prog kazal sie zastanowic nad rozmiarem, zanim
// odruchowo wpisze sie `int`. Bilans: 4 + 4 + 4 + 1 = 13 B, dopelnione do 16 B przez
// wyrownanie do 4 B najszerszego pola — sizeof NIE DRGNAL, a prog zostaje 16.
// CO ZOSTALO: 3 BAJTY DOPELNIENIA, i tylko tyle. Kolejne pole zmiesci sie za darmo
// WYLACZNIE gdy bedzie 1-bajtowe (int8_t/bool/enum : uint8_t); czwarte 4-bajtowe
// przebije 16 B i ma sie tu ZATRZYMAC dokladnie jak to, przed chwila.
//
// (v192) I ZATRZYMALO SIE. Czwarte pole przyszlo jako przychod ze sprzedazy i
// odruch mowil "float, tak jak zl obok". Prog kazal policzyc: float to 20 B razy
// dwie kopie. Wiec grosze na uint16_t, ktore weszly W DOPELNIENIE — 4+4+4+1+2 = 15 B,
// dopelnione do 16 B, sizeof BEZ ZMIAN drugi raz z rzedu. To jest cala wartosc tego
// progu: nie zabronil dolozyc pola, tylko kazal wybrac dla niego typ swiadomie.
// CO ZOSTALO TERAZ: 1 BAJT DOPELNIENIA. Piate pole musi byc 1-bajtowe albo przebije
// 16 B — i wtedy trzeba bedzie odpowiedziec na pytanie, ktorego dotad unikamy: czy
// ta struktura ma jeszcze rosnac, czy stan domu zasluguje na wlasny, osobny model.
static_assert(sizeof(CostModel) <= 16,
              "CostModel zyje w dwoch kopiach — powyzej 16 B zaczyna byc widoczny "
              "w budzecie statycznego RAM-u (bariera 76 000 B)");
// (v182) Straznik na TYP, jak przy pvPln nizej: `tariff` MUSI byc ze znakiem, bo -1
// ("nie wiem") jest jego wartoscia poczatkowa i jedynym stanem, przy ktorym plakietka
// taryfy sie NIE RYSUJE. Na uint8_t -1 stalby sie 255, warunek `tariff < 0` bylby
// zawsze falszywy i urzadzenie tuz po starcie — zanim przyjdzie pierwsza wiadomosc
// MQTT — pokazywaloby plakietke strefy, ktorej nie zna. To jest dokladnie ten blad,
// przed ktorym cala ta funkcja ma chronic.
static_assert(static_cast<decltype(CostModel::tariff)>(-1) < 0,
              "CostModel::tariff musi byc typem ZE ZNAKIEM (-1 = 'nie wiem' jest "
              "stanem poczatkowym i wylacza rysowanie plakietki taryfy)");
// (v181) Drugi straznik, tym razem na TYP, nie na rozmiar: pvPln MUSI byc ze znakiem.
// Zamiana na uint32_t nie ruszylaby sizeof (wiec asercja wyzej by tego nie zlapala),
// a wywrocilaby ekran ZWROT w dniu splaty instalacji — (PV_KOSZT_PLN - pvPln) < 0
// przekrecilby sie w ~4 mld i pasek postepu spadlby ze 100% na 0%.
static_assert(static_cast<decltype(CostModel::pvPln)>(-1) < 0,
              "CostModel::pvPln musi byc typem ZE ZNAKIEM (roznica koszt-korzysc "
              "schodzi ponizej zera po splacie instalacji)");
