#include "Portal.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdlib>
#include <cstring>

// Zrzuty awaryjne (coredump). Rdzen 3.3.10 ma je WLACZONE od zawsze — sprawdzone
// w sdkconfig tego rdzenia (tools/esp32s3-libs/3.3.10/sdkconfig):
//   CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y, CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y,
//   CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y, CONFIG_ESP_COREDUMP_CHECK_BOOT=y
// a min_spiffs.csv (nasz schemat partycji) ma na to partycje: coredump, 0x3F0000, 64 kB.
// Czyli przy KAZDYM panicu urzadzenie samo zapisuje do flasha stan wszystkich zadan
// i przezywa to odlaczenie zasilania — tylko do tej pory nie bylo jak tego odczytac.
#include "esp_app_desc.h"   // esp_app_get_elf_sha256() — sha DZIALAJACEGO firmware'u
#include "esp_core_dump.h"
#include "esp_partition.h"
// v111: /api/diag "memfull" — zeby dalo sie zweryfikowac ekran PAMIEC zdalnie,
// bez patrzenia na urzadzenie. Oba juz i tak zlinkowane (ESP.getFreeHeap() korzysta
// z heap_caps_*, Ota.cpp/OtaGuard.cpp z esp_ota_ops.h) — to nie sa nowe zaleznosci.
#include "esp_heap_caps.h"
#include <esp_ota_ops.h>

#include "AirClient.h"
#include "Config.h"
#include "Log.h"
#include "MqttClient.h"
#include "Ota.h"
#include "OtaGuard.h"
#include "BleGateway.h"
#include "BleSensors.h"
#include "GasMeter.h"
#include "RadarMap.h"
#include "Viessmann.h"
#include "Settings.h"
#include "Version.h"

// ZAKRES GLOBALNY, nie wewnatrz namespace portal — gGas i gLock sa zdefiniowane
// w .ino jako ::gGas i ::gLock. Deklaracja wewnatrz namespace stworzylaby
// portal::gGas, ktore nie zlinkuje sie z niczym.
// webTask czyta gGas pod gLock: netTask pisze do niego co 3 minuty.
extern GasHistory gGas;
extern SemaphoreHandle_t gLock;

namespace portal {
namespace {

WebServer server(80);
DNSServer dns;
bool apMode = false;
bool started = false;
bool wifiSaved = false;

// Blokada skanu WiFi — patrz komentarz przy scanLock() w Portal.h.
// Tworzona w konstruktorze globalnym, czyli PRZED setup() i przed startem obu
// zadan: zaden skan nie moze jej wyprzedzic. Gdyby sterta padla przy tworzeniu,
// uchwyt zostaje NULL i skan idzie bez blokady — tak jak dzialalo do tej pory.
SemaphoreHandle_t gScanMx = xSemaphoreCreateMutex();

// Termin waznosci flagi "panel testuje WiFi" (0 = nie testuje). Trzymamy TERMIN,
// a nie zwykle true/false, zeby flaga wygasla sama nawet wtedy, gdy straznik
// ponizej nie zdazy jej skasowac. netTask nigdy nie zostanie z niej na lodzie.
volatile uint32_t gWifiCfgUntil = 0;
constexpr uint32_t kWifiCfgBusyMs = 20000;   // test trwa do 14 s + zapas

// Straznik RAII: podnosi flage na wejsciu, kasuje ja na KAZDEJ sciezce wyjscia
// z apiWifi (takze przy wczesnym return).
struct WifiCfgGuard {
  WifiCfgGuard() {
    uint32_t until = millis() + kWifiCfgBusyMs;
    if (until == 0) until = 1;   // 0 znaczy "nie testuje" — nie kolidujmy
    gWifiCfgUntil = until;
  }
  ~WifiCfgGuard() { gWifiCfgUntil = 0; }
};
void (*gScreenshot)(WiFiClient&) = nullptr;
void (*gViewSet)(int) = nullptr;
void (*gTap)(int) = nullptr;   // symulacja dotyku pinu z panelu (v127)
void (*gViewGet)(int&, int&) = nullptr;
// Podswietlenie — patrz setBacklightHandler() w Portal.h.
void (*gBlTest)(uint8_t, uint32_t) = nullptr;
void (*gBlSweep)(uint32_t) = nullptr;   // wizualny test rampy (v124)
void (*gBlGet)(uint8_t&, uint8_t&) = nullptr;
char apIpStr[20] = "192.168.4.1";

const char kApSsid[] = "Pogoda-Setup";
const char kApPass[] = "pogoda123";

// ------------------------------------------------------------------ strona ---

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=pl><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Wyświetlacz łazienkowy</title><style>
/* MOTYW PANELU "PASMOWY" (V3): jasne tlo, ciemny pasek i kolumna nawigacji, karty biale,
   typografia kondensowana w naglowkach, paleta ze specyfikacji projektanta. Uklad z Fazy 2:
   pasek statusu + (komputer) nawigacja boczna / (telefon) akordeon. Ta sama rodzina co ekran. */
*{box-sizing:border-box}
/* MOTYW: jasny domyslnie. Ciemny wariant = klasa .dark na <html> (przelacznik w pasku
   statusu, wybor zapisany w localStorage). Zmienne kolorow flipuja sie w bloku .dark
   nizej. Uwaga na podzial: --panel to KOLOR TEKSTU (ciemny w jasnym, jasny w ciemnym),
   a pasek gorny i nawigacja stoja na osobnym --chrome, ktory ZOSTAJE ciemny w obu
   trybach (dlatego przycisk primary ma wlasne --btnbg/--btnfg, a nie var(--panel)). */
:root{--bg:#F4F4F0;--panel:#1A1C1E;--second:#6C6F6A;--mute:#9A9C96;--line:#D9DCD6;
  --card:#FFFFFF;--accent:#2563C4;--ok:#4D9A4D;--warn:#B8901F;--warnbg:#F3E4C2;--err:#C04A3A;
  --chrome:#1A1C1E;--chrometxt:#F4F4F0;--btnbg:#1A1C1E;--btnfg:#F4F4F0;--track:#EDEEE9;--code:#FAFAF8;
  --cond:"IBM Plex Sans Condensed","Roboto Condensed","Arial Narrow",system-ui,sans-serif}
.dark{--bg:#14161A;--panel:#E8E8E4;--second:#B7BAB3;--mute:#8C8F89;--line:#2C3038;
  --card:#1E2126;--accent:#5B8DEF;--ok:#63B863;--warn:#D6A93A;--warnbg:#33290F;--err:#E06657;
  --chrome:#0E1013;--chrometxt:#F1F1ED;--btnbg:#E8E8E4;--btnfg:#16181C;--track:#2A2E35;--code:#0E1013}
body{margin:0;background:var(--bg);color:var(--panel);
  font:15px/1.5 -apple-system,system-ui,Segoe UI,Roboto,sans-serif}
a{color:var(--accent)}
.w{padding-bottom:40px}
/* --- PASEK GORNY: ciemny, pelna szerokosc; lewo tytul+IP, prawo pigulki statusu --- */
.top{background:var(--chrome);color:var(--chrometxt)}
.topin{max-width:1060px;margin:0 auto;padding:13px 18px;display:flex;flex-wrap:wrap;
  gap:10px 16px;align-items:center;justify-content:space-between}
.title{font-family:var(--cond);font-weight:700;font-size:19px;letter-spacing:.01em}
.top .sub{color:var(--mute);font-size:12px;margin-top:2px}
.stat{display:flex;flex-wrap:wrap;gap:6px}
.pill{background:#2A2D30;color:#E7E8E3;border-radius:999px;padding:4px 11px;font-size:12px;
  font-family:var(--cond);font-weight:600;letter-spacing:.02em;white-space:nowrap}
.pill.pOn{color:#88CC88}
.pill.pAp{color:#E7C67A}
/* --- UKLAD POD PASKIEM --- */
.layout{max-width:1060px;margin:0 auto}
#nav{display:none}
#content{padding:0}
/* Sekcje jako akordeon (domyslnie = telefon) */
section{background:var(--card);border-bottom:1px solid var(--line)}
.sechead{width:100%;display:flex;justify-content:space-between;align-items:center;gap:10px;
  background:var(--card);border:0;margin:0;padding:16px 18px;cursor:pointer;text-align:left;
  font-family:var(--cond);font-weight:600;font-size:16px;color:var(--panel)}
.hgrp{display:flex;align-items:center;gap:8px}
.chev{color:var(--mute);font-size:13px;transition:transform .15s}
section.open .chev{transform:rotate(90deg)}
.sechead .nb{background:var(--track);color:var(--second);border-radius:999px;padding:1px 9px;font-size:12px;
  font-family:var(--cond);font-weight:600}
.nb:empty{display:none}
.secbody{display:none;padding:0 18px 6px}
section.open .secbody{display:block}
.blk{padding:16px 0;border-bottom:1px solid var(--line)}
.blk:last-child{border-bottom:0}
h2{font-size:12px;letter-spacing:.09em;text-transform:uppercase;color:var(--second);margin:0 0 12px;
  font-family:var(--cond);font-weight:600}
label{display:block;font-size:11px;letter-spacing:.05em;text-transform:uppercase;color:var(--second);
  margin:10px 0 4px;font-family:var(--cond);font-weight:600}
input,select{width:100%;padding:10px 12px;background:var(--card);border:1px solid var(--line);
  border-radius:8px;color:var(--panel);font-size:15px}
input:focus,select:focus{outline:none;border-color:var(--accent)}
button{width:100%;margin-top:14px;padding:11px;border:0;border-radius:8px;background:var(--btnbg);
  color:var(--btnfg);font-weight:600;font-size:15px;cursor:pointer}
button:active{transform:translateY(1px)}
button.s{background:var(--card);color:var(--panel);border:1px solid var(--line);margin-top:8px}
.btnlink{display:block;text-align:center;text-decoration:none;padding:11px;border-radius:8px;
  background:var(--card);color:var(--panel);border:1px solid var(--line);font-weight:600;font-size:15px}
.row{display:flex;gap:10px}.row>*{flex:1}
.hint{font-size:12px;color:var(--second);margin-top:8px}
.ok{color:var(--ok)}.err{color:var(--err)}.warn{color:var(--warn)}
ul{list-style:none;margin:10px 0 0;padding:0}
li{padding:10px 12px;border:1px solid var(--line);border-radius:8px;margin-bottom:6px;cursor:pointer;
  display:flex;justify-content:space-between;align-items:center;background:var(--card)}
li:hover{border-color:var(--accent)}
.sig{color:var(--mute);font-size:12px}
.b{display:inline-block;padding:2px 7px;border-radius:5px;background:var(--track);font-size:11px;color:var(--second)}
.scr{position:relative;background:#000;border:1px solid var(--line);border-radius:10px;overflow:hidden;aspect-ratio:4/3}
.scr img{display:block;width:100%;height:100%;image-rendering:pixelated}
.live{font-size:11px;font-weight:600;color:var(--ok)}
.tabs{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.tabs button{flex:1 1 auto;width:auto;margin:0;padding:8px 10px;font-size:12px;font-weight:600;
  font-family:var(--cond);letter-spacing:.02em;background:var(--card);color:var(--panel);border:1px solid var(--line)}
.tabs.sm button{padding:6px 9px;font-size:11px}
.tabs button.on{background:var(--accent);color:#fff;border-color:var(--accent)}
.intline{display:flex;justify-content:space-between;align-items:center;gap:10px;
  padding:9px 0;border-bottom:1px solid var(--line)}
.intline:last-child{border-bottom:0}
.intline .il{font-family:var(--cond);font-weight:600}
.intline button{width:auto;margin:0;padding:6px 12px;font-size:12px}
.rgt{display:flex;gap:10px;align-items:center}
/* Telefon: cele dotykowe pelnej szerokosci — pigulki po 40% */
@media(max-width:520px){.tabs button{flex:1 1 40%}}
/* --- KOMPUTER (>=780px): nawigacja boczna + jedna sekcja naraz (SPA) --- */
@media(min-width:780px){
 .layout{display:flex;align-items:stretch}
 #nav{display:block;flex:0 0 182px;width:182px;background:var(--chrome);padding:12px 0}
 #content{flex:1;min-width:0;padding:6px 22px 60px}
 .navitem{display:flex;justify-content:space-between;align-items:center;gap:8px;
   padding:11px 18px;color:#C7C9C4;font-family:var(--cond);font-weight:600;font-size:15px;
   cursor:pointer;border-left:3px solid transparent}
 .navitem:hover{color:#fff;background:#26292C}
 .navitem.on{color:#fff;background:#26292C;border-left-color:var(--accent)}
 .navitem .nb{background:var(--accent);color:#fff;border-radius:999px;padding:1px 8px;font-size:11px;
   font-family:var(--cond);font-weight:600}
 .sechead{display:none}
 section{display:none;background:transparent;border-bottom:0}
 section.active{display:block}
 .secbody{display:block;padding:0}
}
/* --- Wspolne prymitywy nowych sekcji (Na zywo / Obecnosc / Zdrowie). Wykresy sa
   INLINE (svg + divy), bez zadnych bibliotek z CDN — urzadzenie bywa offline. --- */
.pill.tog{cursor:pointer;user-select:none}
.pill.tog:hover{color:#fff}
.gcards{display:grid;grid-template-columns:repeat(auto-fit,minmax(128px,1fr));gap:10px;margin:6px 0}
.gcard{background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:11px 13px}
.gcard .lbl{color:var(--second);font-size:11px;text-transform:uppercase;letter-spacing:.05em;font-family:var(--cond);font-weight:600}
.gcard .val{font-size:23px;font-weight:600;margin-top:3px;font-family:var(--cond);line-height:1.15}
.gcard .val small{font-size:13px;color:var(--mute);font-weight:400}
.gcard.hot .val{color:var(--warn)}
.gcard.bad .val{color:var(--err)}
.brow{display:flex;align-items:center;gap:10px;margin:6px 0;font-size:13px}
.brow .k{width:74px;color:var(--second);font-family:var(--cond)}
.brow .track{flex:1;height:13px;background:var(--track);border-radius:7px;overflow:hidden}
.brow .fill{height:100%;border-radius:7px}
.brow .v{width:66px;text-align:right;color:var(--mute)}
.gbox{border:1px solid var(--line);background:var(--bg);border-radius:10px;padding:12px 14px;font-size:13px;line-height:1.5;margin-top:10px}
.gbox.warn{background:var(--warnbg);border-color:var(--warn);color:var(--panel)}
.gbox.okb{border-color:var(--ok)}
.gbox b{font-weight:600}
.svgwrap svg{display:block;width:100%}
.gnote{color:var(--mute);font-size:12px;margin-top:6px;line-height:1.5}
.frow{display:flex;justify-content:space-between;gap:10px;padding:7px 0;border-bottom:1px solid var(--line);font-size:13px}
.frow:last-child{border-bottom:0}
.frow .fk{color:var(--second);font-family:var(--cond)}
</style>
<script>/* wczesny odczyt motywu — zanim narysuje sie strona, zeby nie mrugalo jasnym */
try{if(localStorage.getItem('panelTheme')=='dark')document.documentElement.classList.add('dark')}catch(e){}</script>
<div class=w>
<div class=top><div class=topin>
 <div>
  <div class=title>Wyświetlacz łazienkowy</div>
  <div class=sub id=st>łączę…</div>
 </div>
 <div class=stat>
  <span class="pill pOn" id=onl>● łączę…</span>
  <span class=pill>FW <span id=fw>?</span> · <span id=stab>stabilna</span></span>
  <span class=pill>praca <span id=up>—</span></span>
  <span class="pill tog" id=thbtn onclick=toggleDark() title="Przełącz jasny/ciemny motyw">◐ Ciemny</span>
 </div>
</div></div>

<div class=layout>
<nav id=nav>
 <div class="navitem on" data-sec=ekran>Ekran</div>
 <div class=navitem data-sec=live>Na żywo</div>
 <div class=navitem data-sec=siec>Sieć</div>
 <div class=navitem data-sec=lokal>Lokalizacja</div>
 <div class=navitem data-sec=czuj>Czujniki <span class="nb b-sensors"></span></div>
 <div class=navitem data-sec=obec>Obecność</div>
 <div class=navitem data-sec=integr>Integracje <span class="nb b-token"></span></div>
 <div class=navitem data-sec=aktual>Aktualizacje</div>
 <div class=navitem data-sec=diag>Diagnostyka</div>
 <div class=navitem data-sec=zdrowie>Zdrowie</div>
</nav>

<div id=content>

<section id=sec-ekran class="active open">
<button class=sechead data-sec=ekran><span>Ekran</span><span class=hgrp><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Podgląd ekranu <span class=live id=live>● na żywo</span></h2>
  <div class=scr><img id=shot alt="wczytuję ekran…"></div>
  <div class=hint><label style="display:inline;text-transform:none;letter-spacing:0;font-family:inherit;color:var(--panel);margin:0">
   <input type=checkbox id=autoshot checked style="width:auto;margin-right:6px">Auto — odśwież co ~4 s</label>.
   Następna klatka pobiera się dopiero po załadowaniu poprzedniej (nie równolegle), żeby nie zalewać urządzenia.</div>
  <div class=row style=margin-top:10px>
   <a class=btnlink href="/api/screen" download="ekran.jpg">Pobierz zrzut JPEG</a>
   <button class=s style=margin:0 onclick="$('shot').src='/api/screen?'+Date.now()">Odśwież</button>
  </div>
  <div class=row style=margin-top:8px>
   <button class=s style=margin:0 onclick=tap(1)>Dotyk 1× (odśwież)</button>
   <button class=s style=margin:0 onclick=tap(2)>Dotyk 2× (poprzedni)</button>
  </div>
  <div class=hint>Działa jak dotknięcie płytki (pin GPIO7): 1× resetuje odliczanie, 2× cofa ekran.</div>
 </div>

 <div class=blk>
  <h2>Przypnij widok</h2>
  <div class=tabs id=tabs></div>
  <div class=hint id=vmsg>Klikaj, żeby przejść na dany ekran — urządzenie też się przełączy.</div>
  <label>Ekrany diagnostyczne</label>
  <div class="tabs sm" id=tabsd></div>
 </div>

 <div class=blk>
  <h2>Jasność podświetlenia</h2>
  <div class=tabs id=blpills>
   <button data-bl=auto onclick="setBl('auto')">Auto (czujnik)</button>
   <button data-bl=255 onclick="setBl(255)">100%</button>
   <button data-bl=130 onclick="setBl(130)">51%</button>
   <button data-bl=45 onclick="setBl(45)">18%</button>
  </div>
  <div class=hint>„Auto” oddaje sterowanie czujnikowi światła. Wymuszenie trzyma kilka godzin,
   potem samo wraca do automatu. Najniższa pozycja to 18% — ekranu nie da się zgasić.</div>
 </div>

 <div class=blk>
  <h2>Wygląd interfejsu</h2>
  <div class=tabs>
   <button id=thv3 onclick=setTheme(3)>V3 Pasmowy</button>
   <button id=thv1 onclick=setTheme(1)>V1 klasyczny</button>
   <button id=thv2 onclick=setTheme(2)>V2 retro</button>
  </div>
  <div class=hint id=thmsg>Zmiana działa od razu, bez restartu urządzenia.</div>
 </div>

 <div class=blk>
  <h2>Tryb nocny i jasność automatu</h2>
  <label>Tryb nocny — ekran główny zwija się do samego zegara (godziny 0–23)</label>
  <div class=row>
   <div><label>Początek</label><input id=nstart type=number min=0 max=23 step=1></div>
   <div><label>Koniec</label><input id=nend type=number min=0 max=23 step=1></div>
  </div>
  <label>Czas jednego ekranu w rotacji [s] (3–60)</label>
  <input id=dwell type=number min=3 max=60 step=1>
  <label><input type=checkbox id=arot style="width:auto;margin-right:8px"> Automatyczna rotacja ekranów (motyw „Pasmowy")</label>
  <div class=hint>Domyślnie wyłączona — ekrany przełącza dotyk. Po włączeniu widoki zmieniają się same co „czas jednego ekranu” (pole powyżej); dotyk pauzuje rotację, a po 60 s wraca ekran główny i cykl rusza dalej.</div>
  <label>Jasność automatu LDR (0–255)</label>
  <div class=row>
   <div><label>Dzień</label><input id=blday type=number min=60 max=255 step=1></div>
   <div><label>Półmrok</label><input id=bldim type=number min=30 max=255 step=1></div>
   <div><label>Noc</label><input id=blnight type=number min=15 max=255 step=1></div>
  </div>
  <button onclick=saveTune()>Zapisz ustawienia wyświetlacza</button>
  <div class=hint id=tunmsg>Zmiana działa od razu, bez restartu. Minimalna jasność jest wymuszona
   (dzień 60, półmrok 30, noc 15), żeby nie dało się zgasić ekranu na stałe — urządzenie
   wisi w łazience bez klawiatury.</div>
 </div>

 <div class=blk>
  <h2>Integracje — skrót</h2>
  <div class=intline><span class=il>Falownik</span><span class=sig id=sInv>—</span></div>
  <div class=intline><span class=il>Piec Viessmann</span><span class=rgt>
   <span class=sig id=sVi>—</span><button class=s onclick=goViAuth()>Odnów</button></span></div>
  <div class=intline><span class=il>MQTT</span><span class=sig id=sMqtt>—</span></div>
  <div class=hint>Pełne ustawienia tych integracji są w zakładce „Integracje”.</div>
 </div>
</div>
</section>

<section id=sec-live>
<button class=sechead data-sec=live><span>Na żywo</span><span class=hgrp><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Na żywo <span class=live id=liveDot>● odświeża co 5 s</span></h2>
  <div class=hint>Bieżące odczyty prosto z urządzenia. Auto-odświeżanie chodzi TYLKO gdy ta
   zakładka jest otwarta i karta przeglądarki widoczna — inaczej interwał się zatrzymuje,
   żeby nie zalewać urządzenia zapytaniami w tle.</div>
  <div class=gcards id=liveGrid></div>
  <h2 style=margin-top:14px>Pokoje (czujniki Bluetooth)</h2>
  <div id=liveRooms></div>
  <h2 style=margin-top:14px>Świeżość danych</h2>
  <div id=liveFresh></div>
 </div>
</div>
</section>

<section id=sec-siec>
<button class=sechead data-sec=siec><span>Sieć</span><span class=hgrp><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Sieć Wi-Fi</h2>
  <button class=s onclick=scan()>Wyszukaj sieci bezprzewodowe</button>
  <ul id=nets></ul>
  <label>Nazwa sieci (SSID)</label><input id=ssid autocapitalize=off autocorrect=off>
  <label>Hasło</label><input id=pass type=password autocapitalize=off autocorrect=off>
  <button onclick=saveWifi()>Zapisz i połącz</button>
  <div class=hint id=wmsg></div>
 </div>
</div>
</section>

<section id=sec-lokal>
<button class=sechead data-sec=lokal><span>Lokalizacja</span><span class=hgrp><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Lokalizacja pogody</h2>
  <div class=row><input id=q placeholder="np. Gdynia"><button class=s style="flex:0 0 110px;margin:0"
   onclick=geo()>Szukaj</button></div>
  <ul id=locs></ul>
  <div class=hint>Aktualnie: <b id=cur>—</b></div>
 </div>
</div>
</section>

<section id=sec-czuj>
<button class=sechead data-sec=czuj><span>Czujniki</span><span class=hgrp><span class="nb b-sensors"></span><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Czujniki Bluetooth</h2>
  <div class=hint>Xiaomi LYWSD03MMC. Fabryczny firmware szyfruje dane — wtedy potrzebny
   jest klucz (bindkey) z chmury Xiaomi. Klucz zostaje w pamięci urządzenia i nigdy
   nie opuszcza sieci domowej.</div>
  <ul id=bles></ul>
  <div class=hint id=bmsg></div>
 </div>
 <div class=blk>
  <h2>Bramki BLE</h2>
  <label>Bramki BLE — adresy (opcjonalnie)</label>
  <div id=gws></div>
  <button class=s onclick=saveGw()>Zapisz bramki</button>
  <div class=hint id=gmsg></div>
  <div class=hint>Bluetooth nie ma sieci kratowej — czujnik musi dosięgnąć odbiornika.
   Shelly stojący bliżej przekazuje ramki przez WiFi. Klucze zostają tutaj: bramka
   widzi wyłącznie szyfrogram.</div>
 </div>
</div>
</section>

<section id=sec-obec>
<button class=sechead data-sec=obec><span>Obecność · Światło · Ruch</span><span class=hgrp><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Obecność · Światło · Ruch</h2>
  <div class=hint id=obecSub>łazienka · dane z czujnika ruchu (PIR) i światła (LDR)</div>
  <div class=gcards id=obecCards></div>
  <button class=s onclick=obec()>Odśwież dane</button>
 </div>
 <div class=blk>
  <h2>Kiedy najczęściej ktoś jest w łazience</h2>
  <div class=svgwrap id=obecHourly></div>
 </div>
 <div class=blk>
  <h2>Możliwe zostawione światło</h2>
  <div id=obecLeft></div>
 </div>
 <div class=blk>
  <h2>Światło — jasno vs ciemno</h2>
  <div id=obecLight></div>
 </div>
 <div class=blk>
  <h2>Jak długo trwa ruch</h2>
  <div id=obecDur></div>
 </div>
 <div class=blk>
  <div class=gbox id=obecInsight></div>
  <div class=gnote id=obecFoot></div>
 </div>
</div>
</section>

<section id=sec-integr>
<button class=sechead data-sec=integr><span>Integracje</span><span class=hgrp><span class="nb b-token"></span><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Falownik i instalacja</h2>
  <label>Adres IP falownika (Modbus TCP)</label><input id=mb placeholder="adres z aplikacji FusionSolar">
  <label>Moc szczytowa instalacji [kWp]</label><input id=peak type=number step=0.1 placeholder=6.0>
  <button onclick=saveInv()>Zapisz</button>
  <div class=hint id=imsg></div>
 </div>
 <div class=blk>
  <h2>MQTT / Home Assistant</h2>
  <label><input type=checkbox id=mqen style="width:auto;margin-right:8px"> Publikuj dane na brokera MQTT</label>
  <label>Adres brokera</label><input id=mqhost placeholder="np. 192.168.1.10 albo homeassistant.local">
  <div class=row>
   <div><label>Port</label><input id=mqport type=number placeholder=1883></div>
   <div><label>Prefiks tematów</label><input id=mqpre placeholder=pogoda-gdynia></div>
  </div>
  <label>Użytkownik (opcjonalnie)</label><input id=mquser autocapitalize=off autocorrect=off>
  <label>Hasło (opcjonalnie)</label><input id=mqpass type=password autocapitalize=off autocorrect=off
   placeholder="(bez zmian)">
  <button onclick=saveMqtt()>Zapisz</button>
  <div class=hint id=qmsg></div>
  <div class=hint>Encje pojawią się w Home Assistancie same (MQTT Discovery) jako jedno
   urządzenie. Puste hasło = bez zmian; wpisz <b>-</b>, żeby je skasować.</div>
 </div>
 <div class=blk>
  <h2>Piec Viessmann</h2>
  <div class=hint>Twój Vitodens nie wystawia niczego w sieci lokalnej (sprawdzone: zero
   otwartych portów) — jedyna droga to chmura ViCare. Client ID weź z
   <a href="https://app.developer.viessmann-climatesolutions.com" target=_blank>portalu
   deweloperskiego</a>. Jest publiczny; token dostępu zostaje wyłącznie w pamięci urządzenia.</div>
  <label>Client ID</label><input id=vicid autocapitalize=off autocorrect=off
   placeholder="np. 962d...b35ce">
  <button class=s onclick=viLink()>1. Zapisz i wygeneruj link autoryzacyjny</button>
  <div class=hint id=vimsg></div>
  <div id=viauth style="display:none">
   <a id=vihref target=_blank style="font-weight:600">2. Otwórz i zaloguj się do Viessmann →</a>
   <div class=hint>Po zalogowaniu przeglądarka wróci tutaj sama i zapisze dostęp.
    Kod autoryzacyjny żyje 20 sekund, więc nie zwlekaj.</div>
  </div>
  <div class=hint id=vistat></div>
  <button class=s onclick=viForget()>Odłącz piec</button>
 </div>
 <div class=blk>
  <h2>Licznik gazu — kontrola pieca</h2>
  <div class=hint>Piec podaje własne zużycie, ale jego liczniki miesięczny i roczny
   są zepsute (sprawdzone: miesiąc pokazywał mniej niż ostatnie 7 dni, rok 5,3 m³ po
   czterech latach). Wiarygodna jest tylko doba — więc zbieramy ją sami, dzień po dniu.
   Wpisz stan licznika przy odczycie, a urządzenie porówna go z sumą z pieca za ten
   sam okres.</div>
  <div class=row>
   <input id=mdate type=date>
   <input id=mval type=number step=0.001 placeholder="stan licznika [m³]">
  </div>
  <button class=s onclick=addMeter()>Dodaj odczyt</button>
  <div class=hint id=mmsg></div>
  <ul id=meters></ul>
 </div>
</div>
</section>

<section id=sec-aktual>
<button class=sechead data-sec=aktual><span>Aktualizacje</span><span class=hgrp><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Aktualizacje</h2>
  <div class=hint>Urządzenie samo sprawdza GitHub co 15 minut.</div>
  <button class=s onclick=upd()>Sprawdź teraz</button>
  <div class=hint id=umsg></div>
 </div>
</div>
</section>

<section id=sec-diag>
<button class=sechead data-sec=diag><span>Diagnostyka</span><span class=hgrp><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Stan i awarie — po ludzku</h2>
  <div class=hint>Zwięzłe podsumowanie bez znajomości kodu: restarty po awarii, ostatnia
   awaria (jeśli była) oraz łączność z falownikiem i piecem.</div>
  <button class=s onclick=fails()>Odczytaj stan</button>
  <div id=failout style=margin-top:10px></div>
 </div>
 <div class=blk>
  <h2>Diagnostyka</h2>
  <div class=row>
   <button class=s style=margin:0 onclick=dg()>Stan urządzenia</button>
   <button class=s style=margin:0 onclick=lg()>Logi</button>
   <button class=s style=margin:0 onclick=cd()>Zrzut awaryjny</button>
  </div>
  <pre id=dbg style="white-space:pre-wrap;font:12px ui-monospace,monospace;color:var(--second);
   background:var(--code);border:1px solid var(--line);border-radius:8px;padding:10px;margin-top:10px;
   max-height:300px;overflow:auto"></pre>
  <div id=cdact style=display:none>
   <div class="hint err">Surowy zrzut to kopia pamięci urządzenia — mogą w nim być hasła Wi-Fi
    i MQTT, bindkeye BLE oraz token Viessmanna. Nie wklejaj go do zgłoszeń błędów ani na
    GitHuba. Zadanie, adres upadku i backtrace widać wyżej i to jest bezpieczne do wklejenia.</div>
   <div class=row>
    <button class=s style=margin:0 onclick=cdget()>Pobierz surowy zrzut</button>
    <button class=s style=margin:0 onclick=cddel()>Skasuj zrzut</button>
   </div>
  </div>
 </div>
 <div class=blk>
  <h2>Pamięć urządzenia</h2>
  <div class=hint>Wszystkie rodzaje pamięci: pojemność, zajętość, wolne, bufory i podział na partycje OTA.</div>
  <button class=s onclick=mem()>Odczytaj stan pamięci</button>
  <div id=memout style=margin-top:10px></div>
 </div>
 <div class=blk>
  <h2>Radar opadów — symulacja</h2>
  <div class=hint>Ekran radaru pojawia się w rotacji tylko wtedy, gdy realnie pada.
   Symulacja pokazuje sztuczny front — do obejrzenia, jak wygląda wizualizacja.</div>
  <div class=row>
   <button class=s style=margin:0 onclick="demo(1)">Włącz symulację</button>
   <button class=s style=margin:0 onclick="demo(0)">Wyłącz</button>
  </div>
  <div class=hint id=dmsg></div>
 </div>
 <div class=blk>
  <h2>Urządzenie</h2>
  <button class=s onclick=rb()>Restartuj urządzenie</button>
  <button class=s onclick=fgt()>Zapomnij sieć Wi-Fi</button>
 </div>
</div>
</section>

<section id=sec-zdrowie>
<button class=sechead data-sec=zdrowie><span>Zdrowie urządzenia</span><span class=hgrp><span class=chev>▸</span></span></button>
<div class=secbody>
 <div class=blk>
  <h2>Zdrowie urządzenia</h2>
  <div class=hint>Stabilność, pamięć i łączność — na podstawie /api/diag. Odczyt „na klik”,
   bez odpytywania w tle.</div>
  <button class=s onclick=health()>Odczytaj stan</button>
  <div id=healthOut style=margin-top:10px></div>
 </div>
</div>
</section>

</div>
</div>
</div><script>
const $=i=>document.getElementById(i);
// V3: jawna mapa etykieta -> indeks widoku (cfg::VIEW_*). Bez "i-1" — indeksy sa dokladnie
// te, ktore przyjmuje pinView() (WeatherUi.cpp). RETRO=0 i HOURS=2 nie istnieja w V3, wiec
// ich tu nie ma. Ekrany diagnostyczne to osobna, mniejsza grupa pigulek.
const VIEWS=[['Auto',-1],['Główny',1],['Radar',3],['5 dni',4],['Prąd',7],['Pokoje',5],['Ogrzewanie',6],['Powietrze',9],['Samoloty',8]];
// Kolejnosc jak numeracja na wyswietlaczu: para dotykowa Statystyki(1/2, VIEW_STATS=12)
// -> Pamiec(2/2, VIEW_MEM=10), a potem osobny Ruch(VIEW_MOTION=11).
const VDIAG=[['Statystyki',12],['Pamięć',10],['Ruch',11]];
let live=true,pin=-1,theme=3;

function bset(cls,txt){document.querySelectorAll('.'+cls).forEach(e=>e.textContent=txt);}
// Uptime czytelnie takze przy krotkiej pracy: <1 h -> "X min", <doba -> "X h Y min",
// dalej "X d Y h". Wczesniej pokazywalo "0 d 0 h" przez pierwsza godzine po restarcie.
function fmtUptime(u){const dd=Math.floor(u/86400),hh=Math.floor(u%86400/3600),mm=Math.floor(u%3600/60);
 if(u<3600)return mm+' min';
 if(u<86400)return hh+' h '+mm+' min';
 return dd+' d '+hh+' h';}
// "ile temu" po ludzku (sekundy -> tekst). -1/undefined = nigdy.
function agoTxt(s){if(s==null||s<0)return 'nigdy';if(s<60)return 'przed chwilą';
 if(s<3600)return Math.floor(s/60)+' min temu';if(s<86400)return Math.floor(s/3600)+' h temu';
 return Math.floor(s/86400)+' d temu';}
// polska mnogosc dla "raz/razy": 1 -> "raz", reszta -> "razy" (0,2,5,22... = razy).
function razy(n){return n===1?'raz':'razy';}

function tabs(){
 $('tabs').innerHTML=VIEWS.map(([n,i])=>
  `<button class="${i===pin?'on':''}" onclick="pickView(${i})">${n}</button>`).join('');
 $('tabsd').innerHTML=VDIAG.map(([n,i])=>
  `<button class="${i===pin?'on':''}" onclick="pickView(${i})">${n}</button>`).join('');
}
async function pickView(i){
 pin=i;tabs();
 const nm=[...VIEWS,...VDIAG].find(v=>v[1]===i);
 $('vmsg').textContent=i<0?'Rotacja automatyczna — dokładnie jak na urządzeniu.'
  :('Zatrzymane na ekranie: '+(nm?nm[0]:i)+'. Kliknij „Auto”, żeby wznowić rotację.');
 try{const r=await(await fetch('/api/view?i='+i)).json();pin=r.pin;tabs();}catch(e){}
}
function themeUI(){
 $('thv1').className=theme===1?'on':'';
 $('thv2').className=theme===2?'on':'';
 $('thv3').className=theme===3?'on':'';
}
async function setTheme(v){
 $('thmsg').textContent='Zmieniam…';
 try{
  const r=await(await fetch('/api/theme?v='+v,{method:'POST'})).json();
  theme=r.theme;
  $('thmsg').className='hint '+(r.ok?'ok':'err');
  $('thmsg').textContent=r.ok?'Zapisano — ekran przełączy się od razu.':'Nie udało się zapisać.';
 }catch(e){$('thmsg').className='hint err';$('thmsg').textContent='Błąd połączenia';}
 themeUI();
}
async function saveTune(){
 $('tunmsg').className='hint';$('tunmsg').textContent='Zapisuję…';
 const q='nightStart='+(+$('nstart').value)+'&nightEnd='+(+$('nend').value)
  +'&dwell='+(+$('dwell').value)+'&blDay='+(+$('blday').value)
  +'&blDim='+(+$('bldim').value)+'&blNight='+(+$('blnight').value)
  +'&arot='+($('arot').checked?1:0);
 try{
  const r=await(await fetch('/api/tuning?'+q,{method:'POST'})).json();
  // Serwer clampuje — pokazujemy realnie zapisane wartosci (np. jasnosc podbita do minimum).
  $('nstart').value=r.night_start;$('nend').value=r.night_end;$('dwell').value=r.dwell;
  $('blday').value=r.bl_day;$('bldim').value=r.bl_dim;$('blnight').value=r.bl_night;
  $('arot').checked=!!r.arot;
  $('tunmsg').className='hint '+(r.ok?'ok':'err');
  $('tunmsg').textContent=r.ok?'Zapisano — działa od razu (jasność mogła zostać podbita do minimum).'
   :'Nie udało się zapisać.';
 }catch(e){$('tunmsg').className='hint err';$('tunmsg').textContent='Błąd połączenia';}
}
async function tap(n){try{await fetch('/api/tap?n='+n,{method:'POST'});}catch(e){}}
// --- JASNOSC: pigulki forsowania podswietlenia (v=auto zwalnia; 255/130/45 forsuja) ---
function highlightBl(v){document.querySelectorAll('#blpills button').forEach(b=>b.classList.toggle('on',b.dataset.bl===String(v)));}
async function setBl(v){
 highlightBl(v);
 try{
  if(v==='auto')await fetch('/api/bl?v=auto');            // zwolnij forsowanie -> automat LDR
  else await fetch('/api/bl?v='+v+'&ms=14400000');        // ~4 h override, sam wygasa
 }catch(e){}
}
// --- NAWIGACJA: komputer = pokaz jedna sekcje; telefon = rozwin/zwin akordeon ---
function showSection(s){
 document.querySelectorAll('section').forEach(x=>x.classList.toggle('active',x.id==='sec-'+s));
 document.querySelectorAll('.navitem').forEach(x=>x.classList.toggle('on',x.dataset.sec===s));
 onShown(s);
}
function toggleSection(s){
 const open=$('sec-'+s).classList.toggle('open');
 if(open)onShown(s); else liveSync();
}
function esc(t){return String(t==null?'':t).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
// Lazy: ciezsze sekcje (Obecnosc, Zdrowie) laduja sie przy PIERWSZYM pokazaniu, nie na
// starcie — zeby nie odpalac kilku /api/diag naraz przy wejsciu na panel.
let obecLoaded=false,healthLoaded=false;
function onShown(s){
 liveSync();
 if(typeof shotDot==='function')shotDot();   // podglad wznawia/pauzuje z widocznoscia sekcji Ekran
 if(s==='obec'&&!obecLoaded){obecLoaded=true;obec();}
 if(s==='zdrowie'&&!healthLoaded){healthLoaded=true;health();}
}
// --- CIEMNY MOTYW: klasa .dark na <html>, wybor w localStorage (wczesny skrypt u gory
// juz go zastosowal — tu tylko napis przycisku i przelaczanie na klik). ---
function applyDark(on){
 document.documentElement.classList.toggle('dark',on);
 if($('thbtn'))$('thbtn').textContent=on?'○ Jasny':'◐ Ciemny';
}
function toggleDark(){
 const on=!document.documentElement.classList.contains('dark');
 applyDark(on);
 try{localStorage.setItem('panelTheme',on?'dark':'light');}catch(e){}
}
// --- NA ZYWO: auto-odswiezanie co 5 s, ale TYLKO gdy sekcja widoczna (komputer: .active,
// telefon: .open) i karta na wierzchu. Interwal startuje/gasnie w liveSync() — wzorem
// screenshotu i diagPills, zeby NIE zalewac urzadzenia zapytaniami w tle. ---
let liveTimer=null;
function liveVisible(){
 const el=$('sec-live');
 return el&&!document.hidden&&(el.classList.contains('active')||el.classList.contains('open'));
}
function liveSync(){
 if(liveVisible()){if(!liveTimer){liveTick();liveTimer=setInterval(liveTick,5000);}}
 else if(liveTimer){clearInterval(liveTimer);liveTimer=null;}
}
function lightWord(mv){return mv==null?'—':(mv<256?'ciemno':(mv<640?'zmierzch':'jasno'));}
async function liveTick(){
 let d,b=[];
 try{d=await(await fetch('/api/diag?'+Date.now())).json();}catch(e){return;}
 try{b=await(await fetch('/api/ble')).json();}catch(e){}
 const s=d.sensors||{},pv=d.pv||{};
 const motion=s.pir?'● TERAZ':agoTxt(s.pir_last_s);
 let inv='—',invC='';
 if(pv.asleep){inv='uśpiony (noc)';invC='hot';}
 else if(pv.ok_ago_s>=0&&pv.ok_ago_s<1800){inv='połączony';}
 else{inv='brak łączności';invC='bad';}
 // "Prad" (moc chwilowa) NIE jest w /api/diag — pokazujemy stan lacznosci falownika.
 const cards=[
  ['Ruch teraz',motion,'',s.pir?'hot':''],
  ['Światło teraz',(s.ldr_mv!=null?s.ldr_mv:'—'),' mV · '+lightWord(s.ldr_mv),''],
  ['Falownik',inv,'',invC],
  ['Praca (uptime)',fmtUptime(d.uptime_s||0),'','']
 ];
 $('liveGrid').innerHTML=cards.map(c=>`<div class="gcard ${c[3]}"><div class=lbl>${c[0]}</div><div class=val>${c[1]}<small>${c[2]}</small></div></div>`).join('');
 const named=(b||[]).filter(x=>x.name);
 $('liveRooms').innerHTML=named.length?named.map(x=>{
  const stt=x.valid?`${x.t.toFixed(1)}°C · ${x.h.toFixed(0)}%`:(x.needsKey?'brak klucza':'czekam');
  const gw=x.gw?' · przez bramkę':'';
  return `<div class=frow><span class=fk>${esc(x.name)}</span><span>${stt} <span style="color:var(--mute)">(${agoTxt(x.age_s)}${gw})</span></span></div>`;
 }).join(''):'<div class=gnote>Brak nazwanych czujników (nadaj nazwy w zakładce „Czujniki”).</div>';
 const fresh=[['Pogoda',d.weather&&d.weather.ok_ago_s],['Falownik',pv.ok_ago_s],
  ['Piec Viessmann',d.vi&&d.vi.ok_ago_s],['Powietrze',d.air&&d.air.ok_ago_s],
  ['Radar opadów',d.radar&&d.radar.ok_ago_s],['Samoloty',d.flights&&d.flights.ok_ago_s],
  ['MQTT',d.mqtt&&d.mqtt.ok_ago_s]];
 $('liveFresh').innerHTML=fresh.map(f=>{const v=(f[1]==null?-1:f[1]);
  const c=v<0?'var(--mute)':(v<900?'var(--ok)':'var(--warn)');
  return `<div class=frow><span class=fk>${f[0]}</span><span style="color:${c}">${agoTxt(v)}</span></div>`;}).join('');
 if($('liveDot'))$('liveDot').textContent='● odświeżono '+new Date().toLocaleTimeString('pl-PL');
}
// --- OBECNOSC · SWIATLO · RUCH (integracja prototypu): logika i wyglad z prototypu, dane
// na zywo z /api/diag (d.sensors). Wykresy INLINE (svg + divy), zero bibliotek z CDN.
// Liczby "na dobe" dzielimy przez collected_s/86400, NIGDY przez uptime. ---
async function obec(){
 let d;
 try{d=await(await fetch('/api/diag?'+Date.now())).json();}catch(e){$('obecCards').innerHTML='<div class="hint err">Nie udało się pobrać danych.</div>';return;}
 const s=d.sensors||{};
 const byh=s.pir_by_hour||[],wid=s.pir_width_ms||[],gap=s.pir_gap_ms||[],hist=s.ldr_hist||[];
 const collected=(s.pir_meas&&s.pir_meas.collected_s)||0;
 const days=collected/86400, perDay=n=>days>0?n/days:0;
 const pl=n=>n.toFixed(1).replace('.',','), pl2=n=>n.toFixed(2).replace('.',',');
 const busiest=byh.length?byh.indexOf(Math.max(...byh)):-1;
 const longGaps=(gap[5]||0)+(gap[6]||0);            // przerwy >=5 min ~ granice wizyt
 const visitsDay=Math.round(perDay(longGaps));
 const ldrSec=hist.map(x=>x/4);                     // 4 probki/s => sekundy
 const bright=ldrSec.slice(9).reduce((a,b)=>a+b,0); // kosze >=640 mV = jasno
 const totL=ldrSec.reduce((a,b)=>a+b,0)||1;
 const dark=ldrSec.slice(0,3).reduce((a,b)=>a+b,0); // kosze <32 mV = ciemno
 const mid=totL-dark-bright, brightHDay=bright/(days||1)/3600;
 const motMinDay=Math.round(perDay(s.pir_total_s||0)/60);
 const cards=[
  ['Wejść / dobę',days>0?'~'+visitsDay:'—',''],
  ['Ruch aktywny',days>0?motMinDay:'—',' min/dobę'],
  ['Najruchliwsza godz.',busiest>=0?(String(busiest).padStart(2,'0')+':00'):'—',''],
  ['Światło jasne',days>0?pl(brightHDay):'—',' h/dobę']];
 $('obecCards').innerHTML=cards.map(c=>`<div class=gcard><div class=lbl>${c[0]}</div><div class=val>${c[1]}<small>${c[2]}</small></div></div>`).join('');
 $('obecSub').textContent=`łazienka · okno obserwacji ${days>0?pl2(days)+' doby':'zbiera się…'} · ${s.pir_pulses||0} impulsów ruchu (PIR + LDR)`;
 // wykres godzinowy (inline SVG); szczyty = 5 najwyzszych godzin
 const W=820,H=170,padL=8,padR=8,padB=24,padT=8,bw=(W-padL-padR)/24,maxv=Math.max(...byh,1);
 const top=byh.map((v,i)=>[v,i]).sort((a,b)=>b[0]-a[0]).slice(0,5).filter(x=>x[0]>0).map(x=>x[1]);
 let bars='';byh.forEach((v,hh)=>{const bh=(H-padB-padT)*(v/maxv),x=padL+hh*bw,y=H-padB-bh,hot=top.includes(hh);
  bars+=`<rect x="${(x+2).toFixed(1)}" y="${y.toFixed(1)}" width="${(bw-4).toFixed(1)}" height="${bh.toFixed(1)}" rx="2" fill="${hot?'var(--accent)':'#9DB8DD'}"><title>${String(hh).padStart(2,'0')}:00 — ${Math.round(perDay(v))} imp./dobę</title></rect>`;});
 let ticks='';[0,6,12,18,23].forEach(hh=>{ticks+=`<text x="${(padL+hh*bw+bw/2).toFixed(1)}" y="${H-7}" font-size="11" fill="var(--mute)" text-anchor="middle">${hh}</text>`;});
 $('obecHourly').innerHTML=byh.length?`<svg viewBox="0 0 ${W} ${H}" role=img aria-label="Obecność wg godzin"><line x1="${padL}" y1="${H-padB}" x2="${W-padR}" y2="${H-padB}" stroke="var(--line)"/>${bars}${ticks}</svg><div class=gnote>godzina doby · podświetlone słupki to szczyty obecności</div>`:'<div class=gnote>Brak danych godzinowych (czekają na zegar NTP).</div>';
 // swiatlo ciemno/zmierzch/jasno
 const pct=x=>Math.round(x/totL*100);
 const brow=(k,v,frac,col)=>`<div class=brow><span class=k>${k}</span><span class=track><span class=fill style="width:${(frac*100).toFixed(0)}%;background:${col}"></span></span><span class=v>${v}</span></div>`;
 $('obecLight').innerHTML=brow('Ciemno',pct(dark)+'%',dark/totL,'var(--second)')+brow('Zmierzch',pct(mid)+'%',mid/totL,'#B8C6DD')+brow('Jasno',pct(bright)+'%',bright/totL,'var(--warn)')+`<div class=gnote>„Jasno” to poziom LDR ≥ ~640 mV (światło włączone albo dzień przez okno). Ok. ${pl(brightHDay)} h/dobę.</div>`;
 // dlugosc ruchu
 const names=['<0,1 s','0,1–1 s','1–3 s','3–10 s','10–60 s','>60 s'];
 const wtot=wid.reduce((a,b)=>a+b,0)||1,wmax=Math.max(...wid,1);
 let drows='';wid.forEach((v,i)=>{if(v)drows+=`<div class=brow><span class=k>${names[i]}</span><span class=track><span class=fill style="width:${(v/wmax*100).toFixed(0)}%;background:var(--warn)"></span></span><span class=v>${Math.round(v/wtot*100)}%</span></div>`;});
 $('obecDur').innerHTML=(drows||'<div class=gnote>Brak zakończonych impulsów.</div>')+`<div class=gnote>Krótkie impulsy 1–3 s to ktoś w ruchu; dłuższe (3–10 s) to zwykle wchodzenie/wychodzenie.</div>`;
 // DETEKTOR "zostawione swiatlo" (task B) — SZACUNEK z histogramow, nie zdarzenia w czasie.
 // brightShare = udzial czasu "jasno"; presenceShare = udzial godzin z JAKIMKOLWIEK ruchem
 // (gorna granica obecnosci — konserwatywnie, zeby nie alarmowac na wyrost).
 const brightShare=bright/totL, hoursWithMotion=byh.filter(v=>v>0).length, presenceShare=hoursWithMotion/24;
 const flag=brightShare>presenceShare+0.15 && brightShare>presenceShare*1.4;
 if(collected<3600){
  $('obecLeft').innerHTML='<div class=gbox>Za mało danych — okno obserwacji dopiero się zbiera.</div>';
 }else if(flag){
  $('obecLeft').innerHTML=`<div class="gbox warn"><b>⚠ Możliwe zostawione światło.</b> Jasno jest ~${pl(brightHDay)} h/dobę (${Math.round(brightShare*100)}% czasu), a ruch wykrywany tylko w ~${hoursWithMotion} godz. doby (${Math.round(presenceShare*100)}%). Skoro „jasno” trwa wyraźnie dłużej niż obecność, część tego czasu to prawdopodobnie światło zostawione po wyjściu — albo dzień przez okno (czujnik LDR tego nie rozróżnia). <b>To szacunek z histogramów LDR/PIR, a nie zdarzenia w czasie rzeczywistym.</b></div>`;
 }else{
  $('obecLeft').innerHTML=`<div class="gbox okb">Udział czasu „jasno” (${Math.round(brightShare*100)}%) nie przewyższa wyraźnie obecności (~${hoursWithMotion} godz./dobę) — brak sygnału zostawionego światła. <span style="color:var(--mute)">Szacunek z histogramów, nie zdarzenia w czasie rzeczywistym.</span></div>`;
 }
 $('obecInsight').innerHTML=days>0?`<b>Co z tego wynika:</b> obecność koncentruje się ${busiest>=0?('wokół <b>'+String(busiest).padStart(2,'0')+':00</b>'):'w kilku porach doby'}. Dzienny ruch to ok. <b>${motMinDay} min</b> aktywnego czujnika w ~<b>${visitsDay} wejściach</b>. Światło jest jasne ~<b>${pl(brightHDay)} h/dobę</b> — warto sprawdzić, czy te godziny pokrywają się z obecnością.`:'Dane się zbierają — wróć po kilku godzinach.';
 $('obecFoot').textContent='Dane na żywo z /api/diag (sensory PIR + LDR, liczniki przeżywają restart/OTA). Liczby „na dobę” liczone z okna obserwacji (collected_s), nie z uptime.';
}
// --- ZDROWIE URZADZENIA (task C): uptime, awarie, sterta (ostrzezenie gdy najwiekszy blok
// < 40 kB), radar/PSRAM, WiFi — wszystko z /api/diag. ---
async function health(){
 const o=$('healthOut');o.innerHTML='<div class=hint>Odczytuję…</div>';
 let d;
 try{d=await(await fetch('/api/diag?'+Date.now())).json();}catch(e){o.innerHTML='<div class="hint err">Nie udało się odczytać.</div>';return;}
 let h='';
 const rs=d.reset||{},cr=rs.crashes_total||0;
 h+=fblock('Stabilność',
  (cr>0?('Restartów po awarii (panice): <b>'+cr+'</b> od początku pomiaru. '):'Ani jednego restartu po awarii. ')
   +'Ostatni restart: '+esc(rs.reason||'?')+(rs.was_crash?' — to była awaria.':' — normalny (nie awaria).')
   +' Praca bez przerwy: <b>'+fmtUptime(d.uptime_s||0)+'</b>.',
  rs.was_crash?'err':(cr>0?'warn':'ok'));
 const hb=d.heap_largest_block||0,hf=d.heap_free||0,hm=d.heap_min_ever||0,lowBlock=hb>0&&hb<40960,total=327680;
 h+='<div style="padding:8px 0;border-bottom:1px solid var(--line)">'
  +'<b style=color:var(--panel)>Pamięć robocza (sterta SRAM)</b>'+memBar(total-hf,total)
  +'<div style="font:12px system-ui;color:var(--second)">wolne <b>'+kb(hf)+'</b> · dołek od startu '+kb(hm)+' · największy ciągły blok <b style="color:'+(lowBlock?'var(--err)':'var(--panel)')+'">'+kb(hb)+'</b></div>'
  +'<div style="font:11px system-ui;color:var(--mute);margin-top:2px">TLS przy aktualizacji potrzebuje ~40 kB ciągłego bloku. Dołek pokazuje, jak blisko granicy bywało.</div>'
  +(lowBlock?'<div class="gbox warn" style="margin-top:8px"><b>⚠ Największy ciągły blok poniżej 40 kB.</b> Aktualizacja po sieci (TLS) może się nie udać przy tak pofragmentowanej stercie — jeśli to się utrzymuje, zrestartuj urządzenie przed OTA.</div>':'')
  +'</div>';
 const rr=d.radar||{},psram=(d.psram!=null?d.psram:(d.memfull&&d.memfull.psram&&d.memfull.psram.total));
 const hasPsram=psram>0;
 h+=fblock('Mapa radaru opadów',
  (hasPsram?('Mapa radaru: <b>OK</b> — bufory klatek w PSRAM ('+kb(psram)+').')
           :'<b>Pomiar punktowy (brak PSRAM przy starcie).</b> Animowana mapa niedostępna, radar w trybie awaryjnym.')
   +' Pominięte klatki przy niskim RAM: <b>'+(rr.skips_low_ram||0)+'</b>.'
   +(rr.err?(' Ostatni błąd: '+esc(rr.err)+'.'):''),
  hasPsram?((rr.skips_low_ram||0)>0?'warn':'ok'):'warn');
 const w=d.wifi||{};
 h+=fblock('Wi-Fi',
  'Sieć <b>'+esc(w.ssid||'?')+'</b> · sygnał <b>'+(w.rssi!=null?w.rssi+' dBm':'?')+'</b>'
   +' · przełączeń AP (roaming): <b>'+(w.roams||0)+'</b> · połączeń od startu: <b>'+(w.connects||0)+'</b>'
   +(w.channel?(' · kanał '+w.channel):''),
  (w.rssi!=null&&w.rssi>-75)?'ok':'warn');
 o.innerHTML=h;
}
// --- PASEK STATUSU z /api/diag: praca (uptime) + stabilnosc + skrot falownika ---
async function diagPills(){
 try{
  const d=await(await fetch('/api/diag?'+Date.now())).json();
  $('up').textContent=fmtUptime(d.uptime_s||0);
  if(d.reset)$('stab').textContent=d.reset.was_crash?'po awarii':'stabilna';
  if(d.pv){
   const p=d.pv;let t='❌ brak łączności',c='err';
   if(p.asleep){t='uśpiony (noc)';c='warn';}
   else if(p.ok_ago_s>=0&&p.ok_ago_s<1800){t='✓ połączony';c='ok';}
   $('sInv').textContent=t;$('sInv').className='sig '+c;
  }
 }catch(e){}
}
function kb(b){if(b==null)return '—';if(b>=1048576)return (b/1048576).toFixed(2)+' MB';if(b>=1024)return (b/1024).toFixed(1)+' kB';return b+' B';}
function memBar(used,total){var p=total>0?Math.round(used/total*100):0;
 return '<div style="height:8px;background:var(--track);border-radius:4px;overflow:hidden;margin:3px 0">'
 +'<div style="height:8px;width:'+p+'%;background:'+(p>85?'var(--err)':p>65?'var(--warn)':'var(--ok)')+'"></div></div>';}
function memRow(name,cap,used,free,buf,desc){
 var body='';
 if(cap!=null)body+='<b>'+kb(cap)+'</b> pojemność';
 if(used!=null)body+=' · zajęte '+kb(used);
 if(free!=null)body+=' · <b style=color:var(--ok)>wolne '+kb(free)+'</b>';
 if(buf!=null)body+=' · '+buf;
 var bar=(cap!=null&&used!=null)?memBar(used,cap):'';
 return '<div style="padding:8px 0;border-bottom:1px solid var(--line)">'
  +'<div style="display:flex;justify-content:space-between"><b style=color:var(--panel)>'+name+'</b></div>'
  +bar+'<div style="font:12px system-ui;color:var(--second)">'+body+'</div>'
  +'<div style="font:11px system-ui;color:var(--mute);margin-top:2px">'+desc+'</div></div>';}
async function mem(){
 var o=$('memout');o.innerHTML='<div class=hint>Odczytuję…</div>';
 try{
  var d=await(await fetch('/api/diag?'+Date.now())).json();
  var m=d.memfull||{},h='';
  var s=m.sram||{};
  h+=memRow('SRAM (pamięć robocza)',327680,327680-(s.free||0),s.free,
    'największy blok '+kb(s.largest_block)+' · dołek od startu '+kb(s.min_ever),
    'Wewnętrzna pamięć układu. Tu żyje sterta: bufory sieciowe, TLS przy aktualizacji, stosy zadań. Największy blok pokazuje fragmentację — TLS potrzebuje ~40 kB ciągłego.');
  var ps=m.psram||{};
  h+=memRow('PSRAM (pamięć zewnętrzna)',ps.total,ps.total-(ps.free||0),ps.free,
    'największy blok '+kb(ps.largest_block),
    'Zewnętrzne 2 MB. Trzyma bufor ekranu (~132 kB) i 13 klatek radaru (~715 kB). Duże bufory graficzne idą tutaj, nie do SRAM.');
  // --- FLASH (uczciwie): 4 MB ukladu to NIE budzet firmware'u. Poprzedni pasek pokazywal
  // "Flash 4 MB - wolne 2,29" (chip - sketch) ORAZ "app1 1,94 - wolne 233 kB" i te dwie liczby
  // sobie przeczyly. PRAWDA: firmware mieszci sie w JEDNEJ partycji app (1,94 MB), a te
  // "2,29 MB wolne" to glownie DRUGA partycja app (rezerwa OTA), w ktora NIE da sie dopisac
  // firmware'u. Rozbijamy wiec 4 MB na: aktywna aplikacja / kopia OTA / system.
  var fl=m.flash||{}, ap=m.app||{}, prt=m.partitions||[];
  var appUsed=(ap.used!=null?ap.used:fl.sketch_size), appSize=ap.size||0;
  var appFree=(appSize&&appUsed!=null)?appSize-appUsed:null;
  var running=ap.running||'?';
  // Kopia OTA = druga partycja app (ta, ktora NIE jest uruchomiona).
  var otaName=(running=='app0')?'app1':'app0';
  var otaP=prt.find(function(p){return p.name==otaName;})||{};
  // System: nvs+otadata+coredump(+spiffs jesli obecny) z tablicy partycji + bootloader
  // i tablica partycji (~36 kB, lezace przed nvs pod 0x0..0x9000 — nie ma ich w liscie).
  var sysSum=36864;
  prt.forEach(function(p){if(p.present&&(p.name=='nvs'||p.name=='otadata'||p.name=='coredump'||p.name=='spiffs'))sysSum+=(p.size||0);});
  h+='<div style="padding:8px 0 2px"><b style=color:var(--panel)>Flash (pamięć programu) — układ '+kb(fl.chip_size)+'</b>'
   +'<div style="font:11px system-ui;color:var(--mute);margin-top:2px">Cały układ flash. Firmware NIE dostaje go w całości — poniżej uczciwy podział.</div></div>';
  h+=memRow('Aktywna aplikacja („'+running+'”) — realny budżet firmware',appSize,appUsed,appFree,null,
    'Tu działa bieżący firmware. Nowa wersja musi zmieścić się w TEJ jednej partycji, więc wolne '+kb(appFree)+' to cały zapas na rozrost (nowe fonty, ekrany). To jest liczba, na którą trzeba patrzeć.');
  h+=memRow('Kopia OTA („'+otaName+'”) — rezerwa aktualizacji',otaP.size,null,null,null,
    'Rezerwa na aktualizację po sieci: nowy obraz wgrywa się tutaj, a stary zostaje jako powrót awaryjny. NIE da się w to dopisać bieżącej aplikacji — to nie jest wolne miejsce dla firmware.');
  h+=memRow('System (NVS, otadata, zrzut, bootloader)',sysSum,null,null,null,
    'Ustawienia trwałe, wskaźnik OTA, zrzut awaryjny oraz bootloader z tablicą partycji. Stały narzut, poza budżetem aplikacji.');
  h+='<div style="font:11px system-ui;color:var(--mute);margin:4px 0 8px;line-height:1.5">Układ ma '+kb(fl.chip_size)+', ale dwie równe partycje app („app0”/„app1”, po '+kb(appSize)+') to WARUNEK bezpiecznej aktualizacji po sieci — jedna trzyma wersję działającą, druga przyjmuje nową i pozwala wrócić, gdy coś pójdzie nie tak. Dlatego realny zapas to '+kb(appFree)+' w aktywnej partycji, a nie „wolne '+kb((fl.chip_size&&fl.sketch_size)?fl.chip_size-fl.sketch_size:0)+'” z całego układu.</div>';
  // partycje
  if(m.partitions){
   var descP={nvs:'Ustawienia trwałe: Wi-Fi, MQTT, klucze BLE, token pieca, wybrany wygląd.',
    otadata:'Wskaźnik, z której partycji OTA startować i czy poprzednia aktualizacja się udała.',
    app0:'Partycja aplikacji OTA #0.',app1:'Partycja aplikacji OTA #1.',
    spiffs:'Zarezerwowana — projekt NIE używa systemu plików (odzyskane miejsce oddano partycjom app).',
    coredump:'Zrzut awaryjny po panice: zadanie, adres upadku, backtrace.'};
   var ph='<div style="padding:8px 0"><b style=color:var(--panel)>Podział na partycje (OTA i dane)</b>'
    +'<table style="width:100%;border-collapse:collapse;font:12px system-ui;color:var(--second);margin-top:4px">'
    +'<tr style=color:var(--mute)><td>nazwa</td><td>adres</td><td style=text-align:right>rozmiar</td></tr>';
   m.partitions.forEach(function(p){if(!p.present)return;
    ph+='<tr><td>'+p.name+'</td><td>0x'+(p.address||0).toString(16)+'</td><td style=text-align:right>'+kb(p.size)+'</td></tr>';});
   ph+='</table><div style="font:11px system-ui;color:var(--mute);margin-top:4px">Dwie równe partycje app0/app1 (po '
    +kb((m.partitions.find(function(p){return p.name=="app0"})||{}).size)+') to serce OTA: nowa wersja wgrywa się na wolną, a stara zostaje jako powrót awaryjny.</div></div>';
   h+=ph;
  }
  var rt=m.rtc||{};
  h+=memRow('RTC SLOW (przeżywa restart)',rt.slow_usable,rt.slow_used,(rt.slow_usable&&rt.slow_used)?rt.slow_usable-rt.slow_used:null,
    'fizycznie '+kb(rt.slow_physical),
    'Maleńka pamięć, którą NIE kasuje restart ani aktualizacja (ale kasuje zanik zasilania). Trzyma statystyki ruchu (PIR) i światła (LDR) zbierane tygodniami.');
  var st=m.stack||{};
  h+=memRow('Stosy zadań (sieć / web)',st.configured_size,null,null,
    'zapas: sieć '+kb(st.net_spare)+', web '+kb(st.web_spare),
    'Każde zadanie ma swój stos '+kb(st.configured_size)+'. „Zapas” to ile nigdy nie zostało użyte — margines bezpieczeństwa przed przepełnieniem.');
  h+=memRow('ROM (bootrom układu)',m.rom_size,m.rom_size,0,null,
    'Stała pamięć producenta 384 kB, tylko do odczytu — pierwszy kod po włączeniu zasilania. Nie da się jej zająć ani zwolnić.');
  h+='<div style="font:11px system-ui;color:var(--mute);margin-top:8px">Zrzut awaryjny obecny: '+(m.coredump_present?'tak':'nie')+'.</div>';
  o.innerHTML=h;
 }catch(e){o.innerHTML='<div class="hint err">Nie udało się odczytać pamięci.</div>';}
}
// kolejna klatka dopiero, gdy poprzednia dojdzie — nie zalewamy urządzenia. Checkbox
// "Auto" (sekcja Ekran) wlacza/wylacza odswiezanie; pauza gdy karta niewidoczna LUB gdy
// sekcja Ekran nie jest na wierzchu (nie ma po co pobierac podgladu, ktorego nie widac).
// Petla chodzi zawsze: gdy warunek niespelniony, tylko sprawdza co 1 s.
function ekranOpen(){const el=$('sec-ekran');return el&&(el.classList.contains('active')||el.classList.contains('open'));}
function shotOn(){return live&&ekranOpen()&&(!$('autoshot')||$('autoshot').checked);}
function nextShot(){
 if(!shotOn()){setTimeout(nextShot,1000);return;}
 const im=new Image();
 im.onload=()=>{$('shot').src=im.src;setTimeout(nextShot,4000);};   // ~4 s, dopiero PO zaladowaniu
 im.onerror=()=>setTimeout(nextShot,4000);
 im.src='/api/screen?'+Date.now();
}
function shotDot(){
 if($('live'))$('live').textContent=shotOn()?'● na żywo':(document.hidden?'‖ wstrzymane (karta w tle)':(($('autoshot')&&!$('autoshot').checked)?'‖ auto wyłączone':'‖ wstrzymane'));
}
document.addEventListener('visibilitychange',()=>{
 live=!document.hidden;shotDot();liveSync();});
// --- czujniki BLE ---
async function bles(){
 let r;
 try{r=await(await fetch('/api/ble')).json();}catch(e){return}
 bset('b-sensors',r.length?String(r.length):'');
 if(!r.length){$('bles').innerHTML='<li>Nie wykryto czujników (nasłuch co 45 s)</li>';return}
 $('bles').innerHTML=r.map((s,i)=>{
  const st = s.valid ? `${s.t.toFixed(1)}°C · ${s.h.toFixed(0)}% · bat ${s.bat}%`
       : (s.needsKey ? '<span class=err>brak klucza</span>' : 'czekam na dane');
  const src = s.gw ? ' <span style="color:#2563C4">· przez bramkę</span>' : '';
  return `<li style="display:block">
   <div style="display:flex;justify-content:space-between">
    <b>${s.name||s.mac}</b><span class=sig>${st} · ${s.rssi} dBm${src}</span></div>
   <div class=row style="margin-top:8px">
    <input id=bn${i} placeholder="nazwa (np. Łazienka)" value="${s.name||''}">
    <input id=bk${i} placeholder="${s.hasKey?'klucz zapisany':'bindkey (32 znaki hex)'}"
     autocapitalize=off autocorrect=off>
   </div>
   <button class=s onclick="saveBle('${s.mac}',${i})">Zapisz</button>
   <div class=sig style="margin-top:6px">${s.mac}</div></li>`;
 }).join('');
}
async function saveGw(){
 $('gmsg').textContent='...';
 const hosts=[...document.querySelectorAll('[id^=blegw]')].map(e=>e.value.trim());
 const r=await(await fetch('/api/blegw',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({hosts:hosts})})).json();
 $('gmsg').className='hint '+(r.ok?'ok':'err');$('gmsg').textContent=r.msg;
 load();   // lista zageszcza sie po stronie urzadzenia — pokaz realny stan, nie wpisany
}
async function meters(){
 const r=await(await fetch('/api/meter')).json();
 if(!r.length){$('meters').innerHTML='<li>Brak odczytów. Wpisz pierwszy — porównanie pojawi się przy drugim.</li>';return;}
 $('meters').innerHTML=r.map(m=>{
  // cmp<0 znaczy "nie ma z czym porownac": albo to pierwszy odczyt, albo log
  // gazu nie siega tak daleko wstecz. To NIE jest blad i nie moze wygladac jak zero.
  let c='<span class=sig>pierwszy odczyt</span>';
  if(m.cmp>=0){
   const d=m.meter_used-m.boiler_used;
   const p=m.meter_used>0?(d/m.meter_used*100):0;
   const cl=Math.abs(p)<=5?'ok':(Math.abs(p)<=15?'':'err');
   c=`<span class="sig ${cl}">licznik ${m.meter_used.toFixed(2)} · piec ${m.boiler_used.toFixed(2)} m³
      · różnica ${d>=0?'+':''}${d.toFixed(2)} (${p>=0?'+':''}${p.toFixed(1)}%)</span>`;
  }else if(m.cmp==-2){c='<span class=sig>log gazu nie sięga tak daleko</span>';}
  return `<li><div class=row style="justify-content:space-between;align-items:center">
   <div><b>${m.date}</b> — ${m.m3.toFixed(3)} m³<br>${c}</div>
   <button class=s onclick="delMeter('${m.date}')" style="width:auto">Usuń</button>
  </div></li>`;
 }).join('');
}
async function addMeter(){
 $('mmsg').textContent='...';
 const r=await(await fetch('/api/meter',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({date:$('mdate').value,m3:parseFloat($('mval').value)})})).json();
 $('mmsg').className='hint '+(r.ok?'ok':'err');$('mmsg').textContent=r.msg;
 if(r.ok){$('mval').value='';meters();}
}
async function delMeter(d){
 await fetch('/api/meter?date='+d,{method:'DELETE'});
 meters();
}
async function saveBle(mac,i){
 $('bmsg').textContent='Zapisuję…';
 const r=await(await fetch('/api/ble',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({mac:mac,name:$('bn'+i).value,key:$('bk'+i).value.trim()})})).json();
 $('bmsg').className='hint '+(r.ok?'ok':'err');
 $('bmsg').textContent=r.msg;
 bles();
}
// "Odnów" w skrocie (sekcja Ekran) wolal viLink(), ale link i komunikat (vihref/viauth/
// vimsg) sa w sekcji INTEGRACJE — na komputerze ukrytej (SPA), na telefonie zwinietej.
// Efekt: klikam "Odnów" i nic nie widac ("nie dziala"). Najpierw wiec pokazujemy sekcje
// Integracje (SPA + rozwiniecie akordeonu + przewiniecie), dopiero potem generujemy link.
async function goViAuth(){
 showSection('integr');
 $('sec-integr').classList.add('open');
 $('sec-integr').scrollIntoView({behavior:'smooth',block:'start'});
 await viLink();
}
async function viLink(){
 $('vimsg').textContent='...';
 const r=await(await fetch('/api/vi/link',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({cid:$('vicid').value.trim()})})).json();
 $('vimsg').className='hint '+(r.ok?'ok':'err');
 $('vimsg').textContent=r.msg;
 if(r.ok){$('vihref').href=r.url;$('viauth').style.display='block';}
}
async function viForget(){
 if(!confirm('Odłączyć piec? Trzeba będzie autoryzować od nowa.'))return;
 await fetch('/api/vi/forget',{method:'POST'});
 location.reload();
}
async function viStat(){
 try{
  const r=await(await fetch('/api/vi')).json();
  $('vicid').value=r.cid||'';
  if(!r.auth){
   $('vistat').textContent='Nie autoryzowano.';
   bset('b-token','');
   $('sVi').textContent='niepodłączony';$('sVi').className='sig';
   return;
  }
  if(r.ok){
   // UCZCIWIE: token Viessmanna ROTUJE — przy kazdym odswiezeniu (~co 55 min) dostaje
   // nowe 180 dni. Dopoki urzadzenie jest online, autoryzacja NIE wygasa. Wiec "X dni"
   // to tylko zapas na offline, a nie odliczanie do konca. Gdy dziala — zaden badge.
   bset('b-token','');
   $('sVi').textContent='✓ działa';$('sVi').className='sig ok';
   $('vistat').className='hint ok';
   $('vistat').textContent=`✓ Działa automatycznie — nic nie musisz robić. `
    +`CWU ${r.dhw.toFixed(1)}°C, zasilanie ${r.sup.toFixed(1)}°C (ostatni odczyt ${agoTxt(r.ok_ago_s)}). `
    +`Autoryzacja odnawia się przy każdym połączeniu; ~${r.days>=0?r.days:'?'} dni to zapas na wypadek dłuższego offline (bez prądu/sieci). `
    +`Odnów ręcznie tylko gdy zobaczysz ostrzeżenie.`;
  }else{
   // Dane NIEAKTUALNE = odswiezanie realnie zawiodlo -> zwykle trzeba autoryzowac od nowa.
   bset('b-token','⚠');
   $('sVi').textContent='⚠ Odśwież autoryzację';$('sVi').className='sig warn';
   $('vistat').className='hint warn';
   $('vistat').textContent='⚠ Odśwież autoryzację. '
    +(r.ok_ago_s<0?'Jeszcze ani jednego udanego odczytu. '
      :`Brak świeżych danych (ostatni ${agoTxt(r.ok_ago_s)}). `)
    +(r.err?('Błąd: '+esc(r.err)+'. '):'')
    +'Autoryzacja mogła wygasnąć po dłuższym offline — kliknij „1. Zapisz i wygeneruj link autoryzacyjny” i autoryzuj piec ponownie.';
  }
 }catch(e){}
}
async function demo(on){
 $('dmsg').textContent='...';
 const r=await(await fetch('/api/radardemo?on='+on)).json();
 $('dmsg').className='hint ok';$('dmsg').textContent=r.msg;
 if(on) pickView(3);   // VIEW_RADAR=3 (dawniej bledne pickView(2)=VIEW_HOURS, nieobecny w V3)
}
// Kolorowy blok "po ludzku": kropka statusu + tytul + tresc. cls: 'ok'|'warn'|'err'.
function fblock(title,body,cls){
 const col=cls==='err'?'var(--err)':cls==='warn'?'var(--warn)':'var(--ok)';
 return '<div style="padding:8px 0;border-bottom:1px solid var(--line)">'
  +'<div style="display:flex;align-items:center;gap:8px">'
  +'<span style="width:9px;height:9px;border-radius:50%;background:'+col+';flex:0 0 auto"></span>'
  +'<b style=color:var(--panel)>'+title+'</b></div>'
  +'<div style="font:12px system-ui;color:var(--second);margin-top:4px">'+body+'</div></div>';}
// Czytelny stan awarii i lacznosci z /api/diag: reset (crashes_total/reason/was_crash),
// skrot coredumpa (zadanie+adres+backtrace, tylko czesc bezpieczna), liczniki Modbusa
// falownika opisane po ludzku, oraz stan pieca. Ma byc zrozumiale bez znajomosci kodu.
async function fails(){
 const o=$('failout');o.innerHTML='<div class=hint>Odczytuję…</div>';
 try{
  const d=await(await fetch('/api/diag?'+Date.now())).json();
  let h='';
  const rs=d.reset||{},cr=rs.crashes_total||0;
  h+=fblock('Restarty po awarii',
   (cr>0?('Urządzenie zrestartowało się po awarii (panice) '+cr+' '+razy(cr)+' od początku pomiaru. ')
        :'Ani jednego restartu po awarii. ')
   +'Ostatni restart: '+(rs.reason||'?')+(rs.was_crash?' — to była awaria.':' — normalny (nie awaria).'),
   rs.was_crash?'err':(cr>0?'warn':'ok'));
  const c=d.coredump||{};
  if(c.present){
   const bt=(c.bt||[]).slice(0,6).join(' ');
   let t='Zadanie „'+(c.task||'?')+'”, adres upadku '+(c.pc||'?')
    +(c.panic_reason?(' — '+c.panic_reason):'')+'.';
   if(bt)t+=' Ślad wywołań: '+bt+'.';
   if(c.elf_match===false)t+=' Uwaga: zrzut z innej wersji firmware — adresy mogą nie pasować.';
   t+=' Pełne dane i pobranie: „Zrzut awaryjny” niżej.';
   h+=fblock('Ostatnia awaria — skrót',t,'warn');
  }else{
   h+=fblock('Ostatnia awaria','Brak zapisanego zrzutu — urządzenie nie padło od ostatniego skasowania.','ok');
  }
  const pv=d.pv||{},fm=pv.fail_meas||{};
  const cf=(fm.connect_fail_day||0)+(fm.connect_fail_night||0);
  const sf=(fm.silent_fail_day||0)+(fm.silent_fail_night||0);
  const mr=(fm.missing_reg_day||0)+(fm.missing_reg_night||0);
  let pt='Falownik nie połączył się '+cf+' '+razy(cf)+' (brak sesji), '
   +'milczał '+sf+' '+razy(sf)+' (sesja żyła, rejestry nie odpowiadały)';
  if(mr>0)pt+=', brakowało rejestru mocy '+mr+' '+razy(mr);
  pt+='. Nocą falownik bywa uśpiony (Huawei wyłącza Modbus) — to normalne. Teraz: '
   +(pv.ok_ago_s>=0&&pv.ok_ago_s<1800?'połączony.':(pv.asleep?'uśpiony (noc).':'brak łączności.'));
  h+=fblock('Falownik (Modbus)',pt,(pv.ok_ago_s>=0&&pv.ok_ago_s<1800)||pv.asleep?'ok':'warn');
  const vi=d.vi;
  if(vi){
   const t=(vi.ok_ago_s<0?'Jeszcze nieodpytywany. ':('Ostatni odczyt '+agoTxt(vi.ok_ago_s)+'. '))
    +(vi.err?('Ostatni błąd: '+vi.err+'.'):'Bez błędów.');
   h+=fblock('Piec Viessmann (chmura)',t,vi.ok_ago_s>=0&&vi.ok_ago_s<900?'ok':'warn');
  }
  o.innerHTML=h;
 }catch(e){o.innerHTML='<div class="hint err">Nie udało się odczytać stanu.</div>';}
}
async function dg(){$('dbg').textContent=await(await fetch('/api/diag')).text();}
async function lg(){$('dbg').textContent=await(await fetch('/api/log')).text();}
async function rb(){if(confirm('Restartować?')){await fetch('/api/reboot',{method:'POST'});}}
async function cd(){
 const r=await(await fetch('/api/coredump')).json();
 $('dbg').textContent=r.present?JSON.stringify(r,null,1)
  :'Brak zrzutu awaryjnego — urządzenie nie padło od ostatniego skasowania.';
 $('cdact').style.display=r.present?'block':'none';
}
// Odpowiedz ma Content-Disposition: attachment, wiec przegladarka pobiera plik i NIE
// opuszcza panelu. Nazwa pliku (…PRYWATNE-NIE-PUBLIKOWAC.bin) przychodzi z urzadzenia.
async function cdget(){
 if(!confirm('Pobrać surowy zrzut?\n\nTo kopia pamięci urządzenia — mogą w nim być hasła '
  +'Wi-Fi i MQTT, bindkeye BLE oraz token Viessmanna. Nie publikuj tego pliku.'))return;
 location.href='/api/coredump/raw';
}
async function cddel(){
 if(!confirm('Skasować zrzut? To jedyny dowód na to, dlaczego urządzenie padło.'))return;
 await fetch('/api/coredump',{method:'DELETE'});cd();
}
async function load(){
 const r=await(await fetch('/api/state')).json();
 $('fw').textContent=r.fw;
 $('st').textContent=r.ap?'tryb konfiguracji (AP)':(r.ssid+' · '+r.ip);
 $('onl').textContent=r.ap?'● tryb AP':'● online';
 $('onl').className='pill '+(r.ap?'pAp':'pOn');
 $('cur').textContent=r.city+' ('+r.lat.toFixed(4)+', '+r.lon.toFixed(4)+')';
 $('ssid').value=r.ssid||'';$('mb').value=r.mb||'';$('peak').value=(r.peak/1000).toFixed(1);
 theme=r.theme||3;themeUI();
 // Ustawienia wyswietlacza — wartosci przychodza juz clampniete z urzadzenia.
 $('nstart').value=r.night_start;$('nend').value=r.night_end;$('dwell').value=r.dwell;
 $('blday').value=r.bl_day;$('bldim').value=r.bl_dim;$('blnight').value=r.bl_night;
 $('arot').checked=!!r.arot;
 $('mqen').checked=!!r.mq_en;$('mqhost').value=r.mq_host||'';$('mqport').value=r.mq_port||1883;
 $('mqpre').value=r.mq_pre||'';$('mquser').value=r.mq_user||'';$('mqpass').value='';
 $('sMqtt').textContent=r.mq_en?'włączone':'wyłączone';$('sMqtt').className='sig '+(r.mq_en?'ok':'');
 // Pola generuje JS z tablicy, zeby ich liczba szla za firmware'em, a nie za HTML-em.
 $('gws').innerHTML=(r.blegw||[]).map((h,i)=>
  `<input id=blegw${i} value="${h}" placeholder="${i?'kolejna bramka (opcjonalnie)':'np. 192.168.0.102'}"
   autocapitalize=off autocorrect=off style="margin-bottom:6px">`).join('');
 // hasla brokera NIE zwracamy — tylko informacje, ze jakies jest zapisane
 $('mqpass').placeholder=r.mq_pass_set?'(zapisane — zostaw puste, aby nie zmieniać)':'(brak)';
}
async function scan(){
 $('nets').innerHTML='<li>Skanuję…</li>';
 const r=await(await fetch('/api/scan')).json();
 $('nets').innerHTML=r.map(n=>`<li onclick="pick('${n.s.replace(/'/g,"\\'")}')">
   <span>${n.s} ${n.e?'🔒':''}</span><span class=sig>${n.r} dBm</span></li>`).join('')
   ||'<li>Nic nie znaleziono</li>';
}
function pick(s){$('ssid').value=s;$('pass').focus();}
async function saveWifi(){
 $('wmsg').textContent='Łączę…';
 const r=await(await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({ssid:$('ssid').value,pass:$('pass').value})})).json();
 $('wmsg').className='hint '+(r.ok?'ok':'err');
 $('wmsg').textContent=r.msg;
}
async function geo(){
 $('locs').innerHTML='<li>Szukam…</li>';
 const r=await(await fetch('/api/geo?q='+encodeURIComponent($('q').value))).json();
 if(r.err){$('locs').innerHTML='<li class=err>'+r.err+'</li>';return}
 $('locs').innerHTML=r.map((l,i)=>`<li onclick="setLoc(${i})">
   <span>${l.n}<br><span class=sig>${l.a}</span></span>
   <span class=b>${l.lat.toFixed(2)}, ${l.lon.toFixed(2)}</span></li>`).join('')
   ||'<li>Brak wyników</li>';
 window._L=r;
}
async function setLoc(i){
 const l=window._L[i];
 await fetch('/api/loc',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({city:l.n,lat:l.lat,lon:l.lon})});
 $('locs').innerHTML='';load();
}
async function saveInv(){
 const r=await(await fetch('/api/inv',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({mb:$('mb').value,peak:Math.round($('peak').value*1000)})})).json();
 $('imsg').className='hint ok';$('imsg').textContent=r.msg;
}
async function saveMqtt(){
 $('qmsg').textContent='Zapisuję…';
 const r=await(await fetch('/api/mqtt',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({en:$('mqen').checked,host:$('mqhost').value,
   port:parseInt($('mqport').value)||1883,pre:$('mqpre').value,
   user:$('mquser').value,pass:$('mqpass').value})})).json();
 $('qmsg').className='hint '+(r.ok?'ok':'err');
 $('qmsg').textContent=r.msg;
 load();
}
async function upd(){
 $('umsg').textContent='Sprawdzam…';
 const r=await(await fetch('/api/update',{method:'POST'})).json();
 $('umsg').textContent=r.msg;
}
async function fgt(){
 if(!confirm('Usunąć zapisaną sieć Wi-Fi?'))return;
 await fetch('/api/forget',{method:'POST'});location.reload();
}
// Wpiecie nawigacji: pozycje sidebaru (komputer) i naglowki sekcji (telefon).
document.querySelectorAll('.navitem').forEach(a=>a.onclick=()=>showSection(a.dataset.sec));
document.querySelectorAll('.sechead').forEach(b=>b.onclick=()=>toggleSection(b.dataset.sec));
(async()=>{
 // Motyw: wczesny skrypt u gory juz dodal klase .dark z localStorage — tu tylko
 // synchronizujemy napis przycisku. Checkbox "Auto" podglądu odswieza wskaznik.
 applyDark(document.documentElement.classList.contains('dark'));
 if($('autoshot'))$('autoshot').onchange=shotDot;
 shotDot();
 try{const r=await(await fetch('/api/view')).json();pin=r.pin;}catch(e){}
 tabs();highlightBl('auto');nextShot();await load();bles();viStat();meters();diagPills();
 setInterval(bles,20000);setInterval(viStat,30000);setInterval(diagPills,60000);
// Data domyslnie dzisiejsza — odczyt licznika robi sie zwykle w dniu wpisywania.
$('mdate').value=new Date().toISOString().slice(0,10);
})();
</script></html>)HTML";

// ------------------------------------------------------------------- API -----

void sendPage() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void apiState() {
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["ap"] = apMode;
  d["ssid"] = settings().ssid;
  d["ip"] = apMode ? apIpStr : WiFi.localIP().toString();
  d["city"] = settings().city;
  d["lat"] = settings().lat;
  d["lon"] = settings().lon;
  d["mb"] = settings().modbusHost;
  d["peak"] = settings().pvPeakW;
  d["theme"] = settings().theme;   // 1/2/3 — panel czyta to przy kazdym ladowaniu

  // Ustawienia wyswietlacza (tryb nocny + rotacja + jasnosc). Wartosci sa juz
  // clampniete w Settings::load(), wiec panel pokazuje PRAWDE (to, co realnie dziala).
  d["night_start"] = settings().nightStartH;
  d["night_end"] = settings().nightEndH;
  d["dwell"] = settings().dwellS;
  d["bl_day"] = settings().blDay;
  d["bl_dim"] = settings().blDim;
  d["bl_night"] = settings().blNight;
  d["arot"] = settings().autoRotate;   // auto-rotacja V3 (checkbox w panelu)

  // UWAGA: hasla brokera nie zwracamy NIGDY — tylko flage "cos jest ustawione".
  d["mq_en"] = settings().mqttEnabled;
  d["mq_host"] = settings().mqttHost;
  d["mq_port"] = settings().mqttPort;
  d["mq_pre"] = settings().mqttPrefix;
  d["mq_user"] = settings().mqttUser;
  d["mq_pass_set"] = settings().mqttPass[0] != '\0';
  // Tablica, nie string. Liczba pol w panelu ma isc za firmware'em (Settings::BLE_GW),
  // a nie byc przepisana w HTML-u — to bylby kolejny literal udajacy zrodlo prawdy.
  JsonArray gw = d["blegw"].to<JsonArray>();
  for (int i = 0; i < Settings::BLE_GW; ++i) gw.add(settings().bleGwAt(i));

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void apiScan() {
  // Blokada obejmuje CALY cykl: skan, odczyt wynikow i scanDelete(). Bufor wynikow
  // jest w rdzeniu JEDEN i wspolny z przegladem roamingowym w netTask, ktory tez
  // bierze te blokade. Bez tego klikniecie "Wyszukaj sieci" w trakcie roamingu
  // czytalo pamiec zwolniona przez cudzy scanDelete() — panic i restart.
  // Czekamy do 8 s: netTask trzyma blokade na czas swojego skanu (1-2 s), wiec
  // w praktyce zawsze zdazymy. Gdyby jednak nie — pusta lista i uzytkownik klika
  // jeszcze raz. Panel na moment przymuli; to uczciwa cena za brak restartu.
  if (!scanLock(8000)) {
    server.send(200, "application/json", "[]");
    return;
  }

  const int n = WiFi.scanNetworks(false, true);
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < n && i < 24; ++i) {
    JsonObject o = a.add<JsonObject>();
    o["s"] = WiFi.SSID(i);
    o["r"] = WiFi.RSSI(i);
    o["e"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  scanUnlock();

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void apiWifi() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Zly JSON\"}");
    return;
  }
  const char* s = in["ssid"] | "";
  const char* p = in["pass"] | "";
  if (strlen(s) == 0) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"Podaj nazwę sieci\"}");
    return;
  }

  // Sprobuj polaczyc ZANIM zapiszemy. Zeby ten test cokolwiek znaczyl, musza byc
  // spelnione trzy rzeczy — kazda z nich zalatwia inna droge do falszywego "OK":
  //  1) netTask nie moze w tym czasie wolac WiFi.begin() ze STARYMI danymi z NVS
  //     (flaga ponizej) — inaczej test mierzy powrot na stara siec, nie nowe haslo;
  //  2) stara asocjacja musi byc zerwana PRZED proba — inaczej przy zmianie samego
  //     hasla (ta sama nazwa sieci) test zdaje sie od razu, nie sprawdzajac niczego;
  //  3) na koncu sprawdzamy nie tylko "polaczony", ale "polaczony z TA nazwa".
  const WifiCfgGuard cfgGuard;

  WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect();
  const uint32_t td = millis();
  while (WiFi.status() == WL_CONNECTED && millis() - td < 2000) {
    delay(50);
  }

  WiFi.begin(s, p);
  const uint32_t t0 = millis();
  while (millis() - t0 < 14000) {
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == s) {
      break;
    }
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED || WiFi.SSID() != s) {
    // Nie zapisujemy niczego. netTask (gdy flaga wygasnie / straznik ja skasuje)
    // sam odtworzy poprzednie polaczenie ze starych danych z NVS.
    server.send(200, "application/json",
                "{\"ok\":false,\"msg\":\"Nie udało się połączyć — sprawdź hasło\"}");
    return;
  }

  strncpy(settings().ssid, s, sizeof(settings().ssid) - 1);
  strncpy(settings().pass, p, sizeof(settings().pass) - 1);
  settings().save();
  wifiSaved = true;

  JsonDocument d;
  d["ok"] = true;
  char msg[80];
  snprintf(msg, sizeof(msg), "Połączono. Panel: http://%s", WiFi.localIP().toString().c_str());
  d["msg"] = msg;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// Geokoder Open-Meteo — dziala tylko gdy urzadzenie ma internet (tryb STA).
void apiGeo() {
  const String q = server.arg("q");
  if (q.length() < 2) {
    server.send(200, "application/json", "{\"err\":\"Wpisz nazwę miejscowości\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    server.send(200, "application/json",
                "{\"err\":\"Najpierw połącz urządzenie z Wi-Fi\"}");
    return;
  }

  String url = "https://geocoding-api.open-meteo.com/v1/search?count=8&language=pl&name=";
  for (size_t i = 0; i < q.length(); ++i) {
    const char c = q[i];
    if (isalnum(static_cast<unsigned char>(c))) {
      url += c;
    } else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", static_cast<unsigned char>(c));
      url += b;
    }
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    server.send(200, "application/json", "{\"err\":\"Błąd połączenia\"}");
    return;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    server.send(200, "application/json", "{\"err\":\"Geokoder nie odpowiada\"}");
    return;
  }
  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(200, "application/json", "{\"err\":\"Zła odpowiedź geokodera\"}");
    return;
  }

  JsonDocument out;
  JsonArray a = out.to<JsonArray>();
  for (JsonObjectConst r : doc["results"].as<JsonArrayConst>()) {
    JsonObject o = a.add<JsonObject>();
    o["n"] = r["name"];
    String adm;
    if (r["admin1"].is<const char*>()) {
      adm = r["admin1"].as<const char*>();
    }
    if (r["country"].is<const char*>()) {
      if (adm.length()) adm += ", ";
      adm += r["country"].as<const char*>();
    }
    o["a"] = adm;
    o["lat"] = r["latitude"];
    o["lon"] = r["longitude"];
  }
  String s;
  serializeJson(out, s);
  server.send(200, "application/json", s);
}

void apiLoc() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  strncpy(settings().city, in["city"] | "Gdynia", sizeof(settings().city) - 1);
  settings().lat = in["lat"] | 54.4870f;
  settings().lon = in["lon"] | 18.5216f;
  settings().save();
  server.send(200, "application/json", "{\"ok\":true}");
}

void apiInv() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  const char* mb = in["mb"] | "";
  if (strlen(mb) >= 7) {
    strncpy(settings().modbusHost, mb, sizeof(settings().modbusHost) - 1);
  }
  const int peak = in["peak"] | 6000;
  settings().pvPeakW = static_cast<uint16_t>(peak < 500 ? 500 : (peak > 60000 ? 60000 : peak));
  settings().save();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Zapisano\"}");
}

// Prefiks jest krotki celowo — wchodzi do kazdego pakietu discovery, ktory musi
// sie zmiescic w 512 B bufora klienta MQTT. Dlatego twardo przycinamy do 23 znakow.
void apiMqtt() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Zły JSON\"}");
    return;
  }

  Settings& s = settings();
  const bool en = in["en"] | false;
  const char* host = in["host"] | "";
  const char* pre = in["pre"] | "";
  const char* user = in["user"] | "";
  const char* pass = in["pass"] | "";
  const int port = in["port"] | 1883;

  if (en && strlen(host) == 0) {
    server.send(200, "application/json",
                "{\"ok\":false,\"msg\":\"Podaj adres brokera\"}");
    return;
  }

  strncpy(s.mqttHost, host, sizeof(s.mqttHost) - 1);
  s.mqttHost[sizeof(s.mqttHost) - 1] = '\0';
  s.mqttPort = static_cast<uint16_t>((port < 1 || port > 65535) ? 1883 : port);

  if (strlen(pre) > 0) {
    strncpy(s.mqttPrefix, pre, sizeof(s.mqttPrefix) - 1);
    s.mqttPrefix[sizeof(s.mqttPrefix) - 1] = '\0';
  }

  strncpy(s.mqttUser, user, sizeof(s.mqttUser) - 1);
  s.mqttUser[sizeof(s.mqttUser) - 1] = '\0';

  // Puste haslo = "nie ruszaj tego, co zapisane". Samo "-" = skasuj.
  if (strcmp(pass, "-") == 0) {
    s.mqttPass[0] = '\0';
  } else if (strlen(pass) > 0) {
    strncpy(s.mqttPass, pass, sizeof(s.mqttPass) - 1);
    s.mqttPass[sizeof(s.mqttPass) - 1] = '\0';
  }

  s.mqttEnabled = en;
  s.save();
  mqttha::configChanged();  // zadanie sieciowe zestawi sesje od nowa

  char msg[80];
  snprintf(msg, sizeof(msg), "Zapisano. MQTT %s.", en ? "włączony" : "wyłączony");
  JsonDocument d;
  d["ok"] = true;
  d["msg"] = msg;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void apiUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(200, "application/json", "{\"msg\":\"Brak internetu\"}");
    return;
  }
  // Nie robimy OTA w zadaniu web — tylko zgłaszamy prośbę zadaniu sieciowemu.
  requestOtaCheck();
  server.send(200, "application/json",
              "{\"msg\":\"Sprawdzanie zlecone — jeśli jest nowsza wersja, "
              "urządzenie zaktualizuje się i zrestartuje.\"}");
}

// --- diagnostyka: urządzenie wisi na ścianie bez USB, Serial jest ślepy ---

void apiLog() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", logDump());
}

// --- zrzut awaryjny: streszczenie, czyli "dlaczego padlo" bez pobierania 64 kB ---
//
// Wypelnia `o` TYLKO tym, co jest bezpieczne do wklejenia do publicznego zgloszenia:
// adresami kodu i nazwa zadania. Wolno to wolac z /api/diag wlasnie dlatego, ze nie
// wchodzi tu ani jeden bajt ZAWARTOSCI pamieci — patrz dlugi komentarz nizej.
//
// Kluczowa rzecz: `esp_core_dump_get_summary()` REALNIE ISTNIEJE w tym rdzeniu
// (esp_core_dump.h:172, symbol T w lib/libespcoredump.a) i jest widoczne, bo naglowek
// odslania je pod `#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && ..._DATA_FORMAT_ELF`,
// a oba sa =y. Ten sam warunek powtarzamy ponizej, zeby zmiana konfiguracji rdzenia
// zepsula co najwyzej te sekcje, a nie caly build panelu.
void coredumpInfo(JsonObject o) {
  size_t addr = 0, size = 0;
  const bool present = esp_core_dump_image_get(&addr, &size) == ESP_OK && size > 0;
  o["present"] = present;
  if (!present) {
    return;   // brak zrzutu to NIE blad: tak wyglada urzadzenie, ktore nie padlo
  }
  o["size"] = size;
  o["addr"] = addr;
  // CRC32 liczy rdzen, czytajac obraz z flasha. Zrzut uciety (np. panic w trakcie
  // zapisu) da tu false — i wtedy wiadomo, ze streszczenie ponizej moze byc smieciem.
  o["crc_ok"] = esp_core_dump_image_check() == ESP_OK;

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  // ~212 B na stosie zadania web (zapas 12,7 kB). Rdzen parsuje ELF-a przez mmap
  // partycji, wiec NIE alokuje 64 kB na stercie — dlatego wolno to robic tutaj.
  esp_core_dump_summary_t s;
  if (esp_core_dump_get_summary(&s) == ESP_OK) {
    s.exc_task[sizeof(s.exc_task) - 1] = '\0';
    // String(), nie goly wskaznik: `s` to zmienna NA STOSIE, a ArduinoJson dla
    // `const char*` zapamietuje sam wskaznik — po wyjsciu z tej funkcji serializacja
    // czytalaby martwa ramke. Dokladnie ten blad zjadl juz raz liste BLE (patrz apiBleList).
    o["task"] = String(s.exc_task);

    char hex[12];
    snprintf(hex, sizeof(hex), "0x%08x", static_cast<unsigned>(s.exc_pc));
    o["pc"] = String(hex);   // ADRES UPADKU — to jest ta jedna liczba, po ktora sie tu przyszlo
    snprintf(hex, sizeof(hex), "0x%08x", static_cast<unsigned>(s.ex_info.exc_vaddr));
    o["vaddr"] = String(hex);            // adres, ktory zabolal (0x0 = wyluskanie nulla)
    o["cause"] = s.ex_info.exc_cause;    // EXCCAUSE
    o["version"] = s.core_dump_version;

    // Ktora WERSJA firmware'u padla. Zrzut przezywa OTA, wiec potrafi pochodzic ze
    // starszego builda niz ten, ktory teraz chodzi — a do rozszyfrowania adresow trzeba
    // DOKLADNIE tego app.elf, ktory sie wywalil. Bez tego pola nie ma jak tego sprawdzic.
    s.app_elf_sha256[sizeof(s.app_elf_sha256) - 1] = '\0';
    const char* dumpSha = reinterpret_cast<const char*>(s.app_elf_sha256);
    o["app_elf_sha"] = String(dumpSha);

    // ...tyle ze samo WYSTAWIENIE tego pola nie chronilo przed niczym: podawalismy je
    // i nigdy z niczym nie porownywali, wiec zeby cokolwiek z niego wyniknelo, trzeba
    // bylo z wlasnej woli sprawdzic sume recznie. Nikt tego nie robi, gdy odpowiedz
    // wyglada na kompletna. Dlatego urzadzenie porownuje samo.
    //
    // Rdzen zapisuje w zrzucie sha256 ELF-a jako NAPIS skrocony do
    // CONFIG_APP_RETRIEVE_LEN_ELF_SHA (=9) znakow hex — dokladnie ten sam napis, ktory
    // esp_app_get_elf_sha256() zwraca dla biezacego firmware'u. Obie strony formatuje ta
    // sama funkcja rdzenia (libespcoredump.a linkuje sie do app_elf_sha256_str), wiec
    // porownanie jest znak w znak i nie zgadujemy tu formatu.
    //
    // NIE brac tego z esp_app_get_description()->app_elf_sha256: TAMTO pole to 32 SUROWE
    // BAJTY, a nie napis. Porownane wprost z tym z zrzutu nie zgadzaloby sie NIGDY —
    // i elf_match bylby kolejnym polem, ktore zawsze klamie, tym razem w druga strone.
    char runSha[sizeof(s.app_elf_sha256)] = {};
    esp_app_get_elf_sha256(runSha, sizeof(runSha));
    o["app_elf_sha_running"] = String(runSha);
    const bool elfMatch = strcmp(dumpSha, runSha) == 0;
    o["elf_match"] = elfMatch;

    // Backtrace: same adresy kodu. To on odpowiada "gdzie padlo", a rozszyfrowuje sie go
    // przez: xtensa-esp32s3-elf-addr2line -pfiaC -e build/pogoda-gdynia.ino.elf <adresy>
    // — ale TYLKO gdy elf_match == true. Patrz nizej.
    JsonArray bt = o["bt"].to<JsonArray>();
    const uint32_t depth = s.exc_bt_info.depth > 16 ? 16 : s.exc_bt_info.depth;
    for (uint32_t i = 0; i < depth; ++i) {
      snprintf(hex, sizeof(hex), "0x%08x", static_cast<unsigned>(s.exc_bt_info.bt[i]));
      bt.add(String(hex));
    }
    // PULAPKA, w ktora naprawde sie wdepnelo: `bt_corrupted: false` NIE ZNACZY
    // "backtrace prawdziwy". Znaczy dokladnie tyle, ze lancuch ramek jest SPOJNY SAM ZE
    // SOBA — kazda ramka wskazuje na nastepna i rdzen doszedl po nich do konca. O tym,
    // czy te adresy odpowiadaja JAKIEMUKOLWIEK ELF-owi, nie mowi nic i mowic nie moze:
    // liczy je z pamieci, nie ma z czym ich zestawic.
    //
    // Zrzut ze starszego builda da tu wiec spokojnie `false` (lancuch BYL w porzadku
    // w chwili panica), a addr2line z ELF-em innej wersji przetlumaczy te adresy na
    // prawdziwe nazwy funkcji, ktore nigdy sie nawzajem nie wolaly — stos wyglada
    // wiarygodnie i jest w calosci zmyslony. Odpowiedzia na pytanie "czy wolno to
    // czytac" jest elf_match, a nie to pole.
    o["bt_corrupted"] = s.exc_bt_info.corrupted;

    // `bt` zostaje takze przy niezgodnosci — z WLASCIWYM app.elf jest pelnowartosciowy,
    // a to jedyny egzemplarz tych adresow, jaki istnieje. Znika za to zludzenie, ze
    // mozna go odczytac tym, co akurat lezy w build/.
    if (!elfMatch) {
      o["note"] =
          "UWAGA: zrzut pochodzi z INNEGO builda niz firmware, ktory teraz chodzi "
          "(app_elf_sha != app_elf_sha_running). Adresy w `pc`, `vaddr` i `bt` NIE "
          "odnosza sie do biezacego ELF-a. addr2line z build/pogoda-gdynia.ino.elf poda "
          "z nich pelny stos z prawdziwymi nazwami funkcji — i ten stos bedzie FALSZYWY. "
          "`bt_corrupted: false` tego nie wyklucza: mowi tylko, ze lancuch ramek jest "
          "spojny, nie ze pasuje do tego ELF-a. Do odczytania potrzebny jest app.elf "
          "builda o sumie app_elf_sha; jesli go nie ma — skasuj zrzut "
          "(DELETE /api/coredump) i poczekaj na nastepny panic, ktory bedzie juz z "
          "biezacej wersji.";
    }

    // CELOWO NIE WYSTAWIAMY ex_info.exc_a[16] (rejestry a0..a15 z chwili wyjatku).
    // Cala reszta powyzej to ADRESY — wskazniki na kod i na pamiec. Rejestry to jedyne
    // pole streszczenia, ktore moze zawierac DANE: gdyby urzadzenie padlo w srodku
    // kopiowania hasla, siedzialyby w nich jego 4-bajtowe kawalki. A /api/diag jest w tym
    // projekcie wprost pomyslane do wklejania do zgloszen (patrz komentarze przy pv i mqtt).
    // Komu rejestry naprawde potrzebne, ten i tak siega po /api/coredump/raw i gdb.
  }

  // Gotowa odpowiedz "dlaczego padlo", slowami rdzenia — np.
  // "Guru Meditation Error: Core 0 panic'ed (StoreProhibited)". Staly tekst z panic
  // handlera, zadnych danych z pamieci.
  char reason[160] = {};
  if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK) {
    o["panic_reason"] = String(reason);
  }
#endif
}

void apiDiag() {
  const Diag& d = diag();
  const uint32_t now = millis();
  auto ago = [&](uint32_t at) -> int {
    if (at == 0) return -1;
    // Signed: `at` bywa SWIEZSZE niz `now`, gdy pisze je inny watek juz PO zlapaniu
    // `now` w tym watku. Bez tego (now - at) na uint32 przekreca sie w ~4294967
    // (0xFFFFFFFF/1000). Przyszlosc traktujemy jak "teraz".
    // Od v101 pirLastAt pisze pirIsr(), a nie loop() — okno na wyprzedzenie zrobilo sie
    // WIEKSZE, nie mniejsze: przerwanie wchodzi w dowolnym momencie, takze miedzy
    // millis() powyzej a ta linijka. Ta poprawka jest teraz bardziej potrzebna niz byla.
    const int32_t d = static_cast<int32_t>(now - at);
    return d < 0 ? 0 : d / 1000;
  };

  JsonDocument j;
  j["fw"] = FW_VERSION;
  j["uptime_s"] = now / 1000;
  j["heap_free"] = ESP.getFreeHeap();
  j["heap_min"] = d.minHeap == 0xFFFFFFFF ? ESP.getFreeHeap() : d.minHeap;
  j["heap_min_ever"] = ESP.getMinFreeHeap();
  j["heap_largest_block"] = ESP.getMaxAllocHeap();
  j["psram"] = ESP.getPsramSize();
  j["cpu_temp"] = temperatureRead();

  // JEDEN obiekt "wifi" — drugie j["wifi"].to<JsonObject>() zastapiloby go nowym,
  // pustym (tak wlasnie ginely ssid/ip/connects). Nazwa sieci TAK, hasla NIGDY.
  JsonObject w = j["wifi"].to<JsonObject>();
  w["ssid"] = WiFi.SSID();
  w["ip"] = WiFi.localIP().toString();
  w["rssi"] = WiFi.RSSI();
  w["connects"] = d.wifiConnects;
  w["bssid"] = WiFi.BSSIDstr();
  w["channel"] = WiFi.channel();
  w["roams"] = d.wifiRoams;

  JsonObject we = j["weather"].to<JsonObject>();
  we["ok_ago_s"] = ago(d.weatherOkAt);
  we["err"] = d.weatherErr;

  JsonObject pv = j["pv"].to<JsonObject>();
  pv["ok_ago_s"] = ago(d.pvOkAt);
  pv["err"] = d.pvErr;
  pv["asleep"] = d.pvAsleep;   // noc: Huawei wyłącza Modbus TCP — to nie awaria

  // Kumulacyjne liczniki Modbusa (patrz Log.h). Bez IP falownika — same liczby,
  // a /api/diag bywa wklejane do zgłoszeń błędów.
  // Odsetek porażek liczy się jako fails / (cycles * 5); `*_hist` to rozkład
  // porażek na cykl (indeks 0..5) i dopiero on mówi, co zrobiłby dany próg.
  pv["cycles"] = d.pvCycles;
  pv["fails"] = d.pvFails;
  pv["extra_fails"] = d.pvExtraFails;
  JsonArray fh = pv["fail_hist"].to<JsonArray>();
  JsonArray eh = pv["extra_hist"].to<JsonArray>();
  for (int i = 0; i < 6; ++i) {
    fh.add(d.pvFailHist[i]);
    eh.add(d.pvExtraHist[i]);
  }

  // --- PRZYCZYNY porażek Modbusa, PRZEŻYWAJĄ restart (patrz PvRtc w Log.h) ---
  // Po co: noc 19/20 lipca 2026 pokazała, że pv.fails/pv.extra_fails wyżej NIE
  // wystarczają do zdiagnozowania nocnej awarii Modbusa — restart o 00:15 je wyzerował
  // (DRAM), log (bufor ~6 minut) już wtedy nic nie pamiętał, a pv.asleep/"Falownik
  // uśpiony" wyglądają identycznie i dla realnego snu falownika, i dla martwej sesji.
  // Te liczniki żyją w RTC (przeżywają restart, OTA, panic) i rozbite są na DWIE
  // RÓŻNE przyczyny x PORĘ DOBY:
  //   connect_fail_* — ensureConnected()==false: nie ma sesji TCP w ogóle.
  //   silent_fail_*  — sesja TCP żyje, ale `fails>=3`: rejestry przestały odpowiadać.
  // Rozbicie dzień/noc NIE jest kosmetyką: nocą falownik naprawdę może spać (Huawei
  // wyłącza Modbus TCP po zachodzie) i nieudane połączenie jest wtedy oczekiwane — bez
  // rozbicia zdrowe nocne zasypianie zalewałoby TEN SAM licznik, co realna awaria, i
  // znowu by ją zamaskowało. Duże connect_fail_night/silent_fail_night SAME W SOBIE nie
  // są dowodem awarii (to może być zwykła noc) — dopiero porównanie noc do nocy (albo
  // z dniem, albo w czasie) to pokazuje.
  JsonObject pm = pv["fail_meas"].to<JsonObject>();
  pm["connect_fail_day"] = gPvRtc.connectFailDay;
  pm["connect_fail_night"] = gPvRtc.connectFailNight;
  pm["silent_fail_day"] = gPvRtc.silentFailDay;
  pm["silent_fail_night"] = gPvRtc.silentFailNight;
  // TRZECIA, inna przyczyna (brakuje jednego rejestru mocy przy żywej sesji — patrz
  // pv.fail_hist wyżej i komentarz przy `missing` w PvClient.cpp). Licząca się OSOBNO,
  // celowo nie zmieszana z połączeniem/ciszą powyżej.
  pm["missing_reg_day"] = gPvRtc.missingRegDay;
  pm["missing_reg_night"] = gPvRtc.missingRegNight;

  // "zbieram od" — dokładnie ta sama mechanika co sensors.pir_meas/ldr_meas niżej i z
  // tego samego powodu: uptime_s zeruje się przy KAŻDYM restarcie, ten pomiar nie.
  pm["collected_s"] = gPvRtc.collectedS;
  pm["boots"] = gPvRtc.boots;
  if (gPvRtc.startedEpoch > 0) {
    pm["started_epoch"] = gPvRtc.startedEpoch;
    const time_t nowT = time(nullptr);
    if (nowT > 1700000000) {
      const uint32_t wall = static_cast<uint32_t>(nowT) - gPvRtc.startedEpoch;
      pm["wall_s"] = wall;
      pm["gap_s"] = wall > gPvRtc.collectedS ? wall - gPvRtc.collectedS : 0;
    }
  } else {
    pm["started_epoch"] = nullptr;   // NTP jeszcze nigdy nie dał czasu
  }

  // --- piec: SUROWE liczniki z API (patrz Log.h) ---
  // Wystawione po to, zeby dalo sie ODCZYTAC Z URZADZENIA, czy `hours` rusza sie
  // o 0,01 czy o 0,1 (albo wcale) i czy `starts` inkrementuje sie przy kazdym
  // zaplonie. Bez tego pomiaru kazdy projekt wykresu palnika jest zgadywaniem:
  // odpytujemy piec co 3 min, a cykl CWU bywa krotszy, wiec chwilowa modulacja
  // przesypia cale grzania (stad "gaz 1,1 m3, modulacja caly czas zero").
  // Zadnych sekretow: liczby z pieca, nigdy token ani clientId.
  // Zmienna nazywa sie `pc` (piec), a NIE `vi`: `vi` to nazwa przestrzeni nazw
  // (Viessmann.h) i lokalna zmienna zaslonilaby ja w calej tej funkcji.
  JsonObject pc = j["vi"].to<JsonObject>();
  pc["ok_ago_s"] = ago(d.viOkAt);
  pc["err"] = d.viErr;
  pc["dhw"] = d.viDhwC;
  pc["sup"] = d.viSupplyC;
  // has_hours == false znaczy "wlasciwosci `hours` NIE BYLO w odpowiedzi" — a nie
  // "licznik stoi". Bez tego rozroznienia nieruchome zero przez cala dobe prowadzi
  // do dokladnie zlego wniosku.
  // DWIE flagi, bo `hours` i `starts` moga przyjsc niezaleznie: kazda liczba ponizej
  // ma wlasny dowod na to, ze jest pomiarem. Jesli has_hours == false, to
  // burner_hours = 0,0 nic nie znaczy i nie wolno go czytac.
  pc["has_hours"] = d.viHasBurnerHours;
  pc["has_starts"] = d.viHasBurnerStarts;
  pc["burner_hours"] = d.viBurnerHours;     // Z ULAMKIEM, surowo z API
  pc["burner_starts"] = d.viBurnerStarts;   // surowo z API
  pc["modulation"] = d.viModulation;        // ostatnia zlapana
  pc["burner_active"] = d.viBurnerActive;
  pc["has_gas"] = d.viHasGas;
  pc["gas_day"] = d.viGasDayM3;             // dhw + heating, currentDay

  // Ostatni restart — dotąd nie wiedzieliśmy, czy urządzenie się wywala.
  JsonObject rs = j["reset"].to<JsonObject>();
  rs["reason"] = resetReasonText(d.resetReason);
  rs["reason_code"] = d.resetReason;
  rs["prev_reason"] = resetReasonText(d.prevResetReason);
  rs["was_crash"] = resetWasCrash();
  rs["crashes_total"] = d.panicCount;

  // Dotad `crashes_total` mowilo ILE razy padlo i na tym sie konczylo. Zrzut awaryjny
  // dokłada DLACZEGO: zadanie, adres upadku i backtrace — bez pobierania czegokolwiek
  // i bez USB. Same adresy i nazwa zadania, wiec ta sekcja jest bezpieczna do wklejenia
  // razem z reszta /api/diag. Caly plik (i jego prywatnosc) to /api/coredump.
  coredumpInfo(j["coredump"].to<JsonObject>());

  JsonObject r = j["radar"].to<JsonObject>();
  r["ok_ago_s"] = ago(d.radarOkAt);
  r["level"] = d.radarLevel;
  r["frame_age_s"] = d.radarAgeSec;
  r["skips_low_ram"] = d.radarSkips;
  r["err"] = d.radarErr;

  JsonObject f = j["flights"].to<JsonObject>();
  f["ok_ago_s"] = ago(d.flightOkAt);
  f["total"] = d.flightsTotal;
  f["err"] = d.flightErr;

  // --- jakosc powietrza (ARMAAG/sensorbox Gdynia — GA17 z automatycznym zapasem
  // GA24, patrz AirClient.h) ---
  // W odroznieniu od pogody/PV/lotow wyzej (ktore pokazuja tylko ok_ago_s/err) tu
  // wystawiamy TAKZE same wartosci — to jedyny sposob, zeby zdalnie sprawdzic, czy
  // fallback GA17->GA24 zadzialal i co REALNIE widzi wlasciciel na ekranie (patrz
  // uzasadnienie fallbacku w AirData.h). "err" tu NIE oznacza, ze air.station/pm10/
  // pm25 sa nieaktualne — tak samo jak przy pogodzie/PV, blad fetcha NIE kasuje
  // ostatnich dobrych danych (patrz komentarz w netTask, pogoda-gdynia.ino), wiec
  // przy chwilowej awarii sieci ok_ago_s po prostu rosnie, a wartosci zostaja te
  // ostatnie prawdziwe.
  JsonObject air = j["air"].to<JsonObject>();
  air["ok_ago_s"] = ago(d.airOkAt);
  air["err"] = d.airErr;
  air["fallback"] = d.airFallback;      // true = na ekranie GA24 (Halicka)
  air["station"] = d.airStation;        // "SANDOMIERSKA" / "HALICKA" / ""
  air["has_pm10"] = d.airHasPm10;
  // Brak danych ma wygladac jak brak danych — ta sama lekcja co viHas*/pir_min_ms
  // wyzej w tej funkcji: nieruchome "0" czytaloby sie jak realny, dobry pomiar.
  if (d.airHasPm10) air["pm10"] = d.airPm10; else air["pm10"] = nullptr;
  air["has_pm25"] = d.airHasPm25;
  if (d.airHasPm25) air["pm25"] = d.airPm25; else air["pm25"] = nullptr;
  air["index"] = d.airIndex;             // 1..6, 0 = brak (patrz tabela w AirClient.cpp)
  air["index_name"] = airIndexName(d.airIndex);
  if (d.airSampleEpoch > 0) {
    air["sample_epoch"] = d.airSampleEpoch;   // unix epoch (UTC) pomiaru na stacji
    const time_t nowT = time(nullptr);
    // Wiek POMIARU (od stacji), NIE wiek naszego fetch'a (to jest ok_ago_s wyzej) —
    // ta sama para pojec, co na ekranie (patrz WeatherUi::drawViewAir).
    if (nowT > 1700000000) {
      air["sample_age_s"] = static_cast<long>(nowT - static_cast<time_t>(d.airSampleEpoch));
    } else {
      air["sample_age_s"] = nullptr;
    }
  } else {
    air["sample_epoch"] = nullptr;
    air["sample_age_s"] = nullptr;
  }

  JsonObject mem = j["mem"].to<JsonObject>();
  mem["sram_free"] = ESP.getFreeHeap();
  mem["sram_min"] = d.minHeap == 0xFFFFFFFF ? 0 : d.minHeap;
  mem["sram_block"] = ESP.getMaxAllocHeap();
  mem["psram_size"] = ESP.getPsramSize();
  mem["psram_free"] = ESP.getFreePsram();
  mem["stack_net_spare"] = d.stackNet;
  mem["stack_web_spare"] = d.stackWeb;

  JsonObject gfx = j["gfx"].to<JsonObject>();
  gfx["draw_us"] = d.frameDrawUs;
  gfx["push_us"] = d.framePushUs;
  gfx["frame_ms"] = (d.frameDrawUs + d.framePushUs) / 1000.0;
  gfx["max_fps"] = d.frameDrawUs + d.framePushUs > 0
                       ? 1000000.0 / (d.frameDrawUs + d.framePushUs)
                       : 0;
  gfx["period_ms"] = d.framePeriodUs / 1000.0;
  gfx["fps_real"] = d.framePeriodUs > 0 ? 1000000.0 / d.framePeriodUs : 0;
  gfx["radar_frame"] = d.radarFrame;
  gfx["radar_min"] = d.radarFrameMin;
  gfx["spi_hz"] = 80000000;   // z User_Setup.h

  // --- v111: "memfull" — WSZYSTKIE rodzaje pamieci, dla ekranu PAMIEC. Zeby dalo
  // sie zweryfikowac ten ekran zdalnie (bez patrzenia na urzadzenie), tu leza TE
  // SAME wywolania ESP-IDF, ktore rysuje WeatherUi::drawViewMem() — patrz komentarze
  // przy tamtej funkcji co do znaczenia largest_block/min_ever/partycji/RTC/ROM.
  // `mem` wyzej (sram_free/sram_min/sram_block/psram_*) zostaje NIETKNIETY — to
  // sekcja dodatkowa, nie zamiennik.
  JsonObject mf = j["memfull"].to<JsonObject>();

  JsonObject mfSram = mf["sram"].to<JsonObject>();
  mfSram["free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  mfSram["largest_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);  // fragmentacja
  mfSram["min_ever"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);        // dolek od startu

  JsonObject mfPsram = mf["psram"].to<JsonObject>();
  mfPsram["total"] = ESP.getPsramSize();
  mfPsram["free"] = ESP.getFreePsram();
  mfPsram["largest_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  mfPsram["min_ever"] = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

  JsonObject mfFlash = mf["flash"].to<JsonObject>();
  mfFlash["chip_size"] = ESP.getFlashChipSize();
  mfFlash["sketch_size"] = ESP.getSketchSize();   // dzialajacy firmware, z naglowkiem
  // UWAGA: to rozmiar DRUGIEJ partycji OTA (esp_ota_get_next_update_partition), NIE
  // "ile zostalo w mojej partycji" — w tym schemacie (app0==app1) to zawsze ~1,9 MB,
  // niezaleznie od tego, jak duzy jest dzisiejszy firmware. Realne zapelnienie
  // partycji jest w "app" nizej (used/size).
  mfFlash["free_sketch_space"] = ESP.getFreeSketchSpace();

  const esp_partition_t* runningPart = esp_ota_get_running_partition();
  JsonObject mfApp = mf["app"].to<JsonObject>();
  mfApp["running"] = runningPart ? runningPart->label : "?";
  mfApp["used"] = ESP.getSketchSize();
  mfApp["size"] = runningPart ? runningPart->size : 0;

  JsonArray parts = mf["partitions"].to<JsonArray>();
  auto addPart = [&](const char* name, esp_partition_type_t type, esp_partition_subtype_t sub) {
    const esp_partition_t* p = esp_partition_find_first(type, sub, nullptr);
    JsonObject po = parts.add<JsonObject>();
    po["name"] = name;
    po["present"] = p != nullptr;
    po["size"] = p ? p->size : 0;
    po["address"] = p ? p->address : 0;
  };
  addPart("nvs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS);
  addPart("otadata", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA);
  addPart("app0", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0);
  addPart("app1", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1);
  addPart("spiffs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS);   // zarezerwowany, nieuzywany
  addPart("coredump", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP);

  // To samo, co j["coredump"]["present"] wyzej (coredumpInfo()) — zdublowane tu
  // SWIADOMIE, zeby "memfull" bylo samodzielnym, kompletnym zrzutem pamieci bez
  // koniecznosci zagladania w inna sekcje tej odpowiedzi.
  size_t coreAddr = 0, coreSize = 0;
  mf["coredump_present"] = esp_core_dump_image_get(&coreAddr, &coreSize) == ESP_OK && coreSize > 0;

  JsonObject mfRtc = mf["rtc"].to<JsonObject>();
  mfRtc["slow_physical"] = 8192;   // fizyczny rozmiar RTC SLOW — stala sprzetowa ESP32-S3
  mfRtc["slow_usable"] = 7680;     // realny rozmiar sekcji .rtc_noinit — patrz Log.h przy PirRtc
  mfRtc["slow_used"] = sizeof(PirRtc) + sizeof(LdrRtc);   // gPir + gLdr
  mfRtc["fast_physical"] = 8192;   // RTC FAST — NIEUZYWANE w tym projekcie, patrz Log.h

  mf["rom_size"] = 393216;   // 384 kB, bootrom ESP32-S3 — stala sprzetowa, tylko do odczytu

  JsonObject mfStack = mf["stack"].to<JsonObject>();
  mfStack["net_spare"] = d.stackNet;    // to samo, co mem.stack_net_spare wyzej
  mfStack["web_spare"] = d.stackWeb;    // to samo, co mem.stack_web_spare wyzej
  mfStack["configured_size"] = 16384;   // xTaskCreatePinnedToCore(...,16384,...) w .ino, oba zadania

  JsonObject o = j["ota"].to<JsonObject>();
  o["remote"] = d.otaRemote;
  o["msg"] = d.otaMsg;
  // Okres próbny po aktualizacji (patrz OtaGuard.h).
  o["trial"] = d.otaTrial == 1 ? "probna" : (d.otaTrial == 2 ? "potwierdzona" : "stabilna");
  o["trial_active"] = otaTrialActive();
  o["confirmed_ago_s"] = ago(d.otaConfirmAt);
  o["rejected_version"] = d.otaBadVersion;   // 0 = brak zablokowanej wersji

  // bez hasla i bez uzytkownika — /api/diag bywa wklejane do zgloszen bledow
  JsonObject m = j["mqtt"].to<JsonObject>();
  m["enabled"] = settings().mqttEnabled;
  m["host"] = settings().mqttHost;
  m["port"] = settings().mqttPort;
  m["prefix"] = settings().mqttPrefix;
  m["ok_ago_s"] = ago(d.mqttOkAt);
  m["connects"] = d.mqttConnects;
  m["published"] = d.mqttPublished;
  m["err"] = d.mqttErr;

  // --- czujniki v100 (surowy odczyt do testu) ---
  // ldr_raw rosnie z jasnoscia (dzielnik: jasno => R_LDR male => napiecie wyzsze).
  // pir_last_s: ile sekund temu AM312 ostatnio zglosil ruch (-1 = od startu nic).
  JsonObject sen = j["sensors"].to<JsonObject>();
  sen["ldr_raw"] = d.ldrRaw;
  sen["ldr_mv"] = d.ldrMv;
  // Co kod REALNIE wystawia na PWM podswietlenia. Rozstrzyga spor "ekran sie nie
  // przyciemnia": jesli bl_current spada, a ekran swieci pelnia — problem jest
  // w okablowaniu pinu, nie w logice (patrz WeatherUi::backlightCurrent()).
  if (gBlGet != nullptr) {
    uint8_t blCur = 0, blTgt = 0;
    gBlGet(blCur, blTgt);
    sen["bl_current"] = blCur;
    sen["bl_target"] = blTgt;
  }
  sen["pir"] = d.pirState;
  sen["pir_last_s"] = ago(d.pirLastAt);

  // --- PIR: pomiar zachowania AM312 (liczniki z ISR, PRZEZYWAJA OTA) ---
  // Po co akurat te pola — patrz PirRtc w Log.h. Skrot: log to okno ~6 minut, a pytanie
  // wlasciciela ("kto i kiedy realnie korzysta z lazienki i czy czujnik to wiernie
  // oddaje") wymaga TYGODNIA. Od tej wersji liczniki siedza w RTC, wiec wydanie OTA ich
  // NIE kasuje — a przez ostatnia dobe wyszlo piec wersji.
  // Jak tego uzyc bez czekania tydzien: odczytaj /api/diag, wejdz pod prysznic, odczytaj
  // ponownie — roznica pir_pulses i pir_total_s mowi wprost, czy para wyzwala czujnik.
  //
  // "zbieram od" kontra uptime: uptime_s zeruje sie po KAZDYM restarcie, pomiar leci
  // dalej. Do normowania czegokolwiek (impulsy na godzine) sluzy pir_collected_s, NIGDY
  // uptime_s — ta pomylka jest tu najlatwiejsza do zrobienia i najdrozsza.
  JsonObject pir = sen["pir_meas"].to<JsonObject>();
  pir["collected_s"] = gPir.collectedS;   // REALNY czas zbierania (bez przerw na restarty)
  pir["boots"] = gPir.boots;              // ile restartow przezyl ten komplet danych
  // startedEpoch = 0 => NTP jeszcze nigdy nie dal czasu, wiec poczatku nie znamy. Brak
  // danych ma wygladac jak brak danych (ta sama lekcja co viHas* i pir_min_ms nizej).
  if (gPir.startedEpoch > 0) {
    pir["started_epoch"] = gPir.startedEpoch;
    // Ile czasu ZEGAROWEGO minelo od startu zbierania. Roznica wzgledem collected_s to
    // czas, w ktorym urzadzenie sie restartowalo i NIC nie mierzylo — czyli wprost
    // odpowiedz na "czy w tym tygodniu sa dziury i jak duze".
    const time_t nowT = time(nullptr);
    if (nowT > 1700000000) {
      const uint32_t wall = static_cast<uint32_t>(nowT) - gPir.startedEpoch;
      pir["wall_s"] = wall;
      pir["gap_s"] = wall > gPir.collectedS ? wall - gPir.collectedS : 0;
    }
  } else {
    pir["started_epoch"] = nullptr;
  }

  sen["pir_pulses"] = gPir.pulses;
  sen["pir_rises"] = gPir.rises;   // wyzwolenia; to ich rozklad siedzi w pir_by_hour
  sen["pir_edges"] = gPir.edges;   // czysto => edges == 2*pulses (+1 gdy impuls wlasnie trwa)
  sen["pir_last_ms"] = gPir.lastMs;
  // 0xFFFFFFFF = jeszcze zadnego pelnego impulsu. Wyslanie tego surowo czytaloby sie jak
  // "najkrotszy impuls trwal 49 dni", wiec zamiast tego null — brak danych ma wygladac
  // jak brak danych. Ta sama lekcja co viHas* w Log.h.
  if (gPir.pulses > 0) {
    sen["pir_min_ms"] = gPir.minMs;
    sen["pir_max_ms"] = gPir.maxMs;
  } else {
    sen["pir_min_ms"] = nullptr;
    sen["pir_max_ms"] = nullptr;
  }
  sen["pir_total_s"] = gPir.totalMs / 1000;   // udzial doby: podziel przez pir_collected_s

  // Granice koszy jada W ODPOWIEDZI, nie tylko w komentarzu — czytajacy JSON w przegladarce
  // ma widziec, co znaczy ktora liczba, bez zagladania w zrodlo. Ten plik ma juz za soba
  // trzy komentarze, ktore rozjechaly sie z kodem; opis obok danych rozjezdza sie trudniej.
  JsonArray wh = sen["pir_width_ms"].to<JsonArray>();
  for (uint32_t v : gPir.widthHist) wh.add(v);
  sen["pir_width_bins"] = "<100|100-1k|1k-3k|3k-10k|10k-60k|>=60k ms";

  JsonArray gh = sen["pir_gap_ms"].to<JsonArray>();
  for (uint32_t v : gPir.gapHist) gh.add(v);
  sen["pir_gap_bins"] = "<2k|2k-5k|5k-15k|15k-60k|60k-300k|300k-1800k|>=1800k ms";

  // Rytm doby: wyzwolenia w kazdej godzinie czasu LOKALNEGO, indeks 0..23 = godzina.
  // Liczone tylko wtedy, gdy NTP dal czas — suma pir_by_hour bywa wiec MNIEJSZA niz
  // pir_rises i to nie jest blad, tylko te zbocza, ktorych godziny nie znalismy.
  JsonArray bh = sen["pir_by_hour"].to<JsonArray>();
  for (uint32_t v : gPir.byHour) bh.add(v);
  sen["pir_by_hour_note"] = "indeks = godzina lokalna 0-23; normuj przez collected_s, nie uptime";

  // --- LDR: obserwacja v108 (histogram + poziomy + zdarzenia, PRZEZYWAJA OTA) ---
  // Po co kazde z tych pol — patrz LdrRtc w Log.h. Skrot: w tej lazience sa dwa zrodla
  // swiatla, samo swiatlo pod prysznicem (603-617 mV) NIE dosiega progu LDR_DIM_UP_MV
  // (650) i nie podnosi ekranu z poziomu 0. Wlasciciel chce, zeby podnosilo. Ta wersja
  // NICZEGO nie zmienia — zbiera tydzien danych, zeby prog postawic na rozkladzie, a nie
  // na jednym pomiarze zrobionym w locie (tak powstal blad v103, patrz komentarz przy
  // logice podswietlenia w pogoda-gdynia.ino).
  //
  // OSOBNY ldr_meas, a nie wspolny z pir_meas: liczniki maja NIEZALEZNIE przezywac
  // podbicie cudzego magica. Po zimnym starcie obie sekcje pokaza to samo i tak ma byc.
  JsonObject lm = sen["ldr_meas"].to<JsonObject>();
  lm["collected_s"] = gLdr.collectedS;   // REALNY czas zbierania — TYM normuj histogram
  lm["boots"] = gLdr.boots;
  if (gLdr.startedEpoch > 0) {
    lm["started_epoch"] = gLdr.startedEpoch;
    const time_t nowT = time(nullptr);
    if (nowT > 1700000000) {
      const uint32_t wall = static_cast<uint32_t>(nowT) - gLdr.startedEpoch;
      lm["wall_s"] = wall;
      // Roznica wall_s - collected_s = ile pomiaru zjadly restarty. To jedyna liczba,
      // ktora mowi, na ile temu histogramowi mozna ufac po tygodniu wydan OTA.
      lm["gap_s"] = wall > gLdr.collectedS ? wall - gLdr.collectedS : 0;
    }
  } else {
    lm["started_epoch"] = nullptr;   // NTP jeszcze nigdy nie dal czasu
  }

  // Histogram jasnosci. Probka co 250 ms => 4 zliczenia na sekunde: kosz/4 = SEKUNDY.
  // Suma wszystkich koszy ~= 4 * collected_s (z dokladnoscia do reszty ostatniej sekundy).
  JsonArray lh = sen["ldr_hist"].to<JsonArray>();
  for (uint32_t v : gLdr.hist) lh.add(v);
  sen["ldr_hist_bins"] =
      "<8|8-16|16-32|32-64|64-128|128-256|256-384|384-512|512-640|640-768|768-1024|"
      "1024-1536|1536-2048|2048-2560|2560-3072|>=3072 mV";
  // To zdanie jest tu PO TO, zeby czytajacy za pol roku wiedzial, czego szukac, i nie
  // musial odtwarzac calego kontekstu z pamieci. Kosze 8 i 9 sa cala stawka.
  sen["ldr_hist_note"] =
      "4 probki/s => kosz/4 = sekundy; kosze zageszczone w strefie spornej 256-768 mV. "
      "Pytanie: czy sam prysznic (zmierzone 603-617) daje CZYSTY PIK w koszu 512-640 "
      "(=> prog 450 bezpieczny), czy rozmazuje sie na kosze 7-9 (=> zaden staly prog nie "
      "zadziala). Prog LDR_DIM_UP_MV=650 przecina kosz 640-768.";

  // Sekundy na poziomach podswietlenia. Indeks = poziom: 0 ciemno (BL_NIGHT=45),
  // 1 polmrok (BL_DIM=130), 2 swiatlo (BL_DAY=255).
  JsonArray ll = sen["ldr_level_s"].to<JsonArray>();
  for (uint32_t v : gLdr.levelS) ll.add(v);
  // Suma podana WPROST, bo jest MNIEJSZA niz collected_s i ta roznica jest mylaca, gdy sie
  // o niej nie wie: poziomy licza sie tylko wtedy, gdy petla naprawde nimi steruje (nie
  // w portalu AP, nie podczas OTA, nie na ekranie startowym). Patrz komentarz przy tym
  // liczniku w pogoda-gdynia.ino.
  sen["ldr_level_s_sum"] = gLdr.levelS[0] + gLdr.levelS[1] + gLdr.levelS[2];
  sen["ldr_level_s_note"] =
      "0=ciemno(45) 1=polmrok(130) 2=swiatlo(255). Suma < collected_s o czas w portalu/OTA/"
      "boocie i to nie jest blad. Pytanie: czy poziom 1 w ogole istnieje dluzej niz "
      "pstrykniecie — jesli ~0 s, to BL_DIM jest martwym kodem do usuniecia.";

  // Zdarzenia "zostawione swiatlo". PRYWATNOSC: to jedyne pole tej sekcji ze znacznikiem
  // czasu — przed wklejeniem /api/diag do publicznego zgloszenia wytnij je razem
  // z pir_by_hour (patrz nota o prywatnosci przy LdrRtc w Log.h).
  JsonArray le = sen["ldr_events"].to<JsonArray>();
  // Pierscien nadpisuje najstarsze, wiec kolejnosc w tablicy != kolejnosc w czasie.
  // Rozwijamy od najstarszego zachowanego, zeby czytalo sie chronologicznie: dopoki
  // evCount <= 8 nic sie nie zawinelo i zaczynamy od 0, potem najstarszy siedzi w evHead.
  const uint32_t evN = gLdr.evCount < 8 ? gLdr.evCount : 8;
  const uint32_t evFirst = gLdr.evCount < 8 ? 0 : gLdr.evHead;
  for (uint32_t i = 0; i < evN; ++i) {
    const uint32_t idx = (evFirst + i) % 8;
    const LdrEvent& e = gLdr.events[idx];
    JsonObject eo = le.add<JsonObject>();
    // 0 => ciaglosc zaczela sie przed pierwszym NTP i godziny NIE ZNAMY. Brak danych ma
    // wygladac jak brak danych — ta sama lekcja co viHas* i pir_min_ms wyzej.
    if (e.startEpoch > 0) eo["start_epoch"] = e.startEpoch;
    else eo["start_epoch"] = nullptr;
    eo["dur_s"] = e.durS;
    // 0xFFFFFFFF => od startu urzadzenia nie bylo ANI JEDNEGO ruchu, czyli "nie wiem".
    // Surowo czytaloby sie to jak "ostatni ruch byl 136 lat temu".
    if (e.lastPirBeforeS != 0xFFFFFFFF) eo["last_pir_before_s"] = e.lastPirBeforeS;
    else eo["last_pir_before_s"] = nullptr;
    // Zdarzenie trafia do pierscienia W CHWILI dobicia 20 minut, a nie po zgasnieciu
    // swiatla — wiec dur_s otwartego wciaz rosnie i bez tej flagi wygladaloby jak
    // zamkniete. Bez niej "swieci sie TERAZ od godziny" i "swiecilo sie godzine wczoraj"
    // sa nierozroznialne, a to jest cala roznica miedzy alarmem a statystyka.
    if (gLdr.evSlot == idx) eo["open"] = true;
  }
  sen["ldr_events_total"] = gLdr.evCount;   // > 8 => pierscien sie zawinal, najstarsze przepadly
  sen["ldr_events_note"] =
      "zdarzenie = ldr_mv >= 400 nieprzerwanie ORAZ zero ruchu PIR przez > 20 min. "
      "Prog 400 mV jest SUROWY i celowo niezalezny od progow podswietlenia (LDR_*), bo to "
      "wlasnie one sa przedmiotem sporu — detektor nie moze zalezec od wielkosci, ktora "
      "pomiar ma rozstrzygnac. dur_s liczy sie w collected_s, wiec restart pauzuje "
      "zdarzenie, zamiast doliczac czas, ktorego nie widzielismy.";

  String out;
  serializeJsonPretty(j, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void apiReboot() {
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Restartuję\"}");
  delay(300);
  ESP.restart();
}

void apiScreen() {
  if (gScreenshot == nullptr) {
    server.send(503, "text/plain", "brak zrzutu");
    return;
  }
  WiFiClient c = server.client();
  gScreenshot(c);
}

// --- zrzut awaryjny (coredump) -----------------------------------------------
//
// PRYWATNOSC — powod, dla ktorego ten blok jest zbudowany wlasnie tak.
// Surowy zrzut to KOPIA PAMIECI z chwili panica: stosy wszystkich zadan, ich TCB
// i rejestry. Moga w nim lezec haslo WiFi, login i haslo MQTT, bindkeye BLE oraz token
// i refresh token Viessmanna. To cos zupelnie innego niz /api/diag, ktore wystawia
// starannie dobrane flagi i liczby. Repozytorium tego projektu jest PUBLICZNE.
//
// Dlatego samo ostrzezenie to za malo i zrobione sa trzy rzeczy wiecej:
//
//  1. BEZPIECZNA SCIEZKA ODPOWIADA NA PYTANIE. "Dlaczego padlo" (zadanie, adres upadku,
//     backtrace, panic reason) jest w /api/diag -> coredump, bez ani jednego bajtu
//     zawartosci pamieci. W typowym sledztwie caly plik NIE JEST POTRZEBNY — a sciezka,
//     po ktora nikt nie siega, nie wycieknie. To jest ta prawdziwa ochrona, nie napis.
//  2. OSTRZEZENIE JEDZIE Z PLIKIEM. Nazwa pobranego pliku to
//     "coredump-PRYWATNE-NIE-PUBLIKOWAC.bin". Ostrzezenie w JSON-ie albo w naglowku HTTP
//     zostaje w przegladarce; nazwa pliku jest widoczna jeszcze wtedy, gdy ktos pol roku
//     pozniej przeciaga zalacznik do zgloszenia na GitHubie. Tylko ona tam dociera.
//  3. POBRANIE JEST OSOBNE I JAWNE. Metadane sa pod /api/coredump, a bajty dopiero pod
//     /api/coredump/raw — nie da sie wyciagnac pamieci urzadzenia "przy okazji" klikniecia
//     w diagnostyke.
//
// Czego NIE zrobiono, swiadomie: nie probujemy wycinac sekretow ze zrzutu. Nie da sie
// wiarygodnie znalezc hasla w surowych stosach i TCB, a filtr, ktory przepusci jedno na
// dziesiec, jest GORSZY od braku filtra — dalby falszywe poczucie, ze plik jest czysty.
// Uczciwiej: plik jest brudny zawsze i tak jest opisany.

// Wspolny wstep dla /raw i DELETE: znajduje obraz i partycje.
// esp_core_dump_image_get() daje ADRES BEZWZGLEDNY we flashu, a esp_partition_read()
// chce PRZESUNIECIA wzgledem poczatku partycji — bez tej zamiany czytalibysmy 4 MB obok.
// Przy okazji sprawdzamy, czy obraz w calosci miesci sie w partycji: jesli nie, cos jest
// nie tak i lepiej nie wyslac w swiat przypadkowego kawalka flasha.
bool coredumpLocate(const esp_partition_t** part, size_t* base, size_t* size) {
  size_t addr = 0;
  if (esp_core_dump_image_get(&addr, size) != ESP_OK || *size == 0) {
    return false;
  }
  *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                   ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (*part == nullptr || addr < (*part)->address ||
      addr + *size > (*part)->address + (*part)->size) {
    return false;
  }
  *base = addr - (*part)->address;
  return true;
}

// Metadane + instrukcja obslugi. Tu sie laduje i stad wiadomo, co dalej.
void apiCoredump() {
  JsonDocument j;
  coredumpInfo(j.to<JsonObject>());
  if (j["present"].as<bool>()) {
    j["uwaga"] =
        "Plik z /api/coredump/raw to kopia pamieci urzadzenia: moga w nim byc hasla WiFi "
        "i MQTT, bindkeye BLE oraz token Viessmanna. NIE WOLNO go wkleic do publicznego "
        "zgloszenia bledu ani wrzucic na GitHuba. Zadanie, adres upadku i backtrace masz "
        "wyzej w tej odpowiedzi (i w /api/diag) — TO jest bezpieczne do wklejenia.";
    // Format: to NIE jest goly ELF, tylko obraz zrzutu = naglowek + ELF + CRC32.
    // Dlatego espcoredump.py wymaga --core-format raw; podanie tego pliku jako ELF-a
    // konczy sie bledem parsowania i szukaniem winy nie tam, gdzie trzeba.
    j["howto"] =
        "curl -o coredump.bin http://<ip>/api/coredump/raw ; "
        "esp-coredump info_corefile --core coredump.bin --core-format raw --chip esp32s3 "
        "<app.elf z DOKLADNIE tej wersji, ktora padla — patrz app_elf_sha> ; "
        "po zdiagnozowaniu: curl -X DELETE http://<ip>/api/coredump";
  }
  String out;
  serializeJson(j, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

// Surowy obraz zrzutu, strumieniowo.
//
// RAM: partycja ma 64 kB, a zapasu w statycznym RAM jest ~3 kB — obrazu NIE WOLNO wczytac
// w calosci ani do RAM-u, ani do String-a (String zrobilby dokladnie to samo, tylko na
// stercie). Czytamy z flasha i wysylamy kawalkami po 512 B: w pamieci jest zawsze jeden
// bufor na stosie zadania web (zapas 12,7 kB), niezaleznie od tego, czy zrzut ma 8 kB czy
// 64 kB. Ten sam wzorzec strumieniowania kawalkami co /api/screen (JPEG 320x240, a gdy
// enkoder/PSRAM zawiedzie — awaryjnie BMP wiersz po wierszu).
//
// Zaden mutex nie jest tu brany: to czysty odczyt z flasha, gLock nie chroni partycji
// coredump, a trzymanie go przez ~128 obrotow petli z I/O zablokowaloby netTask i rysowanie.
void apiCoredumpRaw() {
  const esp_partition_t* part = nullptr;
  size_t base = 0, size = 0;
  if (!coredumpLocate(&part, &base, &size)) {
    server.send(404, "text/plain; charset=utf-8",
                "Brak zrzutu awaryjnego (albo obraz nie miesci sie w partycji).\n");
    return;
  }

  // Nazwa pliku jest jedynym ostrzezeniem, ktore zostanie z plikiem po pobraniu.
  server.sendHeader("Content-Disposition",
                    "attachment; filename=\"coredump-PRYWATNE-NIE-PUBLIKOWAC.bin\"");
  server.sendHeader("X-Coredump-Warning",
                    "Kopia pamieci urzadzenia: moga tu byc hasla WiFi/MQTT, bindkeye BLE "
                    "i token Viessmanna. Nie publikowac.");
  server.sendHeader("Cache-Control", "no-store");
  // Znany Content-Length => odpowiedz idzie bez kodowania kawalkowego, wiec curl i
  // przegladarka pokazuja pasek postepu, a uciety transfer widac od razu.
  server.setContentLength(size);
  server.send(200, "application/octet-stream", "");

  uint8_t buf[512];
  WiFiClient client = server.client();
  for (size_t off = 0; off < size; off += sizeof(buf)) {
    const size_t n = (size - off) < sizeof(buf) ? (size - off) : sizeof(buf);
    if (esp_partition_read(part, base + off, buf, n) != ESP_OK) {
      break;   // urwiemy transfer: krotszy niz Content-Length = klient widzi blad, i dobrze
    }
    if (!client.connected()) {
      break;   // ktos zamknal karte w trakcie — nie ma po co czytac reszty flasha
    }
    server.sendContent(reinterpret_cast<const char*>(buf), n);
  }
}

// Kasowanie. HTTP_DELETE, nie GET: to zmiana stanu, a GET-em umie ja wywolac cudza strona
// otwarta w tej samej sieci (<img src=...>) — dokladnie ten powod, dla ktorego /api/vi/set
// jest na POST. Tu stawka jest inna, ale rownie nieprzyjemna: skasowanie jedynego dowodu
// na to, dlaczego urzadzenie padlo, zanim ktokolwiek zdazyl go przeczytac.
void apiCoredumpErase() {
  const esp_err_t err = esp_core_dump_image_erase();
  JsonDocument o;
  o["ok"] = err == ESP_OK;
  o["err"] = err == ESP_OK ? "" : esp_err_to_name(err);
  String out;
  serializeJson(o, out);
  server.send(200, "application/json", out);
}

// Test podswietlenia: /api/bl?v=0..255&ms=5000 — wymus jasnosc na chwile, potem
// automat z LDR wraca sam. Po co: wlasciciel zglosil, ze ekran sie nie przyciemnia,
// a kod wyglada poprawnie. Jesli po tym wywolaniu ekran NIE zmieni jasnosci, to pin
// podswietlenia nie jest sterowany z GPIO i zaden software tego nie naprawi.
// Ograniczenie czasowe jest CELOWE — urzadzenie nie ma klawiatury, wiec trwale 0
// zostawiloby czarny ekran bez drogi powrotu.
void apiBacklight() {
  if (gBlTest == nullptr) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"brak obslugi\"}");
    return;
  }
  // Panel V3, pigulka "Auto (czujnik)": zwolnij wymuszenie i oddaj sterowanie automatowi
  // LDR. testBacklight() nie ma osobnego "zwolnij", wiec forsujemy na 1 ms, nastepny
  // tickBacklight() kasuje blForceUntil_ i automat wraca (WeatherUi.cpp). Krok rampy to
  // 6/255, wiec ten jeden takt jest niewidoczny; wartosc 128 nie ma znaczenia.
  if (server.hasArg("v") && server.arg("v") == "auto") {
    gBlTest(128, 1);
    server.send(200, "application/json", "{\"ok\":true,\"v\":\"auto\"}");
    return;
  }
  const int v = server.hasArg("v") ? server.arg("v").toInt() : 255;
  long ms = server.hasArg("ms") ? server.arg("ms").toInt() : 5000;
  if (ms < 500) ms = 500;
  // Panel V3 forsuje jasnosc na kilka godzin: override ma TRZYMAC, dopoki uzytkownik nie
  // kliknie "Auto". Bezpieczne: najnizsza pigulka to 18% (45/255), NIGDY 0 (ekran wisi
  // bez klawiatury, czerni nie wolno zatrzasnac), a forsowanie i tak samo wygasa po 6 h.
  if (ms > 21600000L) ms = 21600000L;   // 6 h sufitu (bylo 30 s, to bylo dla samego testu sprzetu)
  gBlTest(static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)), static_cast<uint32_t>(ms));
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"v\":%d,\"ms\":%ld}",
           v < 0 ? 0 : (v > 255 ? 255 : v), ms);
  server.send(200, "application/json", buf);
}

// Przelacznik wygladu V1/V2. Dziala NATYCHMIAST bez restartu: settings().setTheme()
// pisze do NVS od razu (patrz Settings.cpp), a nastepna klatka juz czyta nowa
// wartosc — tyle ze na razie NIC jej jeszcze nie czyta (ten etap buduje tylko
// przelacznik i prymitywy V2, patrz ThemeV2.h; podlaczenie do rysowania widokow to
// kolejny krok). v spoza {1,2} zostaje bez zmian — endpoint zwraca wtedy aktualny,
// niezmieniony stan, a nie zamrozony/domyslny, zeby panel zawsze pokazal PRAWDE.
void apiTheme() {
  const int v = server.hasArg("v") ? server.arg("v").toInt() : 0;
  const bool ok = (v >= 1 && v <= 3) && settings().setTheme(static_cast<uint8_t>(v));
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"ok\":%s,\"theme\":%d}", ok ? "true" : "false",
           settings().theme);
  server.send(200, "application/json", buf);
}

// Ustawienia wyswietlacza edytowalne z panelu: okno trybu nocnego (nightStart/nightEnd),
// czas jednego ekranu w rotacji (dwell [s]) i trzy poziomy jasnosci (blDay/blDim/blNight).
// Kazdy argument opcjonalny — brakujacy zostaje bez zmian (bierzemy biezaca wartosc).
// Walidacja i CLAMP siedza w Settings::saveTuning()/clampTuning(): w szczegolnosci
// TWARDE minimum jasnosci (dzien 60 / polmrok 30 / noc 15), zeby panel NIE MOGL zgasic
// ekranu na stale — urzadzenie wisi bez klawiatury i z czerni nie ma jak wrocic.
// Dziala NATYCHMIAST bez restartu (settings() czytane na biezaco przez rysowanie i
// backlight), jak przelacznik motywu. Zwracamy realnie zapisane wartosci, zeby panel
// pokazal PRAWDE (np. jasnosc podbita do minimum).
void apiTuning() {
  Settings& st = settings();
  const int nStart = server.hasArg("nightStart") ? server.arg("nightStart").toInt() : st.nightStartH;
  const int nEnd   = server.hasArg("nightEnd")   ? server.arg("nightEnd").toInt()   : st.nightEndH;
  const int dwell  = server.hasArg("dwell")      ? server.arg("dwell").toInt()      : st.dwellS;
  const int bDay   = server.hasArg("blDay")      ? server.arg("blDay").toInt()      : st.blDay;
  const int bDim   = server.hasArg("blDim")      ? server.arg("blDim").toInt()      : st.blDim;
  const int bNight = server.hasArg("blNight")    ? server.arg("blNight").toInt()    : st.blNight;
  // Auto-rotacja V3: 0/1. Panel zawsze wysyla wprost (&arot=0/1 w saveTune), wiec brak
  // argu == "bez zmian" (biezaca wartosc). TYLKO motyw V3 to czyta (render); V1/V2 nie.
  const bool arot  = server.hasArg("arot")       ? (server.arg("arot").toInt() != 0) : st.autoRotate;

  auto u8 = [](int v) -> uint8_t { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
  auto u16 = [](int v) -> uint16_t { return static_cast<uint16_t>(v < 0 ? 0 : (v > 65535 ? 65535 : v)); };
  const bool ok = st.saveTuning(u8(nStart), u8(nEnd), u16(dwell), u8(bDay), u8(bDim), u8(bNight), arot);

  char buf[224];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"night_start\":%u,\"night_end\":%u,\"dwell\":%u,"
           "\"bl_day\":%u,\"bl_dim\":%u,\"bl_night\":%u,\"arot\":%u}",
           ok ? "true" : "false", static_cast<unsigned>(st.nightStartH),
           static_cast<unsigned>(st.nightEndH), static_cast<unsigned>(st.dwellS),
           static_cast<unsigned>(st.blDay), static_cast<unsigned>(st.blDim),
           static_cast<unsigned>(st.blNight), st.autoRotate ? 1u : 0u);
  server.send(200, "application/json", buf);
}

// Wizualny test podswietlenia: /api/blsweep?ms=60000 — ekran przejmuje tryb testowy
// (duza liczba PWM + rampa w gore i w dol), zeby dalo sie GOLYM OKIEM sprawdzic, czy
// sterowanie jasnoscia w ogole dziala. Sam wygasa.
void apiBacklightSweep() {
  if (gBlSweep == nullptr) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"brak obslugi\"}");
    return;
  }
  long ms = server.hasArg("ms") ? server.arg("ms").toInt() : 60000;
  if (ms < 5000) ms = 5000;
  if (ms > 300000) ms = 300000;   // 5 min sufitu — test ma sam sie skonczyc
  gBlSweep(static_cast<uint32_t>(ms));
  char buf[80];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"ms\":%ld}", ms);
  server.send(200, "application/json", buf);
}

void apiView() {
  if (gViewSet != nullptr && server.hasArg("i")) {
    gViewSet(server.arg("i").toInt());
  }
  int cur = 0, pin = -1;
  if (gViewGet != nullptr) gViewGet(cur, pin);

  char buf[48];
  snprintf(buf, sizeof(buf), "{\"cur\":%d,\"pin\":%d}", cur, pin);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

// Symulacja dotkniecia pinu GPIO7 z panelu: n=1 dziala jak pojedyncze stukniecie
// (restart odliczania ekranu), n=2 jak podwojne (poprzedni ekran) — DOKLADNIE to,
// co robi fizyczny dotyk w petli glownej. Sluzy do sprawdzenia zachowania dotyku
// bez podchodzenia do urzadzenia.
void apiTap() {
  const int n = server.hasArg("n") ? server.arg("n").toInt() : 1;
  if (gTap != nullptr) gTap(n);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":true}");
}

// Lista wykrytych czujnikow BLE. Klucza NIE zwracamy — tylko flage, ze jest.
void apiBleList() {
  JsonDocument j;
  JsonArray arr = j.to<JsonArray>();

  for (int i = 0; i < ble::count(); ++i) {
    const ble::Sensor s = ble::get(i);
    if (s.mac[0] == '\0') continue;

    const Settings::BleCfg* cfg = settings().bleFind(s.mac);
    JsonObject o = arr.add<JsonObject>();
    // UWAGA: String(), nie s.mac. ArduinoJson dla `const char*` zapamietuje sam
    // WSKAZNIK, a `s` to kopia na stosie, ktora ginie po tej iteracji — wszystkie
    // wpisy pokazywaly wtedy ten sam, martwy adres (dwa czujniki, jeden MAC).
    o["mac"] = String(s.mac);
    o["name"] = String(cfg ? cfg->name : "");
    o["hasKey"] = cfg ? cfg->hasKey : false;
    o["valid"] = s.valid;
    o["needsKey"] = s.needsKey;
    o["t"] = s.tempC;
    o["h"] = s.humidity;
    o["bat"] = s.batteryPct;
    o["rssi"] = s.rssi;
    o["age_s"] = s.seenAt ? (millis() - s.seenAt) / 1000 : -1;
    o["gw"] = s.viaGw;
    o["rssi_own"] = s.rssiOwn;      // 0 = nasze radio go nie slyszy
    o["rssi_gw"] = s.rssiGw;
    o["own_age"] = s.ownAt ? (millis() - s.ownAt) / 1000 : -1;
    o["gw_age"] = s.gwAt ? (millis() - s.gwAt) / 1000 : -1;

    // Co slyszy KAZDA bramka z osobna — to jest narzedzie do rozstawiania bramek
    // i czujnikow. Bez tego przy trzech bramkach widac tylko wynik arbitrazu, czyli
    // nie da sie stwierdzic, czy nowa bramka na parterze cokolwiek poprawila.
    o["gw_own"] = blegw::ownerOf(s.mac);   // indeks bramki-opiekuna, -1 = zadna
    JsonArray g = o["gw_rssi"].to<JsonArray>();
    for (int k = 0; k < blegw::SLOTS; ++k) g.add(blegw::rssiOf(s.mac, k));
  }

  String out;
  serializeJson(j, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void apiBleSet() {
  JsonDocument b;
  if (deserializeJson(b, server.arg("plain")) != DeserializationError::Ok) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"zly JSON\"}");
    return;
  }

  const char* mac = b["mac"] | "";
  const char* name = b["name"] | "";
  const char* key = b["key"] | "";

  if (key[0] != '\0' && strcmp(key, "-") != 0 && strlen(key) != 32) {
    server.send(200, "application/json",
                "{\"ok\":false,\"msg\":\"Bindkey ma dokładnie 32 znaki szesnastkowe\"}");
    return;
  }

  const bool ok = settings().bleSet(mac, name, key);
  if (ok) {
    server.send(200, "application/json",
                "{\"ok\":true,\"msg\":\"Zapisano. Dane pojawią się po najbliższym "
                "nasłuchu (do 45 s).\"}");
    return;
  }
  // Liczba MUSI pochodzic ze stalej, nie z literalu. Ten komunikat mowil "maks. 4"
  // w czasach, gdy bleSet() odrzucal dopiero przy 8 — czyli byl nieosiagalny i
  // nieprawdziwy naraz. Dzis limit to BLE_USABLE i tekst musi za nim nadazac sam.
  char msg[96];
  snprintf(msg, sizeof(msg),
           "{\"ok\":false,\"msg\":\"Brak wolnego miejsca (maks. %d czujników)\"}",
           Settings::BLE_USABLE);
  server.send(200, "application/json", msg);
}

// Symulacja opadu — sztuczny front do obejrzenia wizualizacji, gdy nie pada.
void apiRadarDemo() {
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  radarmap::setDemo(on);
  server.send(200, "application/json",
              on ? "{\"ok\":true,\"msg\":\"Symulacja włączona — ekran radaru pokazuje "
                   "sztuczny front. Wyłącz, żeby wrócić do prawdziwych danych.\"}"
                 : "{\"ok\":true,\"msg\":\"Symulacja wyłączona.\"}");
}

// --- Viessmann ---
// Redirect URI musi byc DOKLADNIE ten sam przy generowaniu linku i przy wymianie
// kodu — inaczej Viessmann odrzuca. Budujemy go z biezacego IP.
String viRedirect() {
  return String("http://") + WiFi.localIP().toString() + "/vicare";
}

// --- licznik gazu: weryfikacja, czy piec mowi prawde ---
//
// SEDNO: piec podaje wlasne zuzycie doby (currentDay). Sumujemy je sami dzien po
// dniu (GasHistory), a uzytkownik wpisuje stany licznika. Miedzy dwoma odczytami
// licznika mamy wiec DWIE niezalezne liczby na ten sam okres: roznice wskazan
// licznika i sume dob z pieca. Jesli sie zgadzaja — piec nie klamie.
//
// "day" to epoch/86400 (UTC). Data z panelu przychodzi jako "YYYY-MM-DD".

namespace {

// Recznie, bez sscanf — i to NIE jest mikrooptymalizacja z nudow.
// To bylo JEDYNE wywolanie sscanf w calym projekcie, a ciagnelo za soba caly silnik
// formatowania wejscia newliba: __ssvfscanf_r (8765 B) + sscanf (104 B) + obsluga.
// Zmierzone na buildzie --clean: 1 781 582 -> 1 771 806 B, czyli -9776 B flasha za 6 linii.
// Format jest sztywny ("YYYY-MM-DD", z panelu), wiec sscanf nic tu nie wnosil.
// Uwaga: nie ruszac na "jeszcze prostsze" atoi() — atoi nie umie powiedziec, GDZIE
// skonczyl, wiec nie da sie sprawdzic separatorow i data "2026xx01" przeszlaby jako 2026.
uint32_t dayFromIso(const char* iso) {
  int y = 0, m = 0, d = 0;
  const char* q = iso;
  char* e = nullptr;
  y = static_cast<int>(strtol(q, &e, 10)); if (e == q || *e != '-') return 0; q = e + 1;
  m = static_cast<int>(strtol(q, &e, 10)); if (e == q || *e != '-') return 0; q = e + 1;
  d = static_cast<int>(strtol(q, &e, 10)); if (e == q) return 0;
  struct tm tmv{};
  tmv.tm_year = y - 1900;
  tmv.tm_mon = m - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = 12;   // poludnie: odporne na strefe i zmiane czasu
  const time_t t = mktime(&tmv);
  return t > 0 ? static_cast<uint32_t>(t) / 86400 : 0;
}

void isoFromDay(uint32_t day, char* out, size_t n) {
  const time_t t = static_cast<time_t>(day) * 86400 + 43200;
  struct tm tmv{};
  localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d", &tmv);
}

}  // namespace

void apiMeterList() {
  // Kopia pod mutexem — gGas zyje w netTask. sumBetween() liczy na kopii.
  GasHistory g;
  xSemaphoreTake(gLock, portMAX_DELAY);
  g = gGas;
  xSemaphoreGive(gLock);

  // Posortowane rosnaco po dacie, zeby "poprzedni odczyt" mial sens.
  int idx[Settings::METERS];
  int n = 0;
  for (int i = 0; i < Settings::METERS; ++i) {
    if (settings().meters[i].day != 0) idx[n++] = i;
  }
  for (int i = 1; i < n; ++i) {
    const int k = idx[i];
    int j = i - 1;
    while (j >= 0 && settings().meters[idx[j]].day > settings().meters[k].day) {
      idx[j + 1] = idx[j];
      --j;
    }
    idx[j + 1] = k;
  }

  JsonDocument j;
  JsonArray arr = j.to<JsonArray>();
  for (int i = 0; i < n; ++i) {
    const Settings::MeterCfg& m = settings().meters[idx[i]];
    JsonObject o = arr.add<JsonObject>();
    char iso[12];
    isoFromDay(m.day, iso, sizeof(iso));
    o["date"] = iso;
    o["m3"] = m.m3;

    if (i == 0) {
      o["cmp"] = -1;   // pierwszy odczyt — nie ma z czym porownac
      continue;
    }
    const Settings::MeterCfg& p = settings().meters[idx[i - 1]];
    const float boiler = g.sumBetween(p.day, m.day);
    if (boiler < 0.f) {
      o["cmp"] = -2;   // log gazu nie siega tak daleko wstecz
      continue;
    }
    o["cmp"] = 0;
    o["meter_used"] = m.m3 - p.m3;
    o["boiler_used"] = boiler;
  }
  String out;
  serializeJson(j, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void apiMeterAdd() {
  JsonDocument b;
  if (deserializeJson(b, server.arg("plain")) != DeserializationError::Ok) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"zły JSON\"}");
    return;
  }
  const uint32_t day = dayFromIso(b["date"] | "");
  const float m3 = b["m3"] | -1.f;
  if (day == 0) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"Podaj datę odczytu\"}");
    return;
  }
  if (m3 < 0.f) {
    server.send(200, "application/json",
                "{\"ok\":false,\"msg\":\"Podaj stan licznika w m³ (można z przecinkiem)\"}");
    return;
  }
  if (!settings().meterAdd(day, m3)) {
    char msg[80];
    snprintf(msg, sizeof(msg),
             "{\"ok\":false,\"msg\":\"Brak miejsca (maks. %d odczytów) — usuń najstarszy\"}",
             Settings::METERS);
    server.send(200, "application/json", msg);
    return;
  }
  settings().meterSave();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Zapisano odczyt.\"}");
}

void apiMeterDel() {
  const uint32_t day = dayFromIso(server.arg("date").c_str());
  if (day != 0 && settings().meterDel(day)) settings().meterSave();
  server.send(200, "application/json", "{\"ok\":true}");
}

void apiBleGw() {
  JsonDocument b;
  if (deserializeJson(b, server.arg("plain")) != DeserializationError::Ok) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"zły JSON\"}");
    return;
  }
  JsonArrayConst hosts = b["hosts"];

  // DWA przebiegi: najpierw walidacja CALEJ listy, dopiero potem zapis. Inaczej
  // literowka w trzecim polu zostawia dwa pierwsze zmienione w RAM i niezapisane
  // w NVS — czyli stan, ktorego uzytkownik nie widzi ani w panelu, ani na ekranie.
  // Host wchodzi prosto do URL-a, wiec walidacja jest tez bariera bezpieczenstwa.
  for (int i = 0; i < Settings::BLE_GW; ++i) {
    const char* h = (i < static_cast<int>(hosts.size())) ? (hosts[i] | "") : "";
    if (!Settings::bleGwHostValid(h)) {
      server.send(200, "application/json",
                  "{\"ok\":false,\"msg\":\"Zły adres — dozwolone litery, cyfry, kropka, "
                  "myślnik i dwukropek (maks. 23 znaki)\"}");
      return;
    }
  }
  for (int i = 0; i < Settings::BLE_GW; ++i) {
    settings().bleGwSet(i, (i < static_cast<int>(hosts.size())) ? (hosts[i] | "") : "");
  }
  // bleGwSave(), NIE save(): tylko ono zageszcza liste. Bez zageszczenia wpis
  // wylacznie w slocie 2 zostawilby slot 0 pusty, a caly kod pyta o bramki przez
  // "czy slot 0 jest obsadzony".
  settings().bleGwSave();
  blegw::retryNow();   // nie czekaj do 2 min backoffu poprzedniej, martwej bramki

  JsonDocument o;
  o["ok"] = true;
  o["msg"] = settings().hasBleGw() ? "Zapisano. Pierwszy odczyt w ciągu 20 s."
                                   : "Bramki wyłączone.";
  String out;
  serializeJson(o, out);
  server.send(200, "application/json", out);
}

void apiViLink() {
  JsonDocument b;
  deserializeJson(b, server.arg("plain"));
  const char* cid = b["cid"] | "";
  if (strlen(cid) < 8) {
    server.send(200, "application/json", "{\"ok\":false,\"msg\":\"Wklej Client ID z portalu\"}");
    return;
  }
  snprintf(settings().viClientId, sizeof(settings().viClientId), "%s", cid);
  settings().viSave();

  const String url = vi::authUrl(cid, viRedirect().c_str());
  JsonDocument o;
  o["ok"] = true;
  o["url"] = url;
  o["msg"] = String("Zapisano. Redirect URI w portalu musi zawierać: ") + viRedirect();
  String out;
  serializeJson(o, out);
  server.send(200, "application/json", out);
}

// Tu wraca przegladarka po autoryzacji — z kodem w query.
void apiViCallback() {
  const String code = server.arg("code");
  const String err = server.arg("error");
  char msg[80] = {};

  if (err.length() > 0) {
    snprintf(msg, sizeof(msg), "Viessmann odmówił: %s", err.c_str());
  } else if (code.length() == 0) {
    snprintf(msg, sizeof(msg), "Brak kodu w odpowiedzi");
  } else if (vi::exchangeCode(code.c_str(), viRedirect().c_str(), msg, sizeof(msg))) {
    snprintf(msg, sizeof(msg), "Połączono z piecem. Możesz zamknąć tę kartę.");
  }

  String h = "<!doctype html><meta charset=utf-8><body style=\"background:#F4F4F0;color:#1A1C1E;"
             "font:16px system-ui;padding:40px;text-align:center\"><h2>";
  h += msg;
  h += "</h2><p><a href=\"/\" style=\"color:#2563C4\">Wróć do panelu</a></p>";
  server.send(200, "text/html; charset=utf-8", h);
}

void apiViState() {
  JsonDocument o;
  o["cid"] = settings().viClientId;       // publiczny — mozna zwracac
  o["auth"] = settings().viRefresh[0] != '\0';   // TOKENA nigdy nie zwracamy
  o["days"] = vi::daysLeft();

  const Diag& d = diag();
  // "ok" = mamy SWIEZE dane, a nie "zero bledow w calej historii". Piec odpytujemy co
  // 3 min i Viessmann potrafi oddac pojedynczy HTTP 400/429 w trakcie sprawnej pracy —
  // stary warunek (viErr puste) zamienial taki PRZEJSCIOWY blad w "piec nie dziala",
  // choc ostatni udany odczyt byl sprzed 3 minut. To wlasnie dawalo panelowi "token 180
  // dni - API HTTP 400" przy w pelni dzialajacym piecu. viOkAt NIE jest kasowany przy
  // porazce (patrz netTask w pogoda-gdynia.ino), wiec jego wiek to wiarygodny znacznik
  // "czy dane sa aktualne". Dopiero brak swiezosci = odswiezanie realnie zawodzi.
  const int32_t dms = static_cast<int32_t>(millis() - d.viOkAt);
  const int okAgo = d.viOkAt == 0 ? -1 : (dms < 0 ? 0 : dms / 1000);
  o["ok"] = okAgo >= 0 && okAgo < 900;   // 15 min = 5 nieudanych cykli z rzedu
  o["ok_ago_s"] = okAgo;
  o["err"] = d.viErr;                    // ostatni blad — do pokazania, gdy dane stare
  o["dhw"] = d.viDhwC;
  o["sup"] = d.viSupplyC;

  String out;
  serializeJson(o, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

// Test zapisu: ustawia nastawe obiegu. Zaden NASZ automat tego nie wola — ale o cudzym
// kodzie w domowej sieci to nic nie mowi, dlatego trasa jest tylko na POST (patrz routes()).
void apiViSet() {
  const int t = server.hasArg("t") ? server.arg("t").toInt() : 0;
  char err[64] = {};
  const bool ok = vi::setCircuitTemp(t, err, sizeof(err));
  JsonDocument o;
  o["ok"] = ok;
  o["set"] = t;
  o["was"] = vi::circuitTarget();
  o["err"] = err;
  String out;
  serializeJson(o, out);
  server.send(200, "application/json", out);
}

void apiViForget() {
  vi::forget();
  server.send(200, "application/json", "{\"ok\":true}");
}

void apiForget() {
  settings().clearWifi();
  server.send(200, "application/json", "{\"ok\":true}");
  delay(400);
  ESP.restart();
}

void routes() {
  server.on("/", sendPage);
  server.on("/api/state", apiState);
  server.on("/api/scan", apiScan);
  server.on("/api/wifi", HTTP_POST, apiWifi);
  server.on("/api/geo", apiGeo);
  server.on("/api/loc", HTTP_POST, apiLoc);
  server.on("/api/inv", HTTP_POST, apiInv);
  server.on("/api/mqtt", HTTP_POST, apiMqtt);
  server.on("/api/update", HTTP_POST, apiUpdate);
  server.on("/api/forget", HTTP_POST, apiForget);
  server.on("/api/log", apiLog);
  server.on("/api/diag", apiDiag);
  server.on("/api/reboot", HTTP_POST, apiReboot);
  // Zrzut awaryjny. GET /api/coredump = metadane i instrukcja (bezpieczne),
  // GET /api/coredump/raw = surowa kopia pamieci (NIE publikowac — patrz apiCoredumpRaw),
  // DELETE /api/coredump = skasowanie, czyli zwolnienie miejsca na nastepny zrzut.
  server.on("/api/coredump", HTTP_GET, apiCoredump);
  server.on("/api/coredump", HTTP_DELETE, apiCoredumpErase);
  server.on("/api/coredump/raw", HTTP_GET, apiCoredumpRaw);
  server.on("/api/screen", apiScreen);
  server.on("/api/view", apiView);
  server.on("/api/tap", HTTP_POST, apiTap);
  server.on("/api/theme", HTTP_POST, apiTheme);
  server.on("/api/tuning", HTTP_POST, apiTuning);
  server.on("/api/bl", apiBacklight);
  server.on("/api/blsweep", apiBacklightSweep);
  server.on("/api/ble", HTTP_GET, apiBleList);
  server.on("/api/ble", HTTP_POST, apiBleSet);
  server.on("/api/blegw", HTTP_POST, apiBleGw);
  server.on("/api/meter", HTTP_GET, apiMeterList);
  server.on("/api/meter", HTTP_POST, apiMeterAdd);
  server.on("/api/meter", HTTP_DELETE, apiMeterDel);
  server.on("/api/radardemo", apiRadarDemo);
  server.on("/api/vi", HTTP_GET, apiViState);
  server.on("/api/vi/link", HTTP_POST, apiViLink);
  server.on("/api/vi/forget", HTTP_POST, apiViForget);
  // POST, nie GET: zapytanie GET-em umie wywolac cudza strona otwarta w tej samej
  // sieci (<img src="http://<ip>/api/vi/set?t=70">) i przestawic ogrzewanie.
  // Przegladarka nie wysle POST-a z obcej strony bez zgody urzadzenia.
  // Recznie: curl -X POST "http://<ip>/api/vi/set?t=45"
  server.on("/api/vi/set", HTTP_POST, apiViSet);
  server.on("/vicare", apiViCallback);   // tu wraca autoryzacja
  server.onNotFound(sendPage);  // captive portal
  server.begin();
  started = true;
}

}  // namespace

bool wifiConfigBusy() {
  const uint32_t until = gWifiCfgUntil;
  if (until == 0) {
    return false;
  }
  // Roznica ze znakiem — przezywa przekrecenie licznika millis().
  return static_cast<int32_t>(millis() - until) < 0;
}

bool scanLock(uint32_t waitMs) {
  if (gScanMx == nullptr) {
    return true;   // mutexa nie ma — zachowujemy sie jak dawniej, bez blokady
  }
  return xSemaphoreTake(gScanMx, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void scanUnlock() {
  if (gScanMx != nullptr) {
    xSemaphoreGive(gScanMx);
  }
}

void setBacklightHandler(void (*testFn)(uint8_t, uint32_t), void (*getFn)(uint8_t&, uint8_t&)) {
  gBlTest = testFn;
  gBlGet = getFn;
}

void setBacklightSweepHandler(void (*fn)(uint32_t)) { gBlSweep = fn; }

void setTapHandler(void (*fn)(int)) { gTap = fn; }

void setViewHandler(void (*setFn)(int), void (*getFn)(int&, int&)) {
  gViewSet = setFn;
  gViewGet = getFn;
}

void setScreenshotHandler(void (*fn)(WiFiClient&)) {
  gScreenshot = fn;
}

void beginAp() {
  apMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kApSsid, kApPass);
  delay(300);
  strncpy(apIpStr, WiFi.softAPIP().toString().c_str(), sizeof(apIpStr) - 1);
  dns.start(53, "*", WiFi.softAPIP());
  routes();
  Serial.printf("Portal AP: %s / %s -> http://%s\n", kApSsid, kApPass, apIpStr);
}

void beginSta() {
  apMode = false;
  if (!started) {
    routes();
  }
  Serial.printf("Panel: http://%s\n", WiFi.localIP().toString().c_str());
}

void loop() {
  if (!started) {
    return;
  }
  if (apMode) {
    dns.processNextRequest();
  }
  server.handleClient();
}

bool apActive() { return apMode; }
const char* apSsid() { return kApSsid; }
const char* apPass() { return kApPass; }
const char* apIp() { return apIpStr; }
bool wifiJustSaved() { return wifiSaved; }
void clearWifiSavedFlag() { wifiSaved = false; }

// ------------------------------------------------- konsola szeregowa ---------
// Awaryjna/serwisowa konfiguracja bez telefonu:
//   wifi <ssid> <haslo>
//   loc <nazwa> <lat> <lon>
//   modbus <ip>
//   peak <W>
//   mqtt <host> [port] | mqtt off
//   mqttauth <user> <haslo>   (haslo "-" kasuje)
//   mqttprefix <prefiks>
//   show / reset
void serialConsole() {
  if (!Serial.available()) {
    return;
  }
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) {
    return;
  }

  if (line.startsWith("wifi ")) {
    const int sp = line.indexOf(' ', 5);
    if (sp < 0) {
      Serial.println("uzycie: wifi <ssid> <haslo>");
      return;
    }
    String s = line.substring(5, sp);
    String p = line.substring(sp + 1);
    strncpy(settings().ssid, s.c_str(), sizeof(settings().ssid) - 1);
    strncpy(settings().pass, p.c_str(), sizeof(settings().pass) - 1);
    settings().save();
    Serial.printf("OK: wifi=%s — restart\n", settings().ssid);
    delay(300);
    ESP.restart();
  } else if (line.startsWith("modbus ")) {
    strncpy(settings().modbusHost, line.substring(7).c_str(),
            sizeof(settings().modbusHost) - 1);
    settings().save();
    Serial.printf("OK: modbus=%s\n", settings().modbusHost);
  } else if (line.startsWith("peak ")) {
    settings().pvPeakW = static_cast<uint16_t>(line.substring(5).toInt());
    settings().save();
    Serial.printf("OK: peak=%u W\n", settings().pvPeakW);
  } else if (line.startsWith("loc ")) {
    const int a = line.indexOf(' ', 4);
    const int b = line.indexOf(' ', a + 1);
    if (a < 0 || b < 0) {
      Serial.println("uzycie: loc <nazwa> <lat> <lon>");
      return;
    }
    strncpy(settings().city, line.substring(4, a).c_str(), sizeof(settings().city) - 1);
    settings().lat = line.substring(a + 1, b).toFloat();
    settings().lon = line.substring(b + 1).toFloat();
    settings().save();
    Serial.printf("OK: %s %.4f %.4f\n", settings().city, settings().lat, settings().lon);
  } else if (line == "mqtt off") {
    settings().mqttEnabled = false;
    settings().save();
    mqttha::configChanged();
    Serial.println("OK: mqtt wylaczony");
  } else if (line.startsWith("mqtt ")) {
    String rest = line.substring(5);
    rest.trim();
    const int sp = rest.indexOf(' ');
    String host = (sp < 0) ? rest : rest.substring(0, sp);
    const int port = (sp < 0) ? 1883 : rest.substring(sp + 1).toInt();
    if (host.length() == 0) {
      Serial.println("uzycie: mqtt <host> [port] | mqtt off");
      return;
    }
    strncpy(settings().mqttHost, host.c_str(), sizeof(settings().mqttHost) - 1);
    settings().mqttPort =
        static_cast<uint16_t>((port < 1 || port > 65535) ? 1883 : port);
    settings().mqttEnabled = true;
    settings().save();
    mqttha::configChanged();
    Serial.printf("OK: mqtt=%s:%u (wlaczony)\n", settings().mqttHost, settings().mqttPort);
  } else if (line.startsWith("mqttauth ")) {
    String rest = line.substring(9);
    rest.trim();
    const int sp = rest.indexOf(' ');
    if (sp < 0) {
      Serial.println("uzycie: mqttauth <user> <haslo>  (haslo \"-\" kasuje)");
      return;
    }
    const String u = rest.substring(0, sp);
    const String p = rest.substring(sp + 1);
    strncpy(settings().mqttUser, u.c_str(), sizeof(settings().mqttUser) - 1);
    if (p == "-") {
      settings().mqttPass[0] = '\0';
    } else {
      strncpy(settings().mqttPass, p.c_str(), sizeof(settings().mqttPass) - 1);
    }
    settings().save();
    mqttha::configChanged();
    Serial.printf("OK: mqtt user=%s haslo=%s\n", settings().mqttUser,
                  settings().mqttPass[0] ? "ustawione" : "brak");
  } else if (line.startsWith("mqttprefix ")) {
    strncpy(settings().mqttPrefix, line.substring(11).c_str(),
            sizeof(settings().mqttPrefix) - 1);
    settings().save();
    mqttha::configChanged();
    Serial.printf("OK: mqtt prefix=%s\n", settings().mqttPrefix);
  } else if (line == "show") {
    Serial.printf("fw=%d ssid=%s city=%s lat=%.4f lon=%.4f modbus=%s peak=%u\n", FW_VERSION,
                  settings().ssid, settings().city, settings().lat, settings().lon,
                  settings().modbusHost, settings().pvPeakW);
    // haslo brokera nie jest wypisywane
    Serial.printf("mqtt=%s host=%s:%u prefix=%s user=%s haslo=%s\n",
                  settings().mqttEnabled ? "on" : "off", settings().mqttHost,
                  settings().mqttPort, settings().mqttPrefix, settings().mqttUser,
                  settings().mqttPass[0] ? "ustawione" : "brak");
  } else if (line == "reset") {
    settings().clearWifi();
    Serial.println("OK: wyczyszczono WiFi — restart");
    delay(300);
    ESP.restart();
  } else {
    Serial.println("komendy: wifi | loc | modbus | peak | mqtt | mqttauth | mqttprefix | "
                   "show | reset");
  }
}

}  // namespace portal
