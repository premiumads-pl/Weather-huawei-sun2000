#include "PvClient.h"

#include "Config.h"
#include "Log.h"
#include "Settings.h"

#include <Arduino.h>
#include <WiFi.h>

namespace {
constexpr uint8_t kUnit = 1;
constexpr uint32_t kTimeoutMs = 2500;

WiFiClient gSock;
uint16_t gTid = 0xA100;
uint16_t gExpectTid = 0;

bool recvExact(uint8_t* buf, size_t n, uint32_t deadline) {
  size_t got = 0;
  while (got < n) {
    if (static_cast<int32_t>(deadline - millis()) <= 0) {
      return false;
    }
    if (!gSock.connected() && gSock.available() == 0) {
      return false;
    }
    const int r = gSock.read(buf + got, n - got);
    if (r <= 0) {
      delay(4);
      continue;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

}  // namespace

// Odczyt z weryfikacją Transaction ID. Falownik Huawei bywa wolny — spóźniona
// odpowiedź na poprzednie zapytanie zostaje odrzucona zamiast przesunąć wszystkie
// kolejne rejestry (klasyczny błąd desynchronizacji Modbus TCP).
bool PvClient::readRegs(uint16_t addr, uint16_t count, uint16_t* out) {
  const uint16_t tid = ++gTid;
  gExpectTid = tid;

  uint8_t req[12];
  req[0] = static_cast<uint8_t>(tid >> 8);
  req[1] = static_cast<uint8_t>(tid & 0xFF);
  req[2] = 0;
  req[3] = 0;
  req[4] = 0;
  req[5] = 6;
  req[6] = kUnit;
  req[7] = 0x03;
  req[8] = static_cast<uint8_t>(addr >> 8);
  req[9] = static_cast<uint8_t>(addr & 0xFF);
  req[10] = static_cast<uint8_t>(count >> 8);
  req[11] = static_cast<uint8_t>(count & 0xFF);

  if (gSock.write(req, sizeof(req)) != sizeof(req)) {
    return false;
  }

  const uint32_t deadline = millis() + kTimeoutMs;

  while (true) {
    uint8_t hdr[8];
    if (!recvExact(hdr, sizeof(hdr), deadline)) {
      return false;
    }
    const uint16_t rxTid = (static_cast<uint16_t>(hdr[0]) << 8) | hdr[1];
    const uint16_t len = (static_cast<uint16_t>(hdr[4]) << 8) | hdr[5];
    const uint8_t fc = hdr[7];
    if (len < 2) {
      return false;
    }

    uint8_t body[256];
    const size_t bodyLen = len - 2;  // po unit id i function code
    if (bodyLen > sizeof(body) || !recvExact(body, bodyLen, deadline)) {
      return false;
    }

    if (rxTid != tid) {
      continue;  // spóźniona odpowiedź — zignoruj i czekaj na właściwą
    }
    if (fc & 0x80) {
      return false;  // wyjątek Modbus
    }
    if (bodyLen < 1) {
      return false;
    }
    const uint8_t bytes = body[0];
    if (bytes < count * 2 || bodyLen < static_cast<size_t>(1 + bytes)) {
      return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
      out[i] = (static_cast<uint16_t>(body[1 + i * 2]) << 8) | body[2 + i * 2];
    }
    return true;
  }
}

bool PvClient::readRegsRetry(uint16_t addr, uint16_t count, uint16_t* out, int tries) {
  for (int i = 0; i < tries; ++i) {
    if (readRegs(addr, count, out)) {
      return true;
    }
    delay(150);
  }
  return false;
}

bool PvClient::readS32(uint16_t addr, int32_t& out) {
  uint16_t regs[2]{};
  if (!readRegsRetry(addr, 2, regs)) {
    return false;
  }
  const uint32_t u = (static_cast<uint32_t>(regs[0]) << 16) | regs[1];
  out = static_cast<int32_t>(u);
  return true;
}

bool PvClient::readU32(uint16_t addr, uint32_t& out) {
  uint16_t regs[2]{};
  if (!readRegsRetry(addr, 2, regs)) {
    return false;
  }
  out = (static_cast<uint32_t>(regs[0]) << 16) | regs[1];
  return true;
}

bool PvClient::readS16(uint16_t addr, int gain, float& out) {
  uint16_t reg = 0;
  if (!readRegsRetry(addr, 1, &reg)) {
    return false;
  }
  const int16_t v = static_cast<int16_t>(reg);
  out = static_cast<float>(v) / static_cast<float>(gain);
  return true;
}

bool PvClient::readU16(uint16_t addr, uint16_t& out) {
  return readRegsRetry(addr, 1, &out);
}

bool PvClient::warmUp() {
  uint16_t regs[2]{};
  for (int i = 0; i < 4; ++i) {
    if (readRegs(32080, 2, regs)) {
      return true;
    }
    delay(250);
  }
  return false;
}

// Połączenie TCP trzymamy otwarte między odczytami. Huawei po każdym nowym
// handshake potrzebuje długiej rozgrzewki (nawet ~20 s), więc łączenie się od
// nowa co 30 s było głównym powodem, dlaczego dane pojawiały się z opóźnieniem.
bool PvClient::ensureConnected() {
  if (gSock.connected()) {
    // wyczyść ewentualne spóźnione ramki z poprzedniego cyklu
    while (gSock.available()) {
      gSock.read();
    }
    return true;
  }

  gSock.stop();
  if (!gSock.connect(settings().modbusHost, settings().modbusPort, 3000)) {
    return false;
  }
  gSock.setNoDelay(true);
  gSock.setTimeout(kTimeoutMs / 1000);
  delay(400);

  if (!warmUp()) {
    gSock.stop();
    return false;
  }
  Serial.println("Modbus: polaczono z falownikiem");
  return true;
}

bool PvClient::fetch(PvModel& out, bool night) {
  out.errorMsg[0] = '\0';
  out.asleep = false;

  if (WiFi.status() != WL_CONNECTED) {
    out.online = false;
    strncpy(out.errorMsg, "Brak WiFi", sizeof(out.errorMsg) - 1);
    return false;
  }
  if (!settings().hasInverter()) {
    out.online = false;
    strncpy(out.errorMsg, "Ustaw IP falownika w panelu", sizeof(out.errorMsg) - 1);
    return false;
  }

  // UWAGA: nocą też próbujemy się połączyć — falownik potrafi odpowiadać jeszcze
  // po zachodzie i budzi się z opóźnieniem. Zmienia się TYLKO opis braku odpowiedzi
  // (neutralny zamiast czerwonego) i częstotliwość odpytywania (o tym decyduje netTask).
  if (!ensureConnected()) {
    out.online = false;
    out.asleep = night;
    strncpy(out.errorMsg, night ? "Falownik uśpiony" : "Falownik nie odpowiada",
            sizeof(out.errorMsg) - 1);
    return false;
  }

  PvSnapshot snap{};
  int32_t s32 = 0;
  uint32_t u32 = 0;
  float f = 0.f;
  uint16_t u16 = 0;

  // DWA liczniki, nie jeden — i to jest celowe.
  //
  // Prog "fails >= 3" rozstrzyga jedna rzecz: czy sesja Modbus jest martwa na tyle,
  // ze oplaca sie ja zerwac i zaplacic za nowy handshake (~20 s rozgrzewki Huawei).
  // Skalibrowany zostal jako 3 z 5, czyli 60% odczytow. Dorzucenie drugiej piatki do
  // tego samego licznika zmienia go po cichu na 3 z 10, czyli na 30% — dwa razy
  // ostrzejszy werdykt "Modbus bez odpowiedzi", bez ani jednej linii o zmianie progu.
  //
  // Zeby zrobic to uczciwie, trzeba wiedziec, jak czesto ta druga piatka pada.
  // NADAL TEGO NIE WIEMY — i /api/log na to pytanie nie odpowie: Log.cpp:7 trzyma
  // bufor KOLOWY 3072 B, czyli ~47 linii, a przy zdrowej pracy (PV co 30 s, loty
  // co 15 s) jest to okno rzedu SZESCIU MINUT. Poprzednia wersja tego pliku scalila
  // liczniki i podniosla prog do 6, powolujac sie na "7 h bez ani jednej porazki"
  // zmierzone przez `grep -c` po /api/log. To byl argument o szesciu minutach,
  // podpisany jako argument o siedmiu godzinach. Stad ten rollback.
  //
  // Pomiar zbiera teraz licznik kumulacyjny w /api/diag (nizej): zyje tyle, co
  // uptime, wiec po dobie powie prawde o dobie. Prog wroci do rozmowy, gdy beda dane.
  //
  // `extra` liczy druga piatke (32016, 32087, 32086, 32089, 37100): idzie do pomiaru
  // i do logu, ale NIE zamyka sesji.
  int fails = 0;
  int extra = 0;
  uint16_t missing = 0;    // pierwszy rejestr MOCY, ktory nie doszedl (0 = komplet)
  uint16_t firstFail = 0;  // pierwszy nieudany odczyt z DOWOLNEJ grupy (tylko do logu)

  auto mark = [&](uint16_t addr) {
    if (!firstFail) firstFail = addr;
  };

  // Trzy rejestry mocy sa nierozlaczne — z dwoch z nich liczy sie houseLoadW.
  if (readS32(32064, s32)) snap.powerDcW = s32; else { ++fails; mark(32064); if (!missing) missing = 32064; }
  if (readS32(32080, s32)) snap.powerAcW = s32; else { ++fails; mark(32080); if (!missing) missing = 32080; }
  if (readS32(37113, s32)) snap.gridPowerW = s32; else { ++fails; mark(37113); if (!missing) missing = 37113; }
  if (readU32(32106, u32)) snap.energyTotalKwh = static_cast<float>(u32) / 100.f; else { ++fails; mark(32106); }
  if (readU32(32114, u32)) snap.energyTodayKwh = static_cast<float>(u32) / 100.f; else { ++fails; mark(32114); }

  // Druga piatka. snap jest wyzerowany, wiec nieudany odczyt wychodzi na ekran jako
  // pomiar — i chronia przed tym DWAJ wartownicy z pieciu, nie wszyscy:
  //
  //   32089 (statusCode) -> 0xFFFF: kodu tego Huawei nie uzywa, wiec pvStatusLabel()
  //         zejdzie do default i powie "Stan nieznany" zamiast udawac "Czuwanie"
  //         (tak czyta sie 0) w sloneczne poludnie.
  //   37100 (meterOk)    -> true: zeby brak odczytu nie podnosil alarmu o awarii
  //         licznika, ktorej nikt nie widzial.
  //
  // Pozostale trzy — pvVoltageV, inverterTempC, efficiencyPct — wartownika NIE MAJA.
  // Przy porazce zostaja na 0 z `PvSnapshot snap{}` i, jesli odczyt przejdzie bramki
  // nizej, ida na ekran jako pomiar: temperatura 0,0 C zima wyglada wiarygodnie.
  // Tak bylo juz w v97, wiec to nie jest regresja — ale trzeba to nazwac wprost,
  // zamiast pisac, ze wartownicy pilnuja "reszty". Sentinel dla tej trojki wymaga
  // zmiany PvData.h i UI, czyli osobnej decyzji; do tego czasu ten komentarz ma
  // mowic, ile naprawde pilnujemy: dwa z pieciu.
  if (readS16(32016, 10, f)) snap.pvVoltageV = f; else { ++extra; mark(32016); }
  if (readS16(32087, 10, f)) snap.inverterTempC = f; else { ++extra; mark(32087); }
  if (readU16(32086, u16)) snap.efficiencyPct = static_cast<float>(u16) / 100.f; else { ++extra; mark(32086); }
  if (readU16(32089, u16)) snap.statusCode = u16; else { snap.statusCode = 0xFFFF; ++extra; mark(32089); }
  if (readU16(37100, u16)) snap.meterOk = (u16 == 1); else { snap.meterOk = true; ++extra; mark(37100); }

  // Licznik kumulacyjny — to jest wlasciwa odpowiedz na "jak czesto padaja", bo
  // w odroznieniu od /api/log nie ma okna: zyje tyle, co uptime. Zadnego mutexa
  // (pola sa tylko dopisywane, czyta je webTask) i zadnej operacji sieciowej wyzej.
  // fails/extra to 0..5, wiec indeks histogramu zawsze miesci sie w [6].
  {
    Diag& dg = diag();
    ++dg.pvCycles;
    dg.pvFails += static_cast<uint32_t>(fails);
    dg.pvExtraFails += static_cast<uint32_t>(extra);
    ++dg.pvFailHist[fails];
    ++dg.pvExtraHist[extra];
  }

  // Ktory rejestr padl, nie tylko ile ich bylo — na urzadzeniu bez konsoli inaczej
  // zostaje samo "Modbus bez odpowiedzi". Najwyzej jedna linia na cykl.
  //
  // `&& !night` jest tu istotne: o zmroku TCP jeszcze stoi, a rejestry juz milcza,
  // wiec bez tego warunku ta linia leci co 5 minut przez CALA NOC — tuz obok
  // neutralnego "falownik usypia", nazywajac sen awaria. Do tego zjada okno logu,
  // ktore i tak ma tylko ~47 linii. Nocne milczenie rejestrow widac w histogramie
  // w /api/diag, i tam jest na nie miejsce.
  if ((fails > 0 || extra > 0) && !night) {
    LOG("PV: nie doszlo %d z 5 podstawowych, %d z 5 dodatkowych (pierwszy: %u)",
        fails, extra, static_cast<unsigned>(firstFail));
  }

  // Sesja się posypała — zamknij, żeby następny cykl zaczął od czystego handshake.
  // (Tak samo wygląda zasypianie falownika o zmroku: TCP jeszcze stoi, ale rejestry
  // już milczą. Nocą nie robimy z tego alarmu.)
  if (fails >= 3) {
    gSock.stop();
    out.online = false;
    out.asleep = night;
    strncpy(out.errorMsg, night ? "Falownik uśpiony" : "Modbus bez odpowiedzi",
            sizeof(out.errorMsg) - 1);
    return false;
  }

  // Brak KTOREGOKOLWIEK z trzech rejestrow mocy = calego odczytu nie wolno pokazac.
  // Prog fails >= 3 tego nie lapie, bo to jest JEDNA porazka. A gdy padalo 37113
  // (bilans sieci — timeout, chwilowa desynchronizacja, licznik na innym Modbusie),
  // gridPowerW zostawalo 0 i wychodzilo houseLoadW = powerAcW - 0 = CALA produkcja:
  // sloneczne poludnie, 5 kW z paneli i "Pobor domu: 5000 W", "Siec: 0 W". Ta sama
  // liczba szla do PvHistory (czerwona krzywa poboru rysowana dokladnie pod zolta
  // krzywa produkcji, na stale, do NVS) i do Home Assistanta jako encja "Pobor domu".
  // Rachunek sie nie zgadzal, a wykres wygladal przekonujaco.
  // Sesji tutaj NIE zamykamy: readRegsRetry probowal juz 3 razy, a gdy reszta
  // rejestrow odpowiada, to nie polaczenie jest zepsute — pelny handshake kosztuje
  // ~20 s rozgrzewki Huawei i zaszkodzilby bardziej, niz pomogl.
  // Rozroznienie "spi" od "zepsuty" zostaje: nocą to nadal stan neutralny.
  if (missing != 0) {
    out.online = false;
    out.asleep = night;
    if (night) {
      strncpy(out.errorMsg, "Falownik uśpiony", sizeof(out.errorMsg) - 1);
    } else {
      // Numer rejestru na ekranie i w /api/diag — inaczej ta awaria jest nie do
      // odroznienia od zwyklego braku sieci na urzadzeniu bez konsoli.
      snprintf(out.errorMsg, sizeof(out.errorMsg), "Brak rejestru mocy %u",
               static_cast<unsigned>(missing));
    }
    return false;
  }

  // Zużycie domu = produkcja AC minus to, co poszło do sieci.
  snap.houseLoadW = snap.powerAcW - snap.gridPowerW;
  snap.valid = true;
  out.data = snap;
  out.online = true;
  return true;
}
