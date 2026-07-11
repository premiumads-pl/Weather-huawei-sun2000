#include "Portal.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstring>

#include "Config.h"
#include "Log.h"
#include "Ota.h"
#include "Settings.h"
#include "Version.h"

namespace portal {
namespace {

WebServer server(80);
DNSServer dns;
bool apMode = false;
bool started = false;
bool wifiSaved = false;
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
</style><div class=w>
<h1>Pogoda Gdynia + Fotowoltaika</h1>
<div class=sub>Firmware v<span id=fw>?</span> &middot; <span id=st>...</span></div>

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
async function dg(){$('dbg').textContent=await(await fetch('/api/diag')).text();}
async function lg(){$('dbg').textContent=await(await fetch('/api/log')).text();}
async function rb(){if(confirm('Restartować?')){await fetch('/api/reboot',{method:'POST'});}}
const $=i=>document.getElementById(i);
async function load(){
 const r=await(await fetch('/api/state')).json();
 $('fw').textContent=r.fw;
 $('st').textContent=r.ap?'tryb konfiguracji':('połączono z '+r.ssid+' · '+r.ip);
 $('cur').textContent=r.city+' ('+r.lat.toFixed(4)+', '+r.lon.toFixed(4)+')';
 $('ssid').value=r.ssid||'';$('mb').value=r.mb||'';$('peak').value=(r.peak/1000).toFixed(1);
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
async function upd(){
 $('umsg').textContent='Sprawdzam…';
 const r=await(await fetch('/api/update',{method:'POST'})).json();
 $('umsg').textContent=r.msg;
}
async function fgt(){
 if(!confirm('Usunąć zapisaną sieć Wi-Fi?'))return;
 await fetch('/api/forget',{method:'POST'});location.reload();
}
load();
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
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void apiScan() {
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

  // sprobuj polaczyc zanim zapiszemy
  WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(s, p);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 14000) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
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
  j["psram"] = ESP.getPsramSize();
  j["cpu_temp"] = temperatureRead();

  JsonObject w = j["wifi"].to<JsonObject>();
  w["ssid"] = WiFi.SSID();
  w["ip"] = WiFi.localIP().toString();
  w["rssi"] = WiFi.RSSI();
  w["connects"] = d.wifiConnects;

  JsonObject we = j["weather"].to<JsonObject>();
  we["ok_ago_s"] = ago(d.weatherOkAt);
  we["err"] = d.weatherErr;

  JsonObject pv = j["pv"].to<JsonObject>();
  pv["ok_ago_s"] = ago(d.pvOkAt);
  pv["err"] = d.pvErr;

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

  JsonObject o = j["ota"].to<JsonObject>();
  o["remote"] = d.otaRemote;
  o["msg"] = d.otaMsg;

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
  server.on("/api/update", HTTP_POST, apiUpdate);
  server.on("/api/forget", HTTP_POST, apiForget);
  server.on("/api/log", apiLog);
  server.on("/api/diag", apiDiag);
  server.on("/api/reboot", HTTP_POST, apiReboot);
  server.onNotFound(sendPage);  // captive portal
  server.begin();
  started = true;
}

}  // namespace

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
  } else if (line == "show") {
    Serial.printf("fw=%d ssid=%s city=%s lat=%.4f lon=%.4f modbus=%s peak=%u\n", FW_VERSION,
                  settings().ssid, settings().city, settings().lat, settings().lon,
                  settings().modbusHost, settings().pvPeakW);
  } else if (line == "reset") {
    settings().clearWifi();
    Serial.println("OK: wyczyszczono WiFi — restart");
    delay(300);
    ESP.restart();
  } else {
    Serial.println("komendy: wifi | loc | modbus | peak | show | reset");
  }
}

}  // namespace portal
