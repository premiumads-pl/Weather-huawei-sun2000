#pragma once

#include <Arduino.h>            // millis()
#include <WiFiClient.h>         // typedef NetworkClient       (WiFiClient.h w rdzeniu 3.3.10)
#include <WiFiClientSecure.h>   // typedef NetworkClientSecure (WiFiClientSecure.h, 3 linijki)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>

#include "Log.h"   // LOG() — slad po zadzialaniu terminu ma trafic do /api/log

// =============================================================================
// GuardedClient<Base> — klient TCP, ktory (1) ODDAJE PROCESOR i (2) MA TERMIN
// =============================================================================
//
// NAZWA PLIKU ZOSTALA STARA (SecureClient.h), zeby nie ruszac osmiu istniejacych
// #include; tresc juz nie dotyczy wylacznie TLS — patrz aliasy na koncu pliku.
//
// -----------------------------------------------------------------------------
// CZESC 1: ODDAWANIE PROCESORA (bylo tu przed v157, tresc bez zmian merytorycznych)
// -----------------------------------------------------------------------------
//
// PO CO TA DZIWNA PODKLASA (bo za pol roku to pytanie na pewno padnie):
// bez niej urzadzenie wieszalo sie na Task watchdogu, zadanie `net`, komunikat
// "IDLE0 (CPU 0) did not reset the watchdog in time". Zrzut awaryjny (rozszyfrowany
// z ELF-a odtworzonego bit w bit) mial CALY stos w bibliotekach rdzenia, ani jednej
// naszej ramki:
//     NetworkClientSecure::available() -> data_to_read() -> mbedtls_ssl_read()
//     -> lwip_recvfrom() -> sys_mutex_lock()
// Czyli nie bylo czego poprawiac u nas — trzeba bylo poprawic to, JAK wolamy rdzen.
//
// MECHANIZM, KROK PO KROKU (wszystko odczytane z kodu rdzenia esp32 3.3.10, nie
// zgadniete — sciezki wzgledem katalogu pakietu .../hardware/esp32/3.3.10):
//
//  1. libraries/NetworkClientSecure/src/ssl_client.cpp:91 ustawia swiezo otwartemu
//     gniazdu O_NONBLOCK:
//         fcntl(ssl_client->socket, F_SETFL, fcntl(..., F_GETFL, 0) | O_NONBLOCK);
//     i NIGDY tego nie zdejmuje. Zwyczajny (nieszyfrowany) NetworkClient robi
//     dokladnie ODWROTNIE — libraries/Network/src/NetworkClient.cpp:298 kasuje ten
//     bit zaraz po connect():
//         fcntl(sockfd, F_SETFL, fcntl(..., F_GETFL, 0) & (~O_NONBLOCK));
//     Ta jedna roznica jest zrodlem wszystkiego ponizej i dotyczy WYLACZNIE TLS.
//
//  2. Przez O_NONBLOCK NetworkClientSecure::read() na pustym gniezdzie zwraca -1
//     (a nie 0, jak wariant blokujacy). To zabija zabezpieczenie, ktore rdzen JUZ MA:
//     NetworkClient::readBytes() (NetworkClient.cpp:513-539) wyglada tak:
//         r = read((uint8_t*)buffer + sofar, left);   // linia 517
//         if (r < 0) { break; }              // <-- TLS wychodzi TUTAJ, linie 518-521
//         if (r > 0) { ...licz dane... }
//         else { if (millis() >= to) break;  // timeout, linie 529-533
//                delay(2); }                 // "Allow other tasks to run", linia 535
//     Przy TLS r wynosi -1, wiec sterowanie leci w `break` z linii 520, a galaz
//     `delay(2)` z linii 535 jest MARTWYM KODEM. readBytes() wraca od razu z zerem.
//
//  3. Wyzej stoi HTTPClient::writeToStreamDataBlock()
//     (libraries/HTTPClient/src/HTTPClient.cpp:1303). W wariancie z Content-Length
//     (len > 0) petla wyglada tak:
//         while (connected() && (len > 0 || len == -1)) {   // linia 1318
//           size_t sizeAvailable = buff_size;      // available() NIE jest wolane!
//           ...
//           int bytesRead = _client->readBytes(buff, readBytes);   // linia 1346, -> 0
//           ...
//           len -= bytesRead;                      // linia 1390: len -= 0, czyli STOI
//           delay(0);                              // linia 1393
//         }
//     Ta petla NIE MA ZADNEGO TIMEOUTU. Gdy readBytes() zaczyna zwracac zera, kreci
//     sie w nieskonczonosc, a jedyne oddanie procesora to `delay(0)`.
//
//  4. `delay(0)` na ESP32 to nie vTaskDelay, tylko `portYIELD()` — czyli przelaczenie
//     na zadanie o priorytecie NIE NIZSZYM niz biezace. netTask ma priorytet 3
//     (xTaskCreatePinnedToCore(..., 3, &gNetTask, 0) w pogoda-gdynia.ino), a IDLE0 ma
//     priorytet 0. IDLE0 nie dostaje wiec ANI JEDNEGO takta. A to wlasnie IDLE0
//     karmi Task watchdoga: CONFIG_ESP_TASK_WDT_TIMEOUT_S=5 i IDLE0 na liscie
//     pilnowanych => po 5 sekundach takiego kreciolka leci panic.
//
// CO ROBI TA KLASA (czesc 1): gdy gniazdo nie ma nic do oddania (a <= 0 albo r <= 0),
// zamiast wrocic natychmiast i pozwolic wolajacemu krecic sie dalej, spi 1 takt przez
// vTaskDelay(1). CONFIG_FREERTOS_HZ=1000, wiec to realna 1 ms prawdziwego snu —
// zadanie schodzi z procesora, IDLE0 dostaje czas, karmi watchdoga i panic nie ma
// prawa wystapic. Gdy dane SA (a > 0 / r > 0) nie robimy NIC — sciezka szybka jest
// nietknieta, wiec przepustowosc (w tym pobieranie OTA) nie cierpi.
//
// CZY TA GALAZ JEST POTRZEBNA DLA BAZY `WiFiClient` (bez TLS)? NIE — i to jest
// swiadoma decyzja, zeby ja mimo to zostawic wspolna dla obu baz. Dla NetworkClient
// bit O_NONBLOCK jest skasowany (NetworkClient.cpp:298), wiec read() na pustym
// gniezdzie zwraca 0, a nie -1; galaz `delay(2)` z NetworkClient.cpp:535 ZYJE i sama
// karmi IDLE0. Nasze vTaskDelay(1) jest tam wiec NADMIAROWE, ale NIESZKODLIWE:
// doklada najwyzej 1 ms do tych wywolan read()/available(), ktore i tak nie przyniosly
// bajtu (przy blokujacym gniezdzie zdarza sie to rzadko — read() wraca dopiero
// z danymi albo po uplywie SO_RCVTIMEO). Rozdzielanie tego na dwie wersje klasy
// oznaczaloby duplikat calego kodu terminu, a to jest gorsze niz 1 ms.
//
// -----------------------------------------------------------------------------
// CZESC 2 (v157): TERMIN BEZCZYNNOSCI
// -----------------------------------------------------------------------------
//
// AWARIA, KTORA TO ZAMYKA: 25.07 urzadzenie stalo 33 minuty (i staloby bez konca).
// Wlasciciel zaktualizowal firmware punktu dostepowego w polowie pobierania mapy
// radaru; druga strona przepadla BEZ RST, wiec gniazdo zostalo NA WPOL OTWARTE.
// lwIP dalej melduje wtedy "polaczony" i to jest udokumentowane w rdzeniu:
// NetworkClient::connected() (NetworkClient.cpp:571) sprawdza gniazdo przez
//     recv(fd(), &dummy, 1, MSG_DONTWAIT | MSG_PEEK);          // linia 577
// i przy errno == EWOULDBLOCK USTAWIA `_connected = true` (linia 585). Dla gniazda,
// z ktorego nigdy juz nic nie przyjdzie, wyglada to identycznie jak dla gniazda,
// ktore po prostu chwilowo milczy. Rdzen nie ma jak ich odroznic — my mamy, bo
// znamy uplyw czasu.
//
// DZWIGNIA TO `connected()`, i to wystarcza — sprawdzone w zrodle, gdzie rdzen go wola:
//   * HTTPClient.cpp:398-403 — HTTPClient::connected() to
//         return ((_client->available() > 0) || _client->connected());
//     czyli KAZDE pytanie rdzenia o "czy jeszcze polaczony" przechodzi przez NASZE
//     dwie metody. Kolejnosc jest dla nas korzystna: gdy w buforze COS jeszcze lezy
//     (available() > 0), petla leci dalej i to jest poprawne — te bajty trzeba odebrac,
//     a odbior odswiezy znacznik. Termin bierze gore dopiero gdy bufor jest pusty.
//   * HTTPClient.cpp:1318 — `while (connected() && (len > 0 || len == -1))` w
//     writeToStreamDataBlock(). TO JEST TA PETLA BEZ TIMEOUTU. Nasze connected()
//     zwracajace 0 jest jedynym sposobem, zeby z niej wyjsc. Obejmuje CALE ciala
//     odpowiedzi: http.getString(), http.writeToStream() oraz wariant chunked.
//   * HTTPClient.cpp:1198 — `while (connected())` w handleHeaderResponse(). Tu nasz
//     termin jest tylko druga linia obrony: ta petla MA wlasny timeout
//     (HTTPClient.cpp:1286-1288, `if ((millis() - lastDataTime) > _tcpTimeout)
//     return HTTPC_ERROR_READ_TIMEOUT;`), wiec faza naglowkow nigdy nie wisiala.
//   * HTTPClient.cpp:367 — `if (connected())` w disconnect(). Skutek uboczny naszego
//     `false`: rdzen NIE wola juz `_client->stop()` (linia 377). Gniazda to jednak
//     NIE przecieka i sprawdzilem to w zrodle, a nie zalozylem: deskryptor trzyma
//     std::shared_ptr<NetworkClientSocketHandle>, ktorego destruktor
//     (NetworkClient.cpp:170-172) wola close() (linie 174-179). Wszystkie nasze
//     instancje sa LOKALNE (na stosie funkcji), wiec zamykaja sie przy wyjsciu
//     z funkcji, kilka linijek po http.end(). Dlatego celowo NIE wolamy stop()
//     wewnatrz connected() — mniej ingerencji w rdzen przy tym samym efekcie.
//   * HTTPClient.cpp:1109 — `_client->connect(_host.c_str(), _port, _connectTimeout)`
//     ma juz wlasny timeout, a linia 1115 (`_client->setTimeout(_tcpTimeout)`)
//     ustawia timeout POJEDYNCZEGO odczytu. Zadnego z nich nie ruszamy.
//
// CZEGO connected() NIE PRZYKRYWA — I MOWIMY TO WPROST:
// pobierania firmware'u OTA. Ota::downloadAndFlash() nie idzie przez
// writeToStreamDataBlock(), tylko przez UpdateClass::writeStream()
// (libraries/Update/src/Updater.cpp:863), ktory czyta ze strumienia BEZPOSREDNIO
// i o connected() nigdy nie pyta. Ta petla ma jednak WLASNY, sprawny bezpiecznik,
// tez sprawdzony w zrodle (Updater.cpp:902-912):
//     while (!toRead) {
//       toRead = data.readBytes(_buffer + _bufferLen, bytesToRead);
//       if (toRead == 0) { timeout_failures++;
//                          if (timeout_failures >= 300) { _abort(UPDATE_ERROR_STREAM); ... }
//                          delay(100); } }
// Przy TLS readBytes() wraca z zerem natychmiast (patrz punkt 2 wyzej), wiec 300 prob
// po 100 ms daje ~30 s i pobieranie samo sie poddaje. Termin zalozony na kliencie OTA
// dziala tam wiec realnie tylko na fazie zadania i naglowkow — i tak ma byc: nie
// dokladamy niczego do sciezki, ktora JUZ sie sama konczy, a od ktorej zalezy
// mozliwosc zdalnej naprawy urzadzenia.
//
// DLACZEGO TERMIN JEST BEZCZYNNOSCIOWY, A NIE CALKOWITY — TO JEST NAJWAZNIEJSZE
// ZDANIE W TYM PLIKU. Liczymy czas od OSTATNIEGO faktycznie odebranego bajtu, a nie
// od poczatku transferu. Urzadzenie jest TYLKO-OTA (bez USB), a wlasna aktualizacja
// to 1,8 MB przez TLS; przy slabym sygnale takie pobranie legalnie trwa minutami.
// Termin CALKOWITY ubilby wlasnie takie POSTEPUJACE pobranie — czyli zamienilby
// zabezpieczenie w cegielnice. Znacznik odswiezamy WYLACZNIE tam, gdzie bajty
// naprawde przechodza (obie wersje read(); readBytes() z rdzenia i tak schodzi do
// read() — NetworkClient.cpp:517), i NIGDY w connected(): connected() jest wolane
// w kolko przez petle rdzenia, wiec odswiezanie znacznika tam znaczyloby "polaczenie
// zyje, bo ktos pyta, czy zyje".
//
// UZBRAJANIE — DWUSTOPNIOWE, I DLATEGO:
//   * armIdleGuard(ms, nazwa) wolane JAWNIE w miejscu uzycia, tuz po utworzeniu
//     klienta. Jawnie, bo dobor wartosci jest decyzja tego konkretnego miejsca
//     (OTA ma inna niz pogoda) i ma byc widoczny pod okiem czytajacego kod, a nie
//     schowany w domysle. Brak wywolania = brak terminu (idleLimitMs_ == 0), czyli
//     zachowanie DOKLADNIE takie jak przed v157 — nowa klasa niczego nie narzuca.
//   * connect() nadpisane i zeruje znacznik przy KAZDYM nowym polaczeniu. To zamyka
//     dziure "drugi transfer na tym samym obiekcie dziedziczy wyczerpany termin"
//     bez polegania na pamieci programisty: HTTPClient wola connect() osobno dla
//     kazdego zadania i osobno po kazdym przekierowaniu (HTTPClient.cpp:1109), a my
//     w kilku miejscach uzywamy HTTPC_STRICT_FOLLOW_REDIRECTS. Nadpisanie jest
//     legalne: `virtual int connect(const char*, uint16_t, int32_t) = 0` stoi
//     w ESPLwIPClient (NetworkClient.h:34) — to WLASNIE ta wersja, ktora wola
//     HTTPClient, a nie dwuargumentowa z Client.h:29.
//
// SLAD W /api/log: wpis leci DOKLADNIE RAZ na klienta (pilnuje tego `expired_`)
// i mowi WPROST, ze to byl termin bezczynnosci, a nie blad serwera — bo od strony
// wolajacego oba koncza sie tym samym: http.getString() oddaje kawalek, a
// writeToStream() zwraca HTTPC_ERROR_STREAM_WRITE (-8). Bez tego wpisu za pol roku
// nikt by nie odroznil "druga strona przepadla" od "serwer zwrocil blad". Bufor
// /api/log ma 3072 B (~6 minut), a taki wpis powstaje tylko przy realnej awarii
// gniazda, wiec logu nie zasmieca.
//
// -----------------------------------------------------------------------------
// SPRAWY WSPOLNE
// -----------------------------------------------------------------------------
//
// DLACZEGO TU, A NIE W BIBLIOTECE: poprawka nalezy do rdzenia (ssl_client.cpp albo
// writeToStreamDataBlock()), ale forkowanie pakietu rdzenia oznaczaloby wlasny
// board manager i reczne scalanie przy kazdej aktualizacji. Podklasa kosztuje po
// jednej vtable we flashu na baze, kilkanascie bajtow na instancje (dwa uint32,
// wskaznik i bool) i ZERO bajtow w .bss — wszystkie nasze instancje sa lokalne,
// na stosie.
//
// DLACZEGO `override` W OGOLE PRZECHODZI: NetworkClient(Secure).h nie pisze przy
// tych metodach slowa `virtual`, ale to bez znaczenia — wirtualnosc jest dziedziczona.
// cores/esp32/Client.h deklaruje:
//         virtual int available() = 0;              // :32
//         virtual int read() = 0;                   // :33
//         virtual int read(uint8_t*, size_t) = 0;   // :34
//         virtual uint8_t connected() = 0;          // :38
// a lancuch dziedziczenia to Client -> ESPLwIPClient -> NetworkClient (= WiFiClient)
// -> NetworkClientSecure (= WiFiClientSecure). Sygnatury ponizej sa z nimi zgodne
// co do znaku, wiec `override` faktycznie NADPISUJE. Przy zlej sygnaturze `override`
// jest bledem kompilacji, a nie cichym utworzeniem przeciazenia — i o to nam chodzi.
//
// KTO TO WYWOLA: HTTPClient trzyma `NetworkClient* _client` (begin(NetworkClient&, ...)),
// Update.writeStream() dostaje `Stream&` z HTTPClient::getStream() (zwraca *_client),
// a NetworkClient::readBytes() wola `read(...)` na `this`. Wszystkie trzy sciezki ida
// przez wskaznik/referencje do klasy bazowej, czyli przez vtable — nasze nadpisania
// dostana sterowanie.
//
// UWAGA PRZY EDYCJI: nie zamieniaj vTaskDelay(1) na delay(1). Wyszloby na to samo
// tylko przypadkiem (delay() dla argumentow >= 1 ms schodzi do vTaskDelay), ale to
// wlasnie mylenie tych dwoch — `delay(0)` w rdzeniu — jest CALA przyczyna tej awarii.
// vTaskDelay jest tu jawne celowo.

namespace netguard {

// TERMIN DLA ZWYKLYCH ZAPYTAN — 30 s bezczynnosci.
// Liczba nie jest okragla dla ozdoby: ma byc SCISLE WIEKSZA od najdluzszego timeoutu
// POJEDYNCZEGO odczytu, jaki ustawia ktorykolwiek nasz klient. Najdluzszy to 25 s
// (Viessmann.cpp: apiGet -> http.setTimeout(25000); HTTPClient przenosi te wartosc
// na gniazdo w HTTPClient.cpp:1115). Gdyby termin byl mniejszy albo rowny, jedno
// LEGALNE, wolne zapytanie, ktore wyczerpuje wlasny timeout, wygladaloby jak awaria.
// Reszta ma zapas jeszcze wiekszy: pogoda i OTA-wersja 15 s, piec/token 15 s,
// piec/post 20 s, loty 12 s, powietrze 12 s, radar 12 s, mapa radaru 12 s,
// geokoder 10 s, bramka BLE 3 s.
// OGRANICZENIE OD DRUGIEJ STRONY: connected() jest sprawdzane MIEDZY odczytami,
// nie w ich trakcie, wiec pojedynczy transfer wisi najwyzej (termin + jeden pelny
// timeout odczytu). Najgorszy przypadek to piec: 30 + 25 = 55 s zamiast
// nieskonczonosci. Nadzorca netTask (900 s) nie ma juz szans do tego dojsc.
constexpr uint32_t kIdleMs = 30000;

// (v161) DLACZEGO PRZY ZADNYM KLIENCIE NIE MA JUZ `client.setTimeout(N)`.
// W dziesieciu miejscach stalo `client.setTimeout(15)` / `(12)` / `(kTimeoutMs/1000)`
// tuz nad `http.setTimeout(15000)`. Wygladalo to na "to samo, tylko w sekundach"
// i taka byla intencja — ale to byla nieprawda i linia nie robila NICZEGO:
//   * NetworkClient (= WiFiClient) i NetworkClientSecure NIE MAJA wlasnego
//     setTimeout. Wolanie schodzilo do Stream::setTimeout(unsigned long)
//     (cores/esp32/Stream.h:67, definicja Stream.cpp:86), ktory jest w
//     MILISEKUNDACH — komentarz w naglowku rdzenia mowi to wprost
//     ("sets maximum milliseconds to wait"). `setTimeout(15)` ustawialo 15 ms.
//   * `NetworkClient::_timeout` (NetworkClient.h:43) to INNE pole niz
//     `Stream::_timeout` (Stream.h:50) i to ono trafia do SO_RCVTIMEO w
//     NetworkClient::read() (NetworkClient.cpp:491-499). Ustawia je
//     setConnectionTimeout(), a nie setTimeout(). Dwa pola, mylaco podobne nazwy.
//   * A i tak nie mialo znaczenia, bo HTTPClient::connect() nadpisuje te wartosc
//     zaraz po nawiazaniu polaczenia: `_client->setTimeout(_tcpTimeout)`
//     (HTTPClient.cpp:1115), czyli tym, co podalismy w http.setTimeout(...).
// Poprawianie jednostki dalo by linie NADAL martwa, tylko wygladajaca na zywa —
// czyli gorzej. Dlatego linie USUNIETE, a jedynym miejscem, gdzie ustawia sie
// timeout odczytu, jest http.setTimeout(<ms>). NIE DOTYKAMY setHandshakeTimeout():
// to prawdziwe API NetworkClientSecure i naprawde bierze SEKUNDY
// (NetworkClientSecure.cpp:450-452 mnozy argument przez 1000).
// Zaden realny timeout nie zostal ta zmiana ruszony.

// TERMIN DLA POBIERANIA FIRMWARE'U OTA — 60 s, czyli dwa razy luzniej.
// Uzasadnieniem jest asymetria kosztow, nie technika: urzadzenie nie ma USB, wiec
// przedwczesne ubicie WLASNEJ aktualizacji jest awaria nie do naprawienia, a jedna
// nieudana proba OTA kosztuje tylko kwadrans do nastepnego sprawdzenia
// (cfg::OTA_CHECK_MS). Przy 1,8 MB i slabym sygnale minuta bez ANI JEDNEGO bajtu to
// juz nie "chwilowe zaciecie", tylko martwe gniazdo — a i tak, jak opisano wyzej,
// faza pobierania ciala ma swoj wlasny bezpiecznik ~30 s w Updater.cpp:902-912
// i to on zadziala pierwszy. Te 60 s pilnuje wiec realnie tylko fazy naglowkow.
constexpr uint32_t kOtaIdleMs = 60000;

}  // namespace netguard

template <class Base>
class GuardedClient : public Base {
 public:
  // idleMs == 0 wylacza termin (to jest stan poczatkowy). `what` musi byc napisem
  // o czasie zycia programu (literal) — trzymamy sam wskaznik, bez kopiowania.
  void armIdleGuard(uint32_t idleMs, const char* what) {
    idleLimitMs_ = idleMs;
    what_ = what;
    lastDataMs_ = millis();
    expired_ = false;
  }

  // Do sprawdzenia przez wolajacego, gdy chce ROZNICOWAC wlasny komunikat bledu.
  bool idleGuardFired() const { return expired_; }

  int available() override {
    const int a = Base::available();
    if (a <= 0) {
      vTaskDelay(1);
    }
    return a;
  }

  int read() override {
    const int c = Base::read();
    if (c >= 0) {
      lastDataMs_ = millis();   // bajt PRZESZEDL — tylko to odswieza termin
    } else {
      vTaskDelay(1);
    }
    return c;
  }

  int read(uint8_t* buf, size_t size) override {
    const int r = Base::read(buf, size);
    if (r > 0) {
      lastDataMs_ = millis();   // j.w.; 0 znaczy "nic nie przyszlo" i NIE odswieza
    } else {
      vTaskDelay(1);
    }
    return r;
  }

  uint8_t connected() override {
    if (idleLimitMs_ != 0 && !expired_ &&
        (millis() - lastDataMs_) >= idleLimitMs_) {
      expired_ = true;
      // Raz na klienta: `expired_` nie wroci do false bez armIdleGuard() albo
      // connect(), wiec petla rdzenia nie zaleje /api/log.
      LOG("NET: %s — TERMIN BEZCZYNNOSCI po %lu s bez ani jednego bajtu, zrywam "
          "transfer (gniazdo na wpol otwarte, to NIE blad serwera)\n",
          what_ != nullptr ? what_ : "?",
          static_cast<unsigned long>(idleLimitMs_ / 1000));
    }
    if (expired_) {
      return 0;
    }
    return Base::connected();
  }

  // Nadpisana WERSJA TRZYARGUMENTOWA, bo dokladnie ta wola HTTPClient
  // (HTTPClient.cpp:1109). Nowe polaczenie = nowy termin; patrz "UZBRAJANIE" wyzej.
  int connect(const char* host, uint16_t port, int32_t timeout) override {
    const int r = Base::connect(host, port, timeout);
    lastDataMs_ = millis();
    expired_ = false;
    return r;
  }

  // Zadeklarowanie read(uint8_t*, size_t) UKRYWA w tej klasie wszystkie pozostale
  // przeciazenia `read` z bazy (w tym bezargumentowe, gdyby ktos je usunal wyzej),
  // a `connect` powyzej ukrywa pozostale wersje connect — m.in. te z certyfikatami
  // w NetworkClientSecure. Te dwa `using` je przywracaja. Nasze wlasne deklaracje
  // maja pierwszenstwo przed sciagnietymi przez `using` (ta sama sygnatura = wersja
  // z klasy pochodnej wygrywa), wiec nie powstaje tu zadna dwuznacznosc.
  using Base::read;
  using Base::connect;

 private:
  uint32_t lastDataMs_ = 0;
  uint32_t idleLimitMs_ = 0;      // 0 = bez terminu
  const char* what_ = nullptr;    // nazwa transferu, wylacznie do logu
  bool expired_ = false;
};

// Aliasy. `YieldingSecureClient` zostaje pod stara nazwa CELOWO: uzywa go osiem
// miejsc w projekcie i zadne z nich nie musialo sie przez v157 zmienic co do typu.
using YieldingSecureClient = GuardedClient<WiFiClientSecure>;
using GuardedPlainClient = GuardedClient<WiFiClient>;
