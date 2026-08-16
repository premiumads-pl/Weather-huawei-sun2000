#include "Ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <cctype>     // isxdigit() — walidacja pola "sha256"
#include <cstring>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>   // ta sama biblioteka, co w Viessmann.cpp (PKCE)
#include <strings.h>  // strcasecmp() — porownanie hex bez wzgledu na wielkosc liter

#include "Config.h"
#include "Log.h"
#include "OtaGuard.h"
#include "SecureClient.h"
#include "Version.h"

namespace {
OtaStatus gStatus;
volatile bool gRequested = false;
volatile bool gBusy = false;
volatile bool gUiFreed = false;

void setMsg(const char* m) {
  strncpy(gStatus.message, m, sizeof(gStatus.message) - 1);
  gStatus.message[sizeof(gStatus.message) - 1] = '\0';
}

// =============================================================================
// Sha256Stream — przekladka liczaca SHA-256 pobieranego firmware'u W LOCIE
// =============================================================================
//
// PO CO W OGOLE, A NIE PO FAKCIE Z PARTYCJI: arduinowy UpdateClass NIE zapisuje
// pierwszych 16 bajtow obrazu w trakcie pobierania. _writeBuffer()
// (libraries/Update/src/Updater.cpp, pakiet rdzenia esp32 3.3.10) odklada je do
// _skipBuffer i dopisuje na partycje dopiero w Update.end() -> _verifyEnd().
// Odczyt partycji PRZED end() pokazalby wiec dziure w naglowku, a PO end() jest
// juz za pozno: to wlasnie end(true) aktywuje obraz i to on wywalil sie bledem 9.
// Suma musi wiec powstac z tego, co plynie po drucie, i to zanim end() ruszy.
//
// DLACZEGO CIENKA PODKLASA, A NIE FORK BIBLIOTEKI: dokladnie ta sama decyzja i te
// same powody, co przy YieldingSecureClient (SecureClient.h) — wlasny board manager
// i reczne scalanie przy kazdej aktualizacji rdzenia kosztowalyby wiecej niz jedna
// vtable we flashu. Instancja jest lokalna, na stosie netTask; zero bajtow w .bss.
//
// KOSZT RAM — ZMIERZONY KOMPILATOREM, NIE OSZACOWANY (chwilowe
// `template<int N> struct POMIAR; POMIAR<sizeof(X)> x;` wypisuje rozmiar w tresci
// bledu; xtensa-esp32s3-g++ z pakietu rdzenia 3.3.10, ten sam fqbn co w release):
//   sizeof(mbedtls_sha256_context) = 116 B  (wariant sprzetowy z sha256_alt.h dla
//       ESP32-S3: total[2] + state[8] + buffer[64] + first_block + mode + sha_state;
//       MBEDTLS_SHA256_ALT jest wlaczone przez CONFIG_MBEDTLS_HARDWARE_SHA)
//   sizeof(Sha256Stream)           = 136 B  (te 116 B kontekstu + 20 B: vptr,
//       Print::write_error, Stream::_timeout, Stream::_startMillis i Stream& src_)
// Do tego w downloadAndFlash() lezy `char got[65]` na wynik hex.
// RAZEM ~201 B NA STOSIE netTask (16 kB, zapas mierzony na zywo przez
// uxTaskGetStackHighWaterMark i wystawiany w /api/diag jako stack_net) i
// DOKLADNIE 0 B w .bss — nic z tego nie jest globalne ani na stercie.
//
// KTORE METODY Stream WOLA UpdateClass::writeStream — SPRAWDZONE W ZRODLE, nie
// zgadniete (Updater.cpp 3.3.10, funkcja zaczyna sie w linii 863):
//   * linia 875:  _verifyHeader(data.peek())   — JEDEN raz, przed petla;
//   * linia 903:  data.readBytes(_buffer + _bufferLen, bytesToRead)  — w petli,
//                 _buffer jest typu uint8_t*, wiec celuje w przeciazenie
//                 `virtual size_t readBytes(uint8_t*, size_t)` (Stream.h:109),
//                 ktore w rdzeniu przekazuje dalej do wersji char* (Stream.h:108).
// INNYCH metod strumienia writeStream() NIE uzywa — w szczegolnosci NIE wola
// available() ani read(). Mimo to read() jest ponizej nadpisany i tez haszuje:
// gdyby ktos kiedys podmienil implementacje rdzenia albo wolal na tym obiekcie
// readString()/find() (one ida przez Stream::timedRead() -> read()), bajty NIE
// moga wyjsc bokiem — kontrola sumy, ktora da sie ominac, jest gorsza od jej braku,
// bo klamie o tym, ze cokolwiek sprawdzila.
//
// PEEK CELOWO NIE HASZUJE: peek() nie KONSUMUJE bajtu — ten sam bajt przyjdzie za
// chwile przez readBytes() i wtedy zostanie policzony. Haszowanie w peek() dodaloby
// pierwszy bajt obrazu (0xE9) drugi raz i suma NIGDY by sie nie zgodzila.
//
// available() nie przepuszcza danych, wiec tylko przekazuje pytanie dalej.
class Sha256Stream : public Stream {
 public:
  explicit Sha256Stream(Stream& src) : src_(src) {
    mbedtls_sha256_init(&ctx_);
    mbedtls_sha256_starts(&ctx_, 0);   // 0 = SHA-256 (1 byloby SHA-224)
  }
  ~Sha256Stream() { mbedtls_sha256_free(&ctx_); }

  Sha256Stream(const Sha256Stream&) = delete;
  Sha256Stream& operator=(const Sha256Stream&) = delete;

  int available() override { return src_.available(); }

  int peek() override { return src_.peek(); }   // NIE haszuje — patrz wyzej

  int read() override {
    const int c = src_.read();
    if (c >= 0) {
      const uint8_t b = static_cast<uint8_t>(c);
      mbedtls_sha256_update(&ctx_, &b, 1);
    }
    return c;
  }

  size_t readBytes(char* buf, size_t len) override {
    const size_t n = src_.readBytes(buf, len);
    if (n > 0) {
      mbedtls_sha256_update(&ctx_, reinterpret_cast<const uint8_t*>(buf), n);
    }
    return n;
  }

  // Nadpisane JAWNIE, mimo ze rdzen ma tu identyczne przekierowanie (Stream.h:109):
  // deklaracja readBytes(char*, size_t) powyzej UKRYWA w tej klasie pozostale
  // przeciazenia `readBytes` z bazy, wiec bez tej metody wolanie na konkretnym typie
  // (a nie przez Stream&) przestaloby sie kompilowac. Jedno miejsce haszowania.
  size_t readBytes(uint8_t* buf, size_t len) override {
    return readBytes(reinterpret_cast<char*>(buf), len);
  }

  // Print::write(uint8_t) jest czysto wirtualne (Print.h:57), wiec MUSI byc tu
  // zaimplementowane, zeby klasa nie byla abstrakcyjna. Do gniazda nic nie piszemy —
  // ten strumien sluzy wylacznie do czytania i celowo nie udaje, ze umie wiecej.
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }

  // Zamyka sume i wypisuje ja jako 64 znaki hex MALYMI literami + NUL.
  void finishHex(char out[65]) {
    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&ctx_, digest);
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
      out[i * 2] = kHex[digest[i] >> 4];
      out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    out[64] = '\0';
  }

 private:
  Stream& src_;
  mbedtls_sha256_context ctx_;
};

// 64 znaki, same cyfry hex. Wielkosc liter bez znaczenia (porownanie i tak idzie
// przez strcasecmp) — sprawdzamy TYLKO, czy to w ogole jest suma SHA-256.
bool isSha256Hex(const char* s) {
  if (s == nullptr) {
    return false;
  }
  int n = 0;
  for (; s[n] != '\0'; ++n) {
    if (n >= 64 || isxdigit(static_cast<unsigned char>(s[n])) == 0) {
      return false;
    }
  }
  return n == 64;
}
}  // namespace

OtaStatus& otaStatus() {
  return gStatus;
}

void requestOtaCheck() {
  gRequested = true;
}

bool takeOtaRequest() {
  if (!gRequested) {
    return false;
  }
  gRequested = false;
  return true;
}

void otaUiBufferFreed() {
  gUiFreed = true;
}

bool Ota::fetchRemoteVersion(OtaManifest& man) {
  YieldingSecureClient client;
  client.setInsecure();
  client.setHandshakeTimeout(15);
  // v157: sprawdzenie wersji to zwykly, maleńki JSON — dostaje ZWYKLY termin (30 s),
  // a NIE luzniejszy termin OTA. Kierunek ryzyka jest tu odwrotny niz przy pobieraniu:
  // sprawdzenie, ktore wisi, NIE aktualizuje niczego, wiec szybkie poddanie sie
  // i ponowienie za kwadrans (cfg::OTA_CHECK_MS) POMAGA zdalnej naprawie, a nie szkodzi.
  client.armIdleGuard(netguard::kIdleMs, "OTA wersja");

  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("pogoda-esp32");

  if (!http.begin(client, cfg::OTA_VERSION_URL)) {
    Serial.println("OTA: http.begin() nie powiodlo sie");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OTA: sprawdzenie wersji -> HTTP %d (%s), heap=%u B\n", code,
                  HTTPClient::errorToString(code).c_str(),
                  static_cast<unsigned>(ESP.getFreeHeap()));
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("OTA: zly JSON wersji (%s)\n", err.c_str());
    return false;
  }
  man = OtaManifest{};   // nie zostawiaj resztek po POPRZEDNIM sprawdzeniu
  man.version = doc["version"] | 0;
  if (man.version <= 0) {
    Serial.println("OTA: brak pola 'version'");
    return false;
  }

  // --- rozmiar zapowiedziany przez wydanie (tools/release.sh zawsze go wpisuje) ---
  // 0 = pola nie bylo albo mialo bezsensowna wartosc => kontrola rozmiaru odpada,
  // ale aktualizacji NIE blokujemy: starsze wydania takiego pola nie mialy.
  man.size = doc["size"] | 0;
  if (man.size < 0) {
    man.size = 0;
  }

  // --- suma kontrolna obrazu ---
  const char* sha = doc["sha256"] | "";
  if (sha[0] == '\0') {
    // Starsze wydanie (albo recznie zlozony version.json). NIE blokujemy i NIE
    // logujemy tutaj: sprawdzenie wersji leci co 15 minut, a bufor /api/log ma
    // 3072 B (~6 minut) — taki wpis wypchnalby z niego wszystko inne. Adnotacja
    // "nikt nie sprawdzal sumy" jest w downloadAndFlash(), czyli w jedynym miejscu,
    // gdzie ten brak ma jakiekolwiek znaczenie.
    man.sha256[0] = '\0';
  } else if (!isSha256Hex(sha)) {
    // Pole JEST, ale nie jest suma SHA-256. To nie jest "stare wydanie", tylko
    // uszkodzony albo podmieniony manifest — czyli dokladnie ta klasa zdarzenia,
    // przed ktora ma bronic ta kontrola. Odrzucamy CALE sprawdzenie; udawanie, ze
    // pola nie bylo, zamienialoby weryfikacje w atrape.
    LOG("OTA: pole 'sha256' w version.json nie jest 64-znakowym hex — odrzucam manifest\n");
    return false;
  } else {
    memcpy(man.sha256, sha, 64);
    man.sha256[64] = '\0';
  }
  return true;
}

bool Ota::downloadAndFlash(const OtaManifest& man) {
  // Poczekaj, aż UI odda bufor ekranu (150 kB) — bez tego heapu starcza tylko
  // na TLS, a pobieranie 1,3 MB się wykłada.
  gStatus.state = OtaState::DOWNLOADING;
  setMsg("Zwalniam pamiec...");
  // ZNACZNIK DOTYCZY TEGO JEDNEGO OCZEKIWANIA, NIE CALEJ SESJI — i dlatego zeruje
  // sie go TUTAJ, tuz przed petla, a nie gdziekolwiek indziej.
  // otaUiBufferFreed() (wolane z loop() w pogoda-gdynia.ino) ustawia gUiFreed na
  // true i nic go nigdy nie cofalo. Po pierwszym OTA w danym uruchomieniu UI wraca
  // do IDLE i WeatherUi::render() sam odtwarza bufor 150 kB (restoreBuffer()), wiec
  // przy DRUGIEJ probie w tym samym uruchomieniu warunek `!gUiFreed` byl od razu
  // falszywy: wychodzilismy z czekania natychmiast i zaczynali ciagnac 1,8 MB przez
  // TLS ze sterta mniejsza o te 150 kB. Zerowanie sprawia, ze kazde oczekiwanie pyta
  // o BIEZACY stan bufora, a nie o to, czy kiedykolwiek go oddano.
  gUiFreed = false;
  for (int i = 0; i < 60 && !gUiFreed; ++i) {
    delay(50);
  }
  Serial.printf("OTA: heap przed pobieraniem = %u B\n",
                static_cast<unsigned>(ESP.getFreeHeap()));

  YieldingSecureClient client;
  client.setInsecure();
  // v157: termin LUZNIEJSZY (60 s bezczynnosci) i BEZCZYNNOSCIOWY, nie calkowity —
  // 1,8 MB przez TLS przy slabym sygnale legalnie ciagnie sie minutami, a urzadzenie
  // nie ma USB, wiec ubicie POSTEPUJACEGO pobrania byloby awaria nie do naprawienia.
  // UCZCIWA UWAGA: na sam transfer ciala ten termin i tak nie ma wplywu, bo
  // Update.writeStream() czyta ze strumienia z pominieciem HTTPClient::connected();
  // tam pilnuje wlasny bezpiecznik rdzenia (Updater.cpp:902-912, ~30 s). Tu
  // zabezpieczamy faze zadania i naglowkow. Pelne wyjasnienie: SecureClient.h.
  client.armIdleGuard(netguard::kOtaIdleMs, "OTA firmware");

  HTTPClient http;
  http.setTimeout(20000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, cfg::OTA_FIRMWARE_URL)) {
    setMsg("Nie mogę pobrać pliku");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char b[48];
    snprintf(b, sizeof(b), "HTTP %d przy pobieraniu", code);
    setMsg(b);
    http.end();
    return false;
  }

  const int total = http.getSize();
  if (total <= 0) {
    setMsg("Nieznany rozmiar pliku");
    http.end();
    return false;
  }

  // ---- KONTROLA (a): ROZMIAR, ZANIM COKOLWIEK TRAFI NA PARTYCJE ----
  // Content-Length musi sie zgadzac z tym, co zapowiedzial version.json. To lapie
  // najczestszy i najgrozniejszy przypadek — CDN oddajacy PLIK Z INNEGO (starszego)
  // wydania albo tresc przycieta — natychmiast, zanim Update.begin() skasuje druga
  // partycje. Kasowanie partycji jest nieodwracalne: po nim urzadzenie nie ma juz
  // dokad sie wycofac, wiec kazda kontrola, ktora da sie zrobic wczesniej, MA byc
  // zrobiona wczesniej.
  //
  // man.size == 0 znaczy "wydanie nie zapowiedzialo rozmiaru" (starszy version.json)
  // — wtedy nie ma z czym porownywac i lecimy dalej, tak jak przed ta zmiana.
  //
  // Komunikat podaje OBIE liczby, bo sama informacja "zly rozmiar" nie mowi, po
  // ktorej stronie szukac: rozbieznosc o kilkanascie bajtow to inny problem niz plik
  // krotszy o polowe. Najdluzszy wariant to 2 x 7 cyfr (~1,8 MB), czyli
  // "Plik 1835152 B, zapowiedziano 1835008 B" = 39 znakow — miesci sie w buforze 48 B.
  if (man.size > 0 && total != man.size) {
    char b[48];
    snprintf(b, sizeof(b), "Plik %d B, zapowiedziano %d B", total, man.size);
    setMsg(b);
    LOG("OTA: %s — CDN oddal co innego niz version.json, przerywam\n", b);
    http.end();
    return false;
  }

  // Diagnostyka + sprzątanie po ewentualnej przerwanej próbie.
  const esp_partition_t* run = esp_ota_get_running_partition();
  const esp_partition_t* nxt = esp_ota_get_next_update_partition(nullptr);
  Serial.printf("OTA: plik=%d B, wolny heap=%u B\n", total,
                static_cast<unsigned>(ESP.getFreeHeap()));
  if (run) Serial.printf("OTA: dziala z '%s' (%u B)\n", run->label,
                         static_cast<unsigned>(run->size));
  if (nxt) Serial.printf("OTA: cel '%s' (%u B)\n", nxt->label,
                         static_cast<unsigned>(nxt->size));

  if (Update.isRunning()) {
    Serial.println("OTA: poprzednia proba wisiala — czyszcze");
    Update.abort();
  }

  if (!Update.begin(total)) {
    char b[48];
    snprintf(b, sizeof(b), "%s", Update.errorString());
    setMsg(b);
    Serial.printf("OTA: Update.begin() blad %d: %s\n", Update.getError(),
                  Update.errorString());
    Update.abort();
    http.end();
    return false;
  }

  Serial.printf("OTA: pobieram %d bajtow...\n", total);
  gStatus.state = OtaState::DOWNLOADING;
  gStatus.progress = 0;
  setMsg("Pobieram nową wersję");

  // UWAGA: nie odpytujemy available() — na TLS potrafi zwracać 0 mimo danych
  // w drodze i robi się fałszywy timeout. writeStream() czyta blokująco.
  Update.onProgress([](size_t done, size_t all) {
    gStatus.progress = all ? static_cast<int>((done * 100) / all) : 0;
  });

  // ---- KONTROLA (b), czesc 1: SHA-256 LICZONA W LOCIE ----
  // Zamiast http.getStream() podajemy Update'owi przekladke, ktora przepuszcza bajty
  // bez zmian i po drodze karmi nimi mbedtls_sha256. Uzasadnienie ("dlaczego nie da
  // sie tego zrobic po fakcie z partycji" i "ktore metody Stream wola writeStream")
  // stoi przy definicji Sha256Stream na gorze pliku.
  //
  // WATCHDOG: haszowanie nie dodaje ZADNEJ nowej petli. Odbywa sie wylacznie w
  // readBytes(), czyli dokladnie tam, gdzie writeStream() i tak juz stoi, i obejmuje
  // najwyzej jedna porcje SPI_FLASH_SEC_SIZE = 4096 B na wywolanie (Updater.cpp:891).
  // Sprzetowy akcelerator SHA w ESP32-S3 przelicza taka porcje w ulamku milisekundy,
  // a oddawanie procesora zostaje takie samo jak dotad (vTaskDelay(1) w
  // YieldingSecureClient, gdy gniazdo nie ma danych).
  Sha256Stream hashed(http.getStream());
  const size_t written = Update.writeStream(hashed);

  if (written != static_cast<size_t>(total)) {
    char b[48];
    snprintf(b, sizeof(b), "Pobrano %u z %d B", static_cast<unsigned>(written), total);
    setMsg(b);
    Serial.printf("OTA: %s (%s)\n", b, Update.errorString());
    Update.abort();
    http.end();
    return false;
  }

  http.end();

  // ---- KONTROLA (b), czesc 2: POROWNANIE SUMY, KONIECZNIE PRZED Update.end(true) ----
  // end(true) dopisuje brakujace 16 bajtow naglowka, weryfikuje obraz i AKTYWUJE go
  // (esp_ota_set_boot_partition). Po nim jest juz za pozno na "to nie ten plik", a
  // przed nim partycji i tak nie da sie sensownie odczytac (patrz Sha256Stream).
  char got[65];
  hashed.finishHex(got);
  if (man.sha256[0] == '\0') {
    // Wydanie nie zapowiedzialo sumy. Aktualizacji NIE blokujemy (starsze wydania jej
    // nie mialy), ale musi zostac slad — inaczej za pol roku nie da sie odroznic
    // "sprawdzone i zgodne" od "nikt nie sprawdzal". Wlasna suma idzie do logu, zeby
    // dalo sie ja recznie porownac z plikiem na GitHubie.
    // OSOBNE wywolania LOG(), a nie jedno dlugie: logPrintf() sklada linie w buforze
    // char[192] (Log.cpp), a stempel czasu zjada z niego 11 znakow. Suma SHA-256 ma
    // 64 znaki, wiec zdanie razem z nia nie miesci sie z zapasem i vsnprintf
    // przycialby wlasnie te czesc, dla ktorej ten wpis powstaje.
    LOG("OTA: version.json wersji %d nie ma pola 'sha256' — NIKT nie sprawdzil sumy\n",
        man.version);
    LOG("OTA: policzona lokalnie: %s\n", got);
  } else if (strcasecmp(got, man.sha256) != 0) {
    // Porownanie bez wzgledu na wielkosc liter: hex jest hexem, a rozne narzedzia
    // (sha256sum, shasum, esptool) wypisuja go raz malymi, raz wielkimi literami.
    //
    // KOMUNIKAT MOWI WPROST, CO SIE STALO. Poprzednio taki przypadek konczyl sie
    // dopiero bledem 9 z Update.end() — czyli napisem, ktory sugerowal awarie
    // partycji, a nie to, ze przyszedl nie ten plik. Suma jest za dluga na bufor
    // 48 B, wiec pelne obie wartosci ida do /api/log, a na ekran idzie zdanie,
    // ktore mowi, gdzie szukac.
    //
    // TRZY OSOBNE LOG-i, KAZDY Z JEDNA SUMA: logPrintf() ma bufor char[192] na cala
    // linie razem ze stemplem czasu (Log.cpp). Jeden wpis z dwiema sumami po 64 znaki
    // mial ~231 znakow, czyli vsnprintf uciolby koncowke drugiej sumy — akurat tej,
    // ktora tu jest jedynym dowodem. Trzy krotkie linie mieszcza sie z zapasem.
    setMsg("Plik uszkodzony (zla suma SHA-256)");
    LOG("OTA: SUMA SIE NIE ZGADZA — plik przyszedl uszkodzony, NIE aktywuje go\n");
    LOG("OTA: zapowiedziano: %s\n", man.sha256);
    LOG("OTA: pobrano:       %s\n", got);
    Update.abort();
    return false;
  } else {
    LOG("OTA: suma SHA-256 zgodna z version.json (%d B)\n", total);
  }

  if (!Update.end(true)) {
    // Sam numer nie wystarczy: w UpdateClass::_verifyEnd() ten sam kod 9
    // (UPDATE_ERROR_ACTIVATE) ustawiaja DWIE rozne przyczyny — partycja uznana za
    // nierozruchowa oraz odmowa esp_ota_set_boot_partition(). errorString() nie
    // rozstrzyga miedzy nimi, ale przynajmniej nazywa rzecz po imieniu zamiast
    // kazac szukac numeru w naglowku biblioteki.
    // Najdluzszy mozliwy napis to "Could Not Activate The Firmware" (31 znakow,
    // Updater.cpp:41), czyli caly komunikat ma max 45 znakow — bufor 48 B zostaje
    // bez zmian, nie trzeba go powiekszac.
    char b[48];
    snprintf(b, sizeof(b), "Update.end %d: %s", Update.getError(), Update.errorString());
    setMsg(b);
    LOG("OTA: %s\n", b);
    return false;
  }

  gStatus.state = OtaState::DONE;
  gStatus.progress = 100;
  setMsg("Gotowe — restart");
  Serial.println("OTA: zapisano, restart");
  return true;
}

bool Ota::checkAndUpdate(bool manual) {
  if (gBusy) {
    Serial.println("OTA: sprawdzanie juz trwa — pomijam");
    return false;
  }

  // KLUCZOWE dla rollbacku: dopóki BIEŻĄCA wersja nie jest potwierdzona, nie wolno
  // nic wgrywać. Nowy obraz poszedłby na drugą partycję — czyli nadpisałby JEDYNĄ
  // sprawną wersję, na którą moglibyśmy się cofnąć.
  //
  // ESP-IDF pilnuje tego samo w esp_ota_begin() (ESP_ERR_OTA_ROLLBACK_INVALID_STATE),
  // ale arduinowy Update w ogóle nie używa esp_ota_begin() — zapisuje partycję sam
  // i woła tylko esp_ota_set_boot_partition(), które takiej blokady nie ma.
  // Dlatego musimy jej pilnować tutaj.
  if (otaTrialActive()) {
    LOG("OTA: trwa okres próbny bieżącej wersji — nie ruszam aktualizacji\n");
    return false;
  }

  gBusy = true;

  struct Guard {
    ~Guard() { gBusy = false; }
  } guard;

  gStatus.state = OtaState::CHECKING;
  setMsg("Sprawdzam aktualizacje");

  // Manifest zyje na stosie netTask przez cale wywolanie: to samo, co przeczytalismy
  // z version.json, musi trafic do downloadAndFlash() — inaczej rozmiar i suma
  // opisywalyby INNE sprawdzenie niz to, ktore wlasnie pobiera plik.
  OtaManifest man;
  int remote = 0;
  // Stempel po KAZDEJ zakonczonej probie — i udanej, i nieudanej (diag().otaOkAt nizej
  // stawiamy tylko po udanej). Panel WWW rozstrzyga z ROZNICY tych dwoch, czy
  // sprawdzenie jeszcze trwa, czy juz sie skonczylo i czym. Logiki OTA to nie dotyka.
  bool fetched = fetchRemoteVersion(man);
  diag().otaCheckedAt = millis();
  if (!fetched) {
    // SIEĆ BEZPIECZEŃSTWA: jeśli sterty jest tak mało, że nie da się zestawić TLS,
    // urządzenie nie mogłoby się już NIGDY zaktualizować po sieci (dokładnie to
    // zabiło v14). Oddajemy więc 150 kB bufora ekranu i próbujemy jeszcze raz.
    const uint32_t heap = ESP.getFreeHeap();
    if (heap < 60000) {
      LOG("OTA: malo RAM (%lu B) — zwalniam bufor i probuje ponownie\n",
          static_cast<unsigned long>(heap));
      // DOWNLOADING nie znaczy tu "pobieram firmware" — to jedyny sposob, zeby kazac UI
      // oddac bufor: loop() (pogoda-gdynia.ino) na ten stan wola ui.releaseBuffer().
      // ODDANIE JEST BEZZWROTNE TYLKO POZORNIE i nie trzeba tu nic sprzatac — gdy stan
      // wroci do IDLE, loop() przestaje wchodzic w ta galaz, a WeatherUi::render() sam
      // wola restoreBuffer() przy najblizszej klatce (patrz `if (freed_ && ...)` na
      // poczatku render()). Zaden powrot z tej funkcji nie musi wiec odtwarzac bufora.
      gStatus.state = OtaState::DOWNLOADING;
      setMsg("Sprawdzam aktualizacje...");
      // Zerowanie TUZ PRZED petla — znacznik dotyczy TEGO oczekiwania, nie calej
      // sesji. Pelne uzasadnienie przy blizniaczej petli w downloadAndFlash().
      gUiFreed = false;
      for (int i = 0; i < 60 && !gUiFreed; ++i) {
        delay(50);
      }
      fetched = fetchRemoteVersion(man);
      diag().otaCheckedAt = millis();   // druga proba tez jest proba — patrz wyzej
      if (!fetched) {
        gStatus.state = OtaState::FAILED;
        LOG("OTA: nie udalo sie mimo zwolnienia bufora — restart\n");
        delay(3000);
        ESP.restart();
        return false;
      }
      // Udalo sie za drugim razem. NIE ma tu wlasnej galezi "brak nowszej wersji" —
      // celowo lecimy do wspolnego bloku nizej. Stal tu kiedys wczesny return, ktory
      // robil DOKLADNIE to samo (IDLE + pusty komunikat), ale omijal stemple
      // diag().otaRemote/otaOkAt — czyli po UDANYM sprawdzeniu diagnostyka wygladala
      // jak po nieudanym i panel WWW oglaszal "nie udalo sie odczytac wersji".
    } else {
      gStatus.state = OtaState::IDLE;
      setMsg("");
      return false;
    }
  }

  remote = man.version;
  lastRemote_ = remote;
  gStatus.remoteVersion = remote;
  diag().otaRemote = remote;
  diag().otaOkAt = millis();

  if (remote <= FW_VERSION) {
    gStatus.state = OtaState::IDLE;
    setMsg("");
    Serial.printf("OTA: aktualna wersja %d (zdalna %d)\n", FW_VERSION, remote);
    return false;
  }

  // Ta wersja już raz (a właściwie dwa razy) nie przeżyła okresu próbnego. Bez tej
  // blokady wpadlibyśmy w pętlę: pobierz cegłę -> rollback -> pobierz tę samą cegłę.
  // Ręczne sprawdzenie z panelu WWW blokadę omija — to furtka na wypadek, gdyby
  // wersja została odrzucona niesłusznie (np. przez awarię routera w złym momencie).
  if (!manual && otaVersionRejected(remote)) {
    gStatus.state = OtaState::IDLE;
    setMsg("");
    LOG("OTA: wersja %d była już odrzucona po rollbacku — pomijam "
        "(wymuś ręcznie w panelu, jeśli to pomyłka)\n",
        remote);
    return false;
  }

  Serial.printf("OTA: nowa wersja %d (mam %d) — aktualizuje\n", remote, FW_VERSION);

  if (!downloadAndFlash(man)) {
    gStatus.state = OtaState::FAILED;
    Serial.printf("OTA BLAD: %s — restart\n", gStatus.message);
    // Bufor ekranu jest już zwolniony; najczystszy powrót do normalnej pracy
    // to restart (spróbujemy ponownie przy kolejnym sprawdzeniu).
    delay(4000);
    ESP.restart();
    return false;
  }

  // Obraz siedzi już na drugiej partycji, a esp_ota_set_boot_partition() (w środku
  // Update.end()) ustawił jej stan na ESP_OTA_IMG_NEW. Zapisz w NVS, KTÓRA wersja
  // zaraz wejdzie w okres próbny — bo jeśli padnie panikiem od razu po starcie,
  // cofnie ją sam bootloader i nie zdąży o sobie nic powiedzieć. Bez tego znacznika
  // pobralibyśmy dokładnie tę samą cegłę w kółko.
  otaGuardArmTrial(remote);

  delay(1200);
  ESP.restart();
  return true;
}
