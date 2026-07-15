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
#include <cstring>

#include "Config.h"
#include "Log.h"
#include "MqttClient.h"
#include "Ota.h"
#include "OtaGuard.h"
#include "BleGateway.h"
#include "BleSensors.h"
#include "RadarMap.h"
#include "Viessmann.h"
#include "Settings.h"
#include "Version.h"

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
<label>Bramka BLE — adres Shelly (opcjonalnie)</label>
<input id=blegw placeholder="np. 192.168.0.102" autocapitalize=off autocorrect=off>
<button class=s onclick=saveGw()>Zapisz bramkę</button>
<div class=hint id=gmsg></div>
<div class=hint>Bluetooth nie ma sieci kratowej — czujnik musi dosięgnąć odbiornika.
Shelly stojący bliżej przekazuje ramki przez WiFi. Klucze zostają tutaj: bramka
widzi wyłącznie szyfrogram.</div>
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
</div>
<pre id=dbg style="white-space:pre-wrap;font:12px ui-monospace,monospace;color:#9fb6cf;
 background:#081221;border:1px solid #24405f;border-radius:8px;padding:10px;margin-top:10px;
 max-height:300px;overflow:auto"></pre>
<button class=s onclick=rb()>Restartuj urządzenie</button>
<button class=s onclick=fgt()>Zapomnij sieć Wi-Fi</button>
</div>
</div><script>
const $=i=>document.getElementById(i);
const NAMES=['Auto','Teraz','Godziny','Radar','5 dni','W domu','Piec','Fotowoltaika','Samoloty','Statystyki'];
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
 const r=await(await fetch('/api/blegw',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({host:$('blegw').value.trim()})})).json();
 $('gmsg').className='hint '+(r.ok?'ok':'err');$('gmsg').textContent=r.msg;
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
async function load(){
 const r=await(await fetch('/api/state')).json();
 $('fw').textContent=r.fw;
 $('st').textContent=r.ap?'tryb konfiguracji':('połączono z '+r.ssid+' · '+r.ip);
 $('cur').textContent=r.city+' ('+r.lat.toFixed(4)+', '+r.lon.toFixed(4)+')';
 $('ssid').value=r.ssid||'';$('mb').value=r.mb||'';$('peak').value=(r.peak/1000).toFixed(1);
 $('mqen').checked=!!r.mq_en;$('mqhost').value=r.mq_host||'';$('mqport').value=r.mq_port||1883;
 $('mqpre').value=r.mq_pre||'';$('mquser').value=r.mq_user||'';$('mqpass').value='';
 $('blegw').value=r.blegw||'';
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
 tabs();nextShot();await load();bles();viStat();setInterval(bles,20000);setInterval(viStat,30000);
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
  d["blegw"] = settings().bleGwHost;

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

void apiDiag() {
  const Diag& d = diag();
  const uint32_t now = millis();
  auto ago = [&](uint32_t at) -> int {
    return at == 0 ? -1 : static_cast<int>((now - at) / 1000);
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

  // Ostatni restart — dotąd nie wiedzieliśmy, czy urządzenie się wywala.
  JsonObject rs = j["reset"].to<JsonObject>();
  rs["reason"] = resetReasonText(d.resetReason);
  rs["reason_code"] = d.resetReason;
  rs["prev_reason"] = resetReasonText(d.prevResetReason);
  rs["was_crash"] = resetWasCrash();
  rs["crashes_total"] = d.panicCount;

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

void apiBleGw() {
  JsonDocument b;
  deserializeJson(b, server.arg("plain"));
  const char* h = b["host"] | "";
  snprintf(settings().bleGwHost, sizeof(settings().bleGwHost), "%s", h);
  settings().save();
  JsonDocument o;
  o["ok"] = true;
  o["msg"] = h[0] ? "Zapisano. Pierwszy odczyt w ciągu 20 s." : "Bramka wyłączona.";
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
  server.on("/api/screen", apiScreen);
  server.on("/api/view", apiView);
  server.on("/api/ble", HTTP_GET, apiBleList);
  server.on("/api/ble", HTTP_POST, apiBleSet);
  server.on("/api/blegw", HTTP_POST, apiBleGw);
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
