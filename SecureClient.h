#pragma once

#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>

// =============================================================================
// YieldingSecureClient — klient TLS, ktory ODDAJE PROCESOR, gdy nie ma co czytac
// =============================================================================
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
//     NetworkClient::readBytes() (NetworkClient.cpp:513-538) wyglada tak:
//         r = read((uint8_t*)buffer + sofar, left);
//         if (r < 0) { break; }              // <-- TLS wychodzi TUTAJ, natychmiast
//         if (r > 0) { ...licz dane... }
//         else { if (millis() >= to) break;  // timeout
//                delay(2); }                 // "Allow other tasks to run"
//     Przy TLS r wynosi -1, wiec sterowanie leci w `break` z linii 520, a galaz
//     `delay(2)` z linii 535 jest MARTWYM KODEM. readBytes() wraca od razu z zerem.
//
//  3. Wyzej stoi HTTPClient::writeToStreamDataBlock()
//     (libraries/HTTPClient/src/HTTPClient.cpp:1303). W wariancie z Content-Length
//     (len > 0) petla wyglada tak:
//         while (connected() && (len > 0 || len == -1)) {
//           size_t sizeAvailable = buff_size;      // available() NIE jest wolane!
//           ...
//           int bytesRead = _client->readBytes(buff, readBytes);   // -> 0 (patrz 2.)
//           ...
//           len -= bytesRead;                      // -> len -= 0, czyli len STOI
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
// CO ROBI TA KLASA: dokladnie jedna rzecz — gdy TLS nie ma nic do oddania (a <= 0
// albo r <= 0), zamiast wrocic natychmiast i pozwolic wolajacemu krecic sie dalej,
// spi 1 takt przez vTaskDelay(1). CONFIG_FREERTOS_HZ=1000, wiec to realna 1 ms
// prawdziwego snu — zadanie schodzi z procesora, IDLE0 dostaje czas, karmi watchdoga
// i panic nie ma prawa wystapic. Gdy dane SA (a > 0 / r > 0) nie robimy NIC — sciezka
// szybka jest nietknieta, wiec przepustowosc (w tym pobieranie OTA) nie cierpi.
//
// DLACZEGO TU, A NIE W BIBLIOTECE: poprawka nalezy do rdzenia (ssl_client.cpp albo
// writeToStreamDataBlock()), ale forkowanie pakietu rdzenia oznaczaloby wlasny
// board manager i reczne scalanie przy kazdej aktualizacji. Podklasa kosztuje jedna
// vtable we flashu, zero bajtow w .bss (wszystkie nasze instancje sa lokalne, na
// stosie) i dziala przez zwykly polimorfizm — patrz nizej.
//
// DLACZEGO `override` W OGOLE PRZECHODZI: NetworkClientSecure.h nie pisze przy tych
// metodach slowa `virtual`, ale to bez znaczenia — wirtualnosc jest dziedziczona.
// cores/esp32/Client.h:32 i :34 deklaruja:
//         virtual int available() = 0;
//         virtual int read(uint8_t *buf, size_t size) = 0;
// a lancuch dziedziczenia to Client -> ESPLwIPClient -> NetworkClient ->
// NetworkClientSecure. Sygnatury ponizej sa z nimi zgodne co do znaku (bez const,
// uint8_t*, size_t), wiec `override` faktycznie NADPISUJE, a nie tworzy nowej metody.
// Kompilator to potwierdza: przy zlej sygnaturze `override` jest bledem kompilacji,
// a nie cichym utworzeniem przeciazenia — i o to nam tu chodzi.
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
class YieldingSecureClient : public WiFiClientSecure {
 public:
  int available() override {
    const int a = WiFiClientSecure::available();
    if (a <= 0) {
      vTaskDelay(1);
    }
    return a;
  }

  int read(uint8_t* buf, size_t size) override {
    const int r = WiFiClientSecure::read(buf, size);
    if (r <= 0) {
      vTaskDelay(1);
    }
    return r;
  }

  // Zadeklarowanie read(uint8_t*, size_t) UKRYWA w tej klasie wszystkie pozostale
  // przeciazenia `read` z bazy — w tym bezargumentowe read(). Bez tej linii
  // `client.read()` wolane wprost na YieldingSecureClient nie skompilowaloby sie.
  // Nasza deklaracja powyzej ma pierwszenstwo przed ta sciagnieta przez `using`,
  // wiec nie ma tu dwuznacznosci.
  using WiFiClientSecure::read;
};
