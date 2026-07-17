# F — ZAMEL MEW-01 przez SUPLA: rozpoznanie

Data: 2026-07-17. Charakter: rozpoznanie, bez kodu.
Wszystkie ustalenia poparte źródłem. Co niepewne — oznaczone wprost.

---

## 1. Werdykt jednym zdaniem

**Warto — MEW-01 to jedyny w tej instalacji niezależny pomiar `houseLoadW`, a droga jest tania:
most na TrueNAS sumuje fazy i publikuje JEDEN temat, ESP tylko go subskrybuje (zero nowego TLS,
zero OAuth, zgodnie z decyzją z issue #14).** Drogę wybrać trzeba świadomie: **most z brokera
chmury SUPLI** (nieinwazyjne, zachowuje aplikację SUPLA) albo **tryb MQTT w samym MEW-01**
(całkowicie lokalne, ale **zabija aplikację SUPLA** — urządzenie odłącza się od chmury).

---

## 2. Jak działa MQTT w SUPLA

### 2.1 To jest broker SUPLI w chmurze — SUPLA NIE publikuje do naszego brokera

To jest zasadnicze ustalenie i odpowiada wprost na pytanie z zadania.

„Połączenie Twojego konta z brokerem MQTT" w `cloud.supla.org` **udostępnia Ci konto na brokerze
SUPLI**. Serwer SUPLI publikuje stany kanałów **na swój własny broker**, a Ty się do niego
**podłączasz jako klient**. To model *pull* — nic nie przychodzi do `192.168.0.9` samo z siebie.

- Host: `mqtt<N>.supla.org` — numer zależy od serwera, na którym stoi Twoje konto
  (spotykane w praktyce: `mqtt1.supla.org`, `mqtt74.supla.org`)
- Port: **8883, TLS**
- Uwierzytelnianie: **login + hasło generowane w Cloud** (nie token, nie OAuth)
- Ścieżka w Cloud: *Moje konto → Integracje → Broker MQTT → włącz*
- **Hasło pokazywane jest RAZ.** Nie da się go odczytać ponownie — tylko wygenerować nowe.
- Zgłaszane w praktyce: konieczne wymuszenie **MQTT 3.1.1** (część klientów domyślnie
  negocjuje inaczej i połączenie pada)

Źródła:
- [Connect to MQTT Broker via Home Assistant — en-forum.supla.org](https://en-forum.supla.org/viewtopic.php?t=12253)
- [MQTT — pierwsze kroki — forum.supla.org](https://forum.supla.org/viewtopic.php?t=12668)
- [Home Assistant MQTT Broker Not Working — en-forum.supla.org](https://en-forum.supla.org/viewtopic.php?t=13311)

**Konsekwencja architektoniczna:** żeby dane trafiły na nasz broker, ktoś musi utrzymywać
sesję TLS z `mqtt<N>.supla.org`. **ESP tego nie zrobi** (patrz kontekst: 2960 B zapasu).
Robi to TrueNAS.

### 2.2 Struktura tematów — zweryfikowana w kodzie źródłowym

Temat budowany jest w `supla-core`, plik
[`supla-server/src/mqtt/mqtt_message_provider.cpp`](https://github.com/SUPLA/supla-core/blob/master/supla-server/src/mqtt/mqtt_message_provider.cpp),
linie 65–66:

```c
snprintf(*topic_name_out, tn_size, "%s%ssupla/%s/",
         prefix_len > 0 ? topic_prefix : "", prefix_len > 0 ? "/" : "",
         suid);
```

Czyli pełny wzorzec:

```
[<prefix>/]supla/<SUID>/devices/<deviceId>/channels/<channelId>/state/<pole>
```

- `<prefix>` — opcjonalny, konfigurowalny; przy pustym znika razem z ukośnikiem
- `<SUID>` — *short unique id* użytkownika (na forum opisywany jako „HASH USERA"),
  stała dla konta, widoczna po podłączeniu do brokera
- `<deviceId>`, `<channelId>` — liczby całkowite z Cloud

Reszta ścieżki pochodzi z
[`mqtt_abstract_state_message_provider.cpp`](https://github.com/SUPLA/supla-core/blob/master/supla-server/src/mqtt/mqtt_abstract_state_message_provider.cpp),
funkcja `get_electricitymeter_message_at_index()` — dokładnie te literały:

```
devices/%i/channels/%i/state/total_forward_active_energy
devices/%i/channels/%i/state/total_forward_active_energy_balanced
devices/%i/channels/%i/state/phases/%i/power_active
devices/%i/channels/%i/state/phases/%i/voltage
...
```

**Ładunek to gołe liczby ASCII, nie JSON.** Jeden temat = jeden skalar.
To akurat dla nas dobre: parsowanie po stronie ESP to `atof`, zero JSON-a.

### 2.3 Prawdziwy przykład — realny MEW-01

Zrzut od użytkownika WMP (przez telegraf do InfluxDB), urządzenie `zamel-mew-01-3a23c`.
To jest **tryb lokalny** (bez `<SUID>`, patrz §4), ale ogon tematu jest identyczny jak w chmurze:

```
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/voltage                       231.82
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/current                       0.787
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/power_active                  86.94076
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/power_reactive                -48.1335
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/power_apparent                179.00546
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/power_factor                  0.476
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/phase_angle                   -24.4
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/frequency                     49.99
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/2/total_forward_active_energy   2.64154
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/3/power_active                  1954.14274
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/3/voltage                       231.49
supla/devices/zamel-mew-01-3a23c/channels/0/state/phases/3/current                       8.644
supla/devices/zamel-mew-01-3a23c/channels/0/state/total_forward_active_energy            29.11634
supla/devices/zamel-mew-01-3a23c/channels/0/state/total_reverse_active_energy            0
supla/devices/zamel-mew-01-3a23c/channels/0/state/total_forward_active_energy_balanced   29.1253
supla/devices/zamel-mew-01-3a23c/channels/0/state/support                                65535
```

Źródło: [solar-logger issue #21](https://github.com/basking-in-the-sun2000/solar-logger/issues/21)
(cytat z [issue #2, komentarz @WMP](https://github.com/basking-in-the-sun2000/solar-logger/issues/2#issuecomment-877469815))

Sprawdzenie jednostek na tych liczbach — zgadza się:
`231.49 V × 8.644 A = 2001 VA`, `power_apparent = 1961.87`, `power_factor = 0.995`,
`1954.14 / 1961.87 = 0.996`. Czyli **`power_active` jest w watach**, nie w dziesiątkach miliwatów.

---

## 3. Co da MEW-01 — pola, jednostki, częstość

### 3.1 Wariant urządzenia — DO POTWIERDZENIA PRZEZ WŁAŚCICIELA

**To trzeba rozstrzygnąć zanim cokolwiek napiszemy**, bo zmienia arytmetykę:

| Model | Fazy |
|---|---|
| **MEW-01** (goły symbol) | **3-fazowy, 3F+N** |
| **MEW-01/1F** | 1-fazowy, 1F+N |
| **MEW-01/ANT-1F** | 1-fazowy z anteną zewnętrzną |
| MEW-01 Lite | 3F+N, wersja okrojona |
| LEW-01 | 1-fazowy 16A (inne urządzenie) |

Właściciel napisał „ZAMEL MEW-01" — **sam symbol MEW-01 to wersja 3-fazowa**.
Skoro mieszkanie stoi za falownikiem SUN2000, 3-fazowe zasilanie jest prawdopodobne.
**Ale to jest domysł — trzeba spojrzeć na urządzenie.**

Źródła: [supla.zamel.com — MEW-01 3F+N](https://supla.zamel.com/en/product/wi-fi-energy-meter-3-ph-n/),
[sklepzamel.com — MEW-01/1F](https://sklepzamel.com/produkt/monitor-energii-elektrycznej-wifi-1fn-mew-01-1f/)

Kod SUPLI obsługuje oba przez flagi `SUPLA_CHANNEL_FLAG_PHASE{1,2,3}_UNSUPPORTED` —
dla wersji 1-fazowej tematy faz 2 i 3 nie niosą wartości.

### 3.2 Pełna lista pól kanału typu *electricity meter*

Z `get_electricitymeter_message_at_index()` — indeksy 0..45, czyli **46 tematów na kanał**.

**Poziom urządzenia** (`.../state/<pole>`):

| Pole | Jednostka | Uwagi |
|---|---|---|
| `total_forward_active_energy` | **kWh** | suma 3 faz, `(faza1+faza2+faza3) × 0.00001` |
| `total_reverse_active_energy` | kWh | oddanie; za falownikiem powinno być ≈ 0 |
| `total_forward_active_energy_balanced` | kWh | bilansowana międzyfazowo — **NIE dla nas**, patrz §6 |
| `total_reverse_active_energy_balanced` | kWh | j.w. |
| `total_cost`, `total_cost_balanced` | waluta | tylko jeśli ustawiona taryfa w Cloud |
| `price_per_unit`, `currency` | — | j.w. |
| `support` | bitmaska | `65535` w przykładzie = wszystko wspierane |

**Poziom fazy** (`.../state/phases/<1..3>/<pole>`) — 12 pól × 3 fazy:

| Pole | Jednostka |
|---|---|
| `power_active` | **W** |
| `power_reactive` | var |
| `power_apparent` | VA |
| `power_factor` | — (cos φ, 0..1) |
| `phase_angle` | stopnie |
| `voltage` | V |
| `current` | A |
| `frequency` | Hz |
| `total_forward_active_energy` | kWh |
| `total_reverse_active_energy` | kWh |
| `total_forward_reactive_energy` | kvarh |
| `total_reverse_reactive_energy` | kvarh |

Tak — jest **napięcie, prąd, częstotliwość i cos φ per faza**. Odpowiedź na pytanie 3 z zadania.

### 3.3 ⚠ NIE MA mocy chwilowej na poziomie urządzenia

**To jest najważniejsze ustalenie praktyczne całego raportu.**

Na poziomie urządzenia jest **tylko energia sumaryczna**. `power_active` istnieje
**wyłącznie per faza**. Nie ma tematu `state/power_active`.

Sprawdzone: w `get_electricitymeter_message_at_index()` indeksy 1–9 to poziom urządzenia
(same koszty, energie i `support`), a `power_active` pojawia się dopiero w indeksach
17 / 29 / 41 — czyli w blokach fazowych.

**Czyli `houseLoadW` = `phases/1/power_active + phases/2/power_active + phases/3/power_active`.**
Sumowanie musi zrobić most na TrueNAS. ESP dostaje gotową liczbę.

To bezpośrednio odpowiada na obawę z zadania: *„jeśli 3-fazowy, a my liczymy jedną liczbę,
trzeba wiedzieć, co sumować"*. Sumujemy `power_active` po fazach — arytmetycznie, nie „balanced".

### 3.4 Częstość publikacji — CZĘŚCIOWO USTALONA

- Biblioteka `supla-device` ma **domyślnie `refreshRateSec = 5`**
  ([`src/supla/sensor/electricity_meter.h`, linia 408](https://github.com/SUPLA/supla-device/blob/main/src/supla/sensor/electricity_meter.h)),
  wymuszane minimum 1 s (`electricity_meter.cpp:762-764`).
- **ALE MEW-01 ma zamknięty firmware ZAMEL-a na ESP8266 i nie mam dowodu, że używa tej
  biblioteki z tą wartością.** MEW-01 jest starszy niż `supla-device`.
- Poszlaka ze zrzutu WMP: znaczniki czasu kolejnych tematów są ~100 ms od siebie, a cały
  cykl 46 tematów zajmuje ~4 s. Sugeruje to okres rzędu **kilku sekund**, ale to
  odczyt z InfluxDB po telegrafie, więc **nie jest to dowód na okres publikacji**.

**Wniosek dla wykresu:** jeśli okres to ~5 s, jest **6× gęściej niż falownik (30 s)** —
wykres wyjdzie ładnie, aliasingu jak przy piecu (3 min) nie będzie. Ale to trzeba
**zmierzyć `mosquitto_sub` przed pisaniem kodu wykresu**, nie zakładać.

---

## 4. Droga lokalna — ISTNIEJE, i jest lepsza niż się spodziewałem

Odpowiedź na pytanie 5: **tak, MEW-01 potrafi publikować wprost na nasz broker, z całkowitym
pominięciem chmury SUPLI.** Nie trzeba nawet stawiać własnego `supla-server`.

### 4.1 Tryb MQTT w firmware MEW-01

Procedura (potwierdzona w dwóch niezależnych źródłach):

1. Przytrzymać przycisk na obudowie ~5–8 s → dioda miga ~4×/s → tryb konfiguracji
2. Podłączyć się do sieci Wi-Fi `ZAMEL-[nazwa]-[numer]`
3. Wejść na `http://192.168.4.1`
4. **Wybrać protokół: MQTT zamiast SUPLA**
5. Podać: adres brokera, port, użytkownika, hasło, **opcjonalnie prefiks tematu**
6. Zapisać, zrestartować. Stałe świecenie diody = połączony.

Firmware **wspiera autodiscovery Home Assistant** — czyli dokładnie ten sam mechanizm,
który już mamy w `MqttClient.cpp`.

Tematy w tym trybie **nie mają `<SUID>`** (bo nie ma konta w chmurze):

```
[<prefix>/]supla/devices/zamel-mew-01-<xxxxx>/channels/0/state/phases/1/power_active
```

Źródła:
- [Fundacja Phinix — ZAMEL SUPLA MQTT (instrukcja krok po kroku, ze zrzutami)](https://docs.phinix.org/Integracje_z_peryferiami/ZAMEL_SUPLA/)
- [SmartNow — MEW-01: podłączenie bezpośrednio do brokera MQTT z pominięciem serwerów Supla](https://smartnow.pl/mew-01-podlaczenie-bezposrednio-do-brokera-mqtt-z-pominieciem-serwerow-supla/)
- [MQTT na pokładzie MEW-a — forum.supla.org](https://forum.supla.org/viewtopic.php?t=8752)
- [MEW01 + supla/mqtt + HA — forum.supla.org](https://forum.supla.org/viewtopic.php?t=10356)

### 4.2 ⚠ Cena trybu lokalnego

**Wybór MQTT = urządzenie odłącza się od serwera SUPLA. Aplikacja SUPLA przestaje pokazywać
MEW-01. Historia pomiarów w Cloud się kończy.** To jest przełącznik albo-albo, nie „oba naraz".

Źródło: [SmartNow](https://smartnow.pl/mew-01-podlaczenie-bezposrednio-do-brokera-mqtt-z-pominieciem-serwerow-supla/)
— *„aplikacja Supla nie będzie odczytywać danych z MEW-01 – nastąpi odłączenie od serwera"*.

Odwracalne (wystarczy wrócić na 192.168.4.1 i przełączyć z powrotem), ale **historia w chmurze
nie wróci**. Aktualizacja firmware w trybie MQTT też jest osobnym tematem
([forum.supla.org t=14059](https://forum.supla.org/viewtopic.php?t=14059)).

### 4.3 Trzecia droga: własny supla-server

Istnieje [`SUPLA/supla-docker`](https://github.com/SUPLA/supla-docker) — cały serwer SUPLA
w kontenerach, open source. MEW-01 pozwala **zmienić sam adres serwera** i łączy się bez
problemu ([forum t=11571](https://forum.supla.org/viewtopic.php?t=11571)).
Zachowuje aplikację SUPLA **i** jest lokalny.

Koszt: utrzymywanie całego serwera + bazy na TrueNAS, i **własny broker MQTT trzeba wtedy
skonfigurować w `.env`** (prywatny supla-server nie ma wbudowanego brokera).
Migracja historii nie istnieje — urządzenia rejestruje się od zera.

**Ocena: nieproporcjonalne do jednej liczby, której potrzebujemy.** Odnotowane, nie rekomendowane.

---

## 5. Podział pracy

### 5.1 Właściciel — kliknięcia

**Najpierw, niezależnie od drogi:**
- [ ] **Sprawdzić na urządzeniu, czy to MEW-01 (3F+N) czy MEW-01/1F.** Bez tego ani kroku.

**Droga A — most z chmury (rekomendowana na start, nieinwazyjna):**
- [ ] `cloud.supla.org` → *Moje konto → Integracje → **Broker MQTT*** → **Włącz**
- [ ] Przepisać **host** (`mqtt<N>.supla.org`), **użytkownika** i **hasło**
      — **hasło zapisać od razu, pokaże się tylko raz**
- [ ] W Cloud odczytać **`deviceId`** i **`channelId`** licznika
- [ ] Podać hasło do NVS/sekretów TrueNAS — **nie do repo** (repo publiczne)
- [ ] *Nie* dotykać zakładki „Moje aplikacje OAuth" — do tego celu niepotrzebna (§6.5)

**Droga B — lokalna (jeśli aplikacja SUPLA jest zbędna):**
- [ ] Przycisk na MEW-01 ~5–8 s → Wi-Fi `ZAMEL-...` → `http://192.168.4.1`
- [ ] Protokół: **MQTT**, broker `192.168.0.9`, port `1883`, user/hasło mosquitto
- [ ] Prefiks tematu: proponuję `dom` → `dom/supla/devices/...`
- [ ] Świadoma zgoda: **MEW-01 znika z aplikacji SUPLA**

### 5.2 Most na TrueNAS — cała robota

Jeden mały serwis (Node-RED / telegraf / 30 linii Pythona — **do decyzji, nie teraz**):

- **Droga A:** utrzymuje sesję TLS `mqtt<N>.supla.org:8883`, MQTT **3.1.1**, subskrybuje
  `supla/<SUID>/devices/<id>/channels/<id>/state/phases/+/power_active`
- **Droga B:** nic nie utrzymuje — MEW-01 sam wpada na `192.168.0.9:1883`
- **W obu:** **sumuje `power_active` po fazach** (§3.3) i publikuje **jeden temat**, np.
  `dom/mew01/house_load_w`, **z `retain=true`**
- Opcjonalnie drugi temat z `total_forward_active_energy` (kWh) — do porównania dobowego,
  analogicznie do `GasHistory`

**Uwaga na drogę A:** `mosquitto` w trybie `bridge` **nie umie sumować** — przeniesie tematy
1:1, ale sumę i tak musi policzyć coś wyżej. Czyli most ≠ sam `mosquitto bridge`.

### 5.3 ESP32-S3 — minimum

- Subskrypcja **jednego** tematu w istniejącym `MqttClient.cpp` (PubSubClient, już połączony
  z `192.168.0.9:1883`)
- `atof` na ładunku ASCII — **bez JSON-a, bez TLS, bez OAuth**
- Porównanie z wyliczanym `houseLoadW = powerAcW - gridPowerW` → rozjazd na ekran,
  dokładnie jak licznik gazu kontra deklaracja pieca
- Koszt RAM: jedna subskrypcja + jeden `float`. **Mieści się w 2960 B z zapasem.**

---

## 6. Ryzyka i pułapki

### 6.1 ✅ Retain — DOBRA wiadomość, zweryfikowana w kodzie

Pytanie z zadania było trafne i odpowiedź jest po naszej stronie.

[`mqtt_publisher_datasource.cpp`, linie 432–447](https://github.com/SUPLA/supla-core/blob/master/supla-server/src/mqtt/mqtt_publisher_datasource.cpp):

```c
bool supla_mqtt_publisher_datasource::fetch(char **topic_name, void **message,
                                            size_t *message_size,
                                            bool *retain) {
  if (fetch_actions(topic_name, message, message_size)) {
    if (retain) {
      *retain = false;          // akcje (execute_action) — bez retain
    }
    return true;
  }

  *retain = true;               // <<< WSZYSTKIE STANY — retained
  return supla_mqtt_client_db_datasource::fetch(topic_name, message,
                                                message_size, retain);
}
```

**Stany są retained.** Po restarcie ESP dostaje ostatnią wartość natychmiast po subskrypcji,
bez czekania na kolejny cykl. To dokładnie to, czego chcieliśmy („obecność/stan chcemy mieć
po restarcie").

⚠ **Ale to dotyczy brokera CHMURY (droga A).** Dla trybu lokalnego (droga B) publikuje
zamknięty firmware ZAMEL-a i **nie mam dowodu, że ustawia retain**. Do sprawdzenia
`mosquitto_sub`. Gdyby nie ustawiał — most i tak republikuje własny temat z `retain=true`,
więc **dla ESP problem znika w obu wariantach**. To argument za tym, żeby most był serwisem,
a nie samym `bridge`.

### 6.2 ⚠ Puste ładunki na nieobsługiwanych fazach — kandydat na „mylącą odpowiedź"

W `get_electricitymeter_message_at_index()` przy nieobsługiwanej fazie (flagi
`SUPLA_CHANNEL_FLAG_PHASE{1,2,3}_UNSUPPORTED`) albo braku danych kod ustawia `emv = nullptr`
i dalej:

```c
if (emv == nullptr) {
    message = nullptr;
    if (message_size) {
      *message_size = 0;
      message_size = nullptr;
    }
}
```

Temat **i tak jest tworzony**, ale z rozmiarem 0. **Pusty ładunek na temacie retained kasuje
retained message** — to standard MQTT.

Dla MEW-01/1F oznaczałoby to, że `phases/2/power_active` i `phases/3/power_active`
**mogą istnieć jako tematy bez wartości**. Naiwne `atof("")` = `0.0` — akurat nieszkodliwe
przy sumowaniu, ale **`sum += atof(payload)` po pustym ładunku to dokładnie ten rodzaj cichego
zera, który już nas gryzł**.

**Most musi rozróżniać „faza = 0 W" od „faza nie nadaje".** Nie widziałem tego na żywo —
to odczyt kodu, nie dowód. Do sprawdzenia `mosquitto_sub -v`.

### 6.3 ⚠ `balanced` to nie to samo co suma — nie pomylić

`total_forward_active_energy_balanced` to energia **bilansowana międzyfazowo** (nadwyżka na
jednej fazie kompensuje pobór na innej — pod net-metering prosumenta).
`total_forward_active_energy` to **zwykła suma faz**.

W zrzucie WMP: `29.11634` (suma) vs `29.1253` (balanced) — **różnią się**, choć niewiele.

MEW-01 mierzy **zużycie mieszkania za falownikiem**, więc bilansowanie nie ma tu sensu
fizycznego. **Bierzemy `total_forward_active_energy`.** Wzięcie `balanced` dałoby liczbę
wyglądającą wiarygodnie i cicho złą — czyli najgorszy możliwy rodzaj błędu.

### 6.4 Pozostałe ryzyka

| Ryzyko | Ocena |
|---|---|
| **Klasa dokładności MEW-01 = klasa 2 (±2%)** | DTSU666-H jest zwykle dokładniejszy. **Rozjazdu ±2% NIE wolno raportować jako awarii.** Próg alarmu musi to uwzględniać — inaczej zbudujemy sobie fałszywy alarm jak przy piecu. Źródło: [supla.zamel.com](https://supla.zamel.com/en/product/wi-fi-energy-meter-3-ph-n/) |
| **Dwa różne punkty pomiarowe** | MEW-01 (mieszkanie) vs `powerAcW − gridPowerW` (wyprowadzone). Różnica to **nie tylko błąd** — to też autokonsumpcja poza MEW-01 i straty. Weryfikacja pokaże **trend**, nie równość. |
| **Zależność od chmury (droga A)** | Broker SUPLI to SPOF. **Ale to model push z retained** — dużo łagodniejszy niż Viessmann REST. Nie ma HTTP 200 przy odrzuconej komendzie, nie ma chunked transfer, nie ma rotacji tokenów. **Tylko czytamy, nic nie sterujemy** — cała rodzina błędów Viessmanna nas tu nie dotyczy. |
| **Hasło brokera pokazywane raz** | Zgubione = trzeba wygenerować nowe i przekonfigurować most. |
| **MQTT 3.1.1 wymuszone** | Zgłaszane na forum jako typowa przyczyna nieudanych połączeń. |
| **ESP8266 w MEW-01 i Wi-Fi 40 MHz** | ESP8266 **nie obsługuje kanału 40 MHz** — jeśli AP ma 40 MHz na 2.4 GHz, MEW-01 się nie połączy. Źródło: [Phinix](https://docs.phinix.org/Integracje_z_peryferiami/ZAMEL_SUPLA/) |
| **46 tematów × kilka sekund** | Dla TrueNAS bez znaczenia. **ESP nie subskrybuje `#`** — dostaje jeden temat. |
| **Limity zapytań** | **Nie ustaliłem** — patrz §7. |

### 6.5 OAuth / REST API — istnieje, odnotowane, ODRZUCONE

Zgodnie z poleceniem: sprawdzone, że istnieje.

- REST API v2.3 / v3, dokumentacja: [svr1.supla.org/api-docs/docs.html](https://svr1.supla.org/api-docs/docs.html),
  [SwaggerHub](https://app.swaggerhub.com/apis/supla/supla-cloud-api/2.3.0)
- OAuth2 authorization-code: `https://cloud.supla.org/oauth/v2/auth`
- Alternatywa prostsza: **personal access token** (*Moje konto → Integracje → Tokeny dostępu
  osobistego*), wysyłany jako `Authorization: Bearer <token>`
- Kanał licznika zwraca w `state` m.in. `totalForwardActiveEnergy`, `totalReverseActiveEnergy`,
  `totalForwardReactiveEnergy` oraz per faza `current`, `voltage`, `frequency`, `powerFactor`, `phaseAngle`
- Źródło: [SUPLA/supla-cloud Wiki — Integrations](https://github.com/SUPLA/supla-cloud/wiki/Integrations)

**Werdykt: nie dla ESP i uprzedzenie z zadania było słuszne.** Kolejny klient TLS + OAuth to
kilkadziesiąt kB sterty przy 2960 B zapasu i 1444 B w dołku OTA. **Nie ma mowy.**
MQTT daje to samo taniej i bez tokenów. REST bywałby ewentualnie użyteczny **na TrueNAS**
do jednorazowego odczytania `deviceId`/`channelId` — ale to samo widać klikając w Cloud,
więc i tam zbędny.

---

## 7. Czego NIE ustaliłem

Uczciwie, bez zapychania:

1. **Wariant MEW-01 właściciela (3F vs 1F).** Sam symbol „MEW-01" wskazuje na 3F+N, ale to
   **wnioskowanie z nazewnictwa katalogowego, nie oględziny**. Rozstrzyga to arytmetykę sumy.
2. **Rzeczywisty okres publikacji MEW-01.** Domyślna wartość `refreshRateSec = 5` pochodzi
   z biblioteki `supla-device` — **nie potwierdziłem, że firmware ZAMEL-a jej używa**.
   MEW-01 jest starszy. Poszlaka ze zrzutu WMP sugeruje kilka sekund. **Zmierzyć `mosquitto_sub`.**
3. **Czy tryb lokalny MEW-01 publikuje z `retain`.** Zamknięty firmware, brak źródeł.
   (Praktycznie nieszkodliwe — most republikuje z retain.)
4. **Numer brokera dla konta właściciela.** `mqtt<N>.supla.org` zależy od serwera konta.
   W źródłach: `mqtt1`, `mqtt74`. **Widoczne dopiero po włączeniu integracji w Cloud.**
5. **Limity zapytań / throttling brokera SUPLI.** Pytanie 6 z zadania — **nie znalazłem
   żadnej dokumentacji limitów**: ani liczby jednoczesnych sesji, ani przepustowości,
   ani polityki rozłączania. Nie twierdzę, że ich nie ma — twierdzę, że ich nie znalazłem.
6. **Czy `deviceId`/`channelId` są stabilne** przy ponownym parowaniu urządzenia.
   Jeśli nie — most miałby zaszyty identyfikator, który cicho przestanie pasować.
   **Nie sprawdziłem.**
7. **Zachowanie tematów przy offline MEW-01.** Kod SUPLI ma `state/connected`
   (na forum widziane jako `.../state/connected`), ale **nie prześledziłem, czy przy
   utracie łączności stany są zerowane, czy zamrażane na ostatniej wartości**.
   To ma znaczenie: zamrożona wartość retained wygląda jak żywy odczyt. **Do sprawdzenia.**
8. **Czy istnieje lokalne HTTP API MEW-01 poza stroną konfiguracyjną `192.168.4.1`.**
   Strona konfiguracyjna działa tylko w trybie AP. **Nie znalazłem dowodów na REST-a
   z odczytami w trybie pracy.** Znalazłem wątek [„MEW-01 REST API"](https://en-forum.supla.org/viewtopic.php?t=6838),
   ale nie zdołałem pobrać treści — nie opieram na nim żadnego wniosku.

---

## 8. Rekomendacja

**Kolejność, gdyby to szło do realizacji:**

1. **Właściciel sprawdza wariant MEW-01** (3F czy 1F). Blokuje wszystko dalsze.
2. **Właściciel włącza broker MQTT w Cloud** i zapisuje hasło (droga A).
   Nieinwazyjne, odwracalne, **aplikacja SUPLA działa dalej**.
3. **`mosquitto_sub -v` z TrueNAS na `supla/<SUID>/#`** — i dopiero to rozstrzyga
   punkty 2, 5, 7 z §7: **realny okres, realne ładunki, realny retain**.
   **Żadnego kodu przed tym krokiem.**
4. Dopiero potem most sumujący → jeden temat retained → jedna subskrypcja w ESP.
5. **Droga B (lokalna) jako opcja później** — jeśli okaże się, że aplikacja SUPLA i tak nie
   jest używana. Wtedy wypada chmura, wypada TLS, wypada hasło do rotacji, a most zostaje
   (bo suma faz jest potrzebna zawsze).

**Dlaczego nie od razu lokalnie:** droga B jest architektonicznie czystsza, ale kasuje
aplikację SUPLA i historię w Cloud. Droga A nic nie psuje i pozwala **zmierzyć rzeczywistość
zanim podejmiemy nieodwracalną decyzję**. Przełączenie A→B później to jedna wizyta na
`192.168.4.1` i zmiana jednego tematu w moście.

**Zysk w obu drogach identyczny:** `houseLoadW` z niezależnego pomiaru, ESP obciążone jedną
subskrypcją i jednym `float`. To jest dokładnie ten sam wzorzec, co licznik gazu kontra
deklaracja pieca — i tak samo tanio.
