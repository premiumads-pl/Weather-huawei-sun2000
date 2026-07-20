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
void (*gViewGet)(int&, int&) = nullptr;
char apIpStr[20] = "192.168.4.1";

const char kApSsid[] = "Pogoda-Setup";
const char kApPass[] = "pogoda123";

// ------------------------------------------------------------------ strona ---

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=pl><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Pogoda + PV</title><style>
*{box-sizing:border-box}
body{margin:0;background:#070d18;color:#e9f0f8;font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.w{max-width:640px;margin:0 auto;padding:20px 16px 60px}
h1{font-size:20px;margin:8px 0 2px}
.sub{color:#7d93ad;font-size:13px;margin-bottom:22px}
.c{background:#111d31;border:1px solid #1e3350;border-radius:12px;padding:16px;margin-bottom:14px}
h2{font-size:13px;letter-spacing:.09em;text-transform:uppercase;color:#00dcf0;margin:0 0 12px}
label{display:block;font-size:12px;color:#8fa6bf;margin:10px 0 4px}
input,select{width:100%;padding:10px 12px;background:#081221;border:1px solid #24405f;
  border-radius:8px;color:#e9f0f8;font-size:15px}
input:focus,select:focus{outline:none;border-color:#00dcf0}
button{width:100%;margin-top:14px;padding:11px;border:0;border-radius:8px;background:#00b9cc;
  color:#04121c;font-weight:600;font-size:15px;cursor:pointer}
button:active{transform:translateY(1px)}
button.s{background:#1e3350;color:#cfe0f0;margin-top:8px}
.row{display:flex;gap:10px}.row>*{flex:1}
.hint{font-size:12px;color:#6f849c;margin-top:8px}
.ok{color:#28e070}.err{color:#ff5555}
ul{list-style:none;margin:10px 0 0;padding:0}
li{padding:10px 12px;border:1px solid #24405f;border-radius:8px;margin-bottom:6px;cursor:pointer;
  display:flex;justify-content:space-between;align-items:center}
li:hover{border-color:#00dcf0;background:#0d1c30}
.sig{color:#7d93ad;font-size:12px}
.b{display:inline-block;padding:2px 7px;border-radius:5px;background:#1e3350;font-size:11px;color:#9fb6cf}
.scr{position:relative;background:#000;border:2px solid #24405f;border-radius:10px;overflow:hidden;
  aspect-ratio:4/3}
.scr img{display:block;width:100%;height:100%;image-rendering:pixelated}
.tabs{display:flex;flex-wrap:wrap;gap:6px;margin-top:12px}
.tabs button{flex:1 1 auto;width:auto;margin:0;padding:8px 6px;font-size:12px;font-weight:600;
  background:#1e3350;color:#9fb6cf}
.tabs button.on{background:#00b9cc;color:#04121c}
.live{float:right;font-size:11px;font-weight:600;color:#28e070;letter-spacing:0}
</style><div class=w>
<h1>Pogoda Gdynia + Fotowoltaika</h1>
<div class=sub>Firmware v<span id=fw>?</span> &middot; <span id=st>...</span></div>

<div class=c>
<h2>Ekran urządzenia <span class=live id=live>● na żywo</span></h2>
<div class=scr><img id=shot alt="wczytuję ekran…"></div>
<div class=tabs id=tabs></div>
<div class=hint id=vmsg>Klikaj, żeby przejść na dany ekran — urządzenie też się przełączy.</div>
</div>

<div class=c>
<h2>Sieć Wi-Fi</h2>
<button class=s onclick=scan()>Wyszukaj sieci bezprzewodowe</button>
<ul id=nets></ul>
<label>Nazwa sieci (SSID)</label><input id=ssid autocapitalize=off autocorrect=off>
<label>Hasło</label><input id=pass type=password autocapitalize=off autocorrect=off>
<button onclick=saveWifi()>Zapisz i połącz</button>
<div class=hint id=wmsg></div>
</div>

<div class=c>
<h2>Lokalizacja pogody</h2>
<div class=row><input id=q placeholder="np. Gdynia"><button class=s style="flex:0 0 110px;margin:0"
 onclick=geo()>Szukaj</button></div>
<ul id=locs></ul>
<div class=hint>Aktualnie: <b id=cur>—</b></div>
</div>

<div class=c>
<h2>Falownik i instalacja</h2>
<label>Adres IP falownika (Modbus TCP)</label><input id=mb placeholder="adres z aplikacji FusionSolar">
<label>Moc szczytowa instalacji [kWp]</label><input id=peak type=number step=0.1 placeholder=6.0>
<button onclick=saveInv()>Zapisz</button>
<div class=hint id=imsg></div>
</div>

<div class=c>
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

<div class=c>
<h2>Czujniki Bluetooth</h2>
<div class=hint>Xiaomi LYWSD03MMC. Fabryczny firmware szyfruje dane — wtedy potrzebny
jest klucz (bindkey) z chmury Xiaomi. Klucz zostaje w pamięci urządzenia i nigdy
nie opuszcza sieci domowej.</div>
<ul id=bles></ul>
<div class=hint id=bmsg></div>
<label>Bramki BLE — adresy (opcjonalnie)</label>
<div id=gws></div>
<button class=s onclick=saveGw()>Zapisz bramki</button>
<div class=hint id=gmsg></div>
<div class=hint>Bluetooth nie ma sieci kratowej — czujnik musi dosięgnąć odbiornika.
Shelly stojący bliżej przekazuje ramki przez WiFi. Klucze zostają tutaj: bramka
widzi wyłącznie szyfrogram.</div>
</div>

<div class=c>
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

<div class=c>
<h2>Piec Viessmann</h2>
<div class=hint>Twój Vitodens nie wystawia niczego w sieci lokalnej (sprawdzone: zero
otwartych portów) — jedyna droga to chmura ViCare. Client ID weź z
<a href="https://app.developer.viessmann-climatesolutions.com" target=_blank
 style="color:#00dcf0">portalu deweloperskiego</a>. Jest publiczny; token dostępu
zostaje wyłącznie w pamięci urządzenia.</div>
<label>Client ID</label><input id=vicid autocapitalize=off autocorrect=off
 placeholder="np. 962d...b35ce">
<button class=s onclick=viLink()>1. Zapisz i wygeneruj link autoryzacyjny</button>
<div class=hint id=vimsg></div>
<div id=viauth style="display:none">
 <a id=vihref target=_blank style="color:#00dcf0;font-weight:600">
  2. Otwórz i zaloguj się do Viessmann →</a>
 <div class=hint>Po zalogowaniu przeglądarka wróci tutaj sama i zapisze dostęp.
 Kod autoryzacyjny żyje 20 sekund, więc nie zwlekaj.</div>
</div>
<div class=hint id=vistat></div>
<button class=s onclick=viForget()>Odłącz piec</button>
</div>

<div class=c>
<h2>Radar opadów</h2>
<div class=hint>Ekran radaru pojawia się w rotacji tylko wtedy, gdy realnie pada.
Symulacja pokazuje sztuczny front — do obejrzenia, jak wygląda wizualizacja.</div>
<div class=row>
<button class=s style=margin:0 onclick="demo(1)">Włącz symulację</button>
<button class=s style=margin:0 onclick="demo(0)">Wyłącz</button>
</div>
<div class=hint id=dmsg></div>
</div>

<div class=c>
<h2>Aktualizacje</h2>
<div class=hint>Urządzenie samo sprawdza GitHub co 15 minut.</div>
<button class=s onclick=upd()>Sprawdź teraz</button>
<div class=hint id=umsg></div>
</div>

<div class=c>
<h2>Diagnostyka</h2>
<div class=row>
<button class=s style=margin:0 onclick=dg()>Stan urządzenia</button>
<button class=s style=margin:0 onclick=lg()>Logi</button>
<button class=s style=margin:0 onclick=cd()>Zrzut awaryjny</button>
</div>
<pre id=dbg style="white-space:pre-wrap;font:12px ui-monospace,monospace;color:#9fb6cf;
 background:#081221;border:1px solid #24405f;border-radius:8px;padding:10px;margin-top:10px;
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
<button class=s onclick=rb()>Restartuj urządzenie</button>
<button class=s onclick=fgt()>Zapomnij sieć Wi-Fi</button>
</div>
</div><script>
const $=i=>document.getElementById(i);
const NAMES=['Auto','Retro','Teraz','Godziny','Radar','5 dni','W domu','Piec','Fotowoltaika','Samoloty','Powietrze','Pamięć','Ruch','Statystyki'];
let live=true,pin=-1;

function tabs(){
 $('tabs').innerHTML=NAMES.map((n,i)=>
  `<button class="${i-1===pin?'on':''}" onclick="pickView(${i-1})">${n}</button>`).join('');
}
async function pickView(i){
 pin=i;tabs();
 $('vmsg').textContent=i<0?'Rotacja automatyczna — dokładnie jak na urządzeniu.'
  :('Zatrzymane na ekranie: '+NAMES[i+1]+'. Kliknij „Auto”, żeby wznowić rotację.');
 try{const r=await(await fetch('/api/view?i='+i)).json();pin=r.pin;tabs();}catch(e){}
}
// kolejna klatka dopiero, gdy poprzednia dojdzie — nie zalewamy urządzenia
function nextShot(){
 if(!live){setTimeout(nextShot,700);return;}
 const im=new Image();
 im.onload=()=>{$('shot').src=im.src;setTimeout(nextShot,700);};
 im.onerror=()=>setTimeout(nextShot,2000);
 im.src='/api/screen?'+Date.now();
}
document.addEventListener('visibilitychange',()=>{
 live=!document.hidden;$('live').textContent=live?'● na żywo':'‖ wstrzymane';});
// --- czujniki BLE ---
async function bles(){
 let r;
 try{r=await(await fetch('/api/ble')).json();}catch(e){return}
 if(!r.length){$('bles').innerHTML='<li>Nie wykryto czujników (nasłuch co 45 s)</li>';return}
 $('bles').innerHTML=r.map((s,i)=>{
  const st = s.valid ? `${s.t.toFixed(1)}°C · ${s.h.toFixed(0)}% · bat ${s.bat}%`
       : (s.needsKey ? '<span class=err>brak klucza</span>' : 'czekam na dane');
  const src = s.gw ? ' <span style="color:#00dcf0">· przez bramkę</span>' : '';
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
  if(!r.auth){$('vistat').textContent='Nie autoryzowano.';return;}
  $('vistat').className='hint ok';
  $('vistat').textContent=r.ok
    ? `Połączono. CWU ${r.dhw.toFixed(1)}°C, zasilanie ${r.sup.toFixed(1)}°C. Autoryzacja ważna jeszcze ${r.days} dni.`
    : `Autoryzowano (ważne ${r.days} dni), ale: ${r.err}`;
 }catch(e){}
}
async function demo(on){
 $('dmsg').textContent='...';
 const r=await(await fetch('/api/radardemo?on='+on)).json();
 $('dmsg').className='hint ok';$('dmsg').textContent=r.msg;
 if(on) pickView(2);
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
 $('st').textContent=r.ap?'tryb konfiguracji':('połączono z '+r.ssid+' · '+r.ip);
 $('cur').textContent=r.city+' ('+r.lat.toFixed(4)+', '+r.lon.toFixed(4)+')';
 $('ssid').value=r.ssid||'';$('mb').value=r.mb||'';$('peak').value=(r.peak/1000).toFixed(1);
 $('mqen').checked=!!r.mq_en;$('mqhost').value=r.mq_host||'';$('mqport').value=r.mq_port||1883;
 $('mqpre').value=r.mq_pre||'';$('mquser').value=r.mq_user||'';$('mqpass').value='';
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
(async()=>{
 try{const r=await(await fetch('/api/view')).json();pin=r.pin;}catch(e){}
 tabs();nextShot();await load();bles();viStat();meters();setInterval(bles,20000);setInterval(viStat,30000);
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
// 64 kB. Ten sam wzorzec co /api/screen, ktore wysyla BMP 320x240 wiersz po wierszu.
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

  String h = "<!doctype html><meta charset=utf-8><body style=\"background:#070d18;color:#e9f0f8;"
             "font:16px system-ui;padding:40px;text-align:center\"><h2>";
  h += msg;
  h += "</h2><p><a href=\"/\" style=\"color:#00dcf0\">Wróć do panelu</a></p>";
  server.send(200, "text/html; charset=utf-8", h);
}

void apiViState() {
  JsonDocument o;
  o["cid"] = settings().viClientId;       // publiczny — mozna zwracac
  o["auth"] = settings().viRefresh[0] != '\0';   // TOKENA nigdy nie zwracamy
  o["days"] = vi::daysLeft();

  const Diag& d = diag();
  o["ok"] = d.viOkAt != 0 && d.viErr[0] == '\0';
  o["err"] = d.viErr;
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
