/*
  SmartDesk – Lern-Ampel mit lokalem Dashboard (ESP32)
  -----------------------------------------------------
  Hardware: LED rot=33, gelb=15, grün=27, Potentiometer=32 (Erweiterungsplatine)
  Aufruf:   http://smartdesk.local  oder die IP aus dem Serial Monitor

  Lernlogik (angelehnt an Pomodoro / ultradiane Rhythmen):
    GRÜN  = 0–75 % der Lernzeit  -> volle Konzentration
    GELB  = 75–100 %             -> Aufmerksamkeit sinkt, Aufgabe abschließen
    ROT   = Pause Pflicht        -> 20 % der Lernzeit (min. 5, max. 15 Min.),
                                    jede 4. Einheit lange Pause (25 Min.)
    GRÜN BLINKT = bereit
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>

// ====== HIER ANPASSEN ======
const char *WIFI_SSID = "A1-135B8CF1";
const char *WIFI_PASS = "9DwN0YSsXHTCY2";
// ===========================

// Pins passend zur Erweiterungsplatine (LED-Modul auf BUS 2, Poti-Modul auf BUS 1)
const int LED_RED = 33;    // BUS 2 · AN
const int LED_YELLOW = 15; // BUS 2 · CS
const int LED_GREEN = 27;  // BUS 2 · PWM
const int POT_PIN = 32;    // BUS 1 · AN (ADC1, WLAN-tauglich)

enum State
{
  IDLE,
  FOCUS,
  BREAK
};
State state = IDLE;

WebServer server(80);
Preferences prefs;

struct Stats
{
  uint32_t day;
  uint32_t sessions;
  uint32_t aborted;
  uint32_t totalMin;
  uint16_t m[7];
} st;

uint8_t setMin = 25;
bool fast = false;  // Testmodus: 1 Min = 1 Sek
uint8_t srcPot = 1; // 1 = Potentiometer, 0 = manuell (Web)
uint32_t phaseStart = 0, phaseLen = 0;
uint32_t alarmId = 0;
const char *alarmType = "none";
float potAvg = 0, potRef = 0;
uint32_t lastPot = 0;
int potRaw = 0;
uint32_t ledTestStart = 0; // LED-Selbsttest (Dashboard-Button)

// ---------- Hilfsfunktionen ----------
uint32_t elapsedSec() { return ((millis() - phaseStart) / 1000) * (fast ? 60 : 1); }

int potMinutes() { return constrain(10 + (int)((potAvg * 50.0f) / 4095.0f + 0.5f), 10, 60); }

uint8_t breakFor(uint8_t focusMin, uint32_t nextSession)
{
  if (nextSession % 4 == 0)
    return 25;
  return constrain((int)(focusMin * 0.2f + 0.5f), 5, 15);
}

long dayNum(const tm &t)
{
  int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
  if (m < 3)
  {
    y--;
    m += 12;
  }
  return 365L * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d;
}

void saveStats() { prefs.putBytes("st", &st, sizeof(st)); }

void rollDay()
{
  struct tm t;
  if (!getLocalTime(&t, 0))
    return;
  long d = dayNum(t);
  if (st.day == 0)
  {
    st.day = d;
    saveStats();
    return;
  }
  long diff = d - (long)st.day;
  if (diff <= 0)
    return;
  for (long k = 0; k < diff && k < 7; k++)
  {
    for (int i = 0; i < 6; i++)
      st.m[i] = st.m[i + 1];
    st.m[6] = 0;
  }
  st.day = d;
  saveStats();
}

void raise(const char *type)
{
  alarmId++;
  alarmType = type;
  Serial.printf("ALARM #%lu: %s\n", (unsigned long)alarmId, type);
}

void addMinutes(uint32_t n)
{
  rollDay();
  st.m[6] += n;
  st.totalMin += n;
  saveStats();
}

void startFocus()
{
  if (state != IDLE)
    return;
  state = FOCUS;
  phaseStart = millis();
  phaseLen = (uint32_t)setMin * 60;
  Serial.printf("Fokus gestartet: %u Min\n", setMin);
}

void stopAll()
{
  if (state == FOCUS)
  {
    if (!fast)
    {
      addMinutes(elapsedSec() / 60);
      st.aborted++;
      saveStats();
    }
  }
  state = IDLE;
}

void completeFocus()
{
  if (!fast)
  {
    st.sessions++;
    addMinutes(setMin);
  }
  uint8_t b = breakFor(setMin, st.sessions + 1);
  if (fast)
    b = breakFor(setMin, 1);
  state = BREAK;
  phaseStart = millis();
  phaseLen = (uint32_t)b * 60;
  raise("break");
}

void tick()
{
  if (state == IDLE)
    return;
  if (elapsedSec() < phaseLen)
    return;
  if (state == FOCUS)
    completeFocus();
  else
  {
    state = IDLE;
    raise("ready");
  }
}

// ---------- LEDs ----------
void setLeds(bool r, bool y, bool g)
{
  digitalWrite(LED_RED, r);
  digitalWrite(LED_YELLOW, y);
  digitalWrite(LED_GREEN, g);
}

void updateLeds()
{
  uint32_t n = millis();
  bool r = 0, y = 0, g = 0;
  if (ledTestStart)
  { // Test: Rot -> Gelb -> Grün, je 1,5 s
    uint32_t d = (n - ledTestStart) / 1500;
    if (d < 3)
    {
      setLeds(d == 0, d == 1, d == 2);
      return;
    }
    ledTestStart = 0;
  }
  if (state == IDLE)
    g = (n / 700) % 2;
  else if (state == FOCUS)
  {
    if (elapsedSec() < phaseLen * 3 / 4)
      g = 1;
    else
      y = 1;
  }
  else
  {
    uint32_t e = elapsedSec(), rem = e < phaseLen ? phaseLen - e : 0;
    r = (rem <= 15 && !fast) ? ((n / 150) % 2) : 1; // letzte 15 s: schnelles Blinken
  }
  setLeds(r, y, g);
}

void bootAnimation()
{
  for (int i = 0; i < 3; i++)
  {
    setLeds(1, 0, 0);
    delay(300);
    setLeds(0, 1, 0);
    delay(300);
    setLeds(0, 0, 1);
    delay(300);
  }
  setLeds(0, 0, 0);
}

// ---------- Potentiometer ----------
void readPot()
{
  if (millis() - lastPot < 20)
    return;
  lastPot = millis();
  long s = 0;
  for (int i = 0; i < 8; i++)
    s += analogRead(POT_PIN);
  potRaw = s / 8;
  potAvg = potAvg * 0.8f + (s / 8.0f) * 0.2f;
  if (state == IDLE && fabsf(potAvg - potRef) > 60)
  { // Poti wurde bewegt -> übernimmt
    potRef = potAvg;
    setMin = potMinutes();
    srcPot = 1;
  }
}

// ---------- Web ----------
extern const char INDEX_HTML[] PROGMEM;

void handleState()
{
  rollDay();
  uint32_t el = elapsedSec();
  uint32_t len = state == IDLE ? (uint32_t)setMin * 60 : phaseLen;
  uint32_t rem = state == IDLE ? len : (el < len ? len - el : 0);
  const char *s = state == IDLE ? "idle" : state == FOCUS ? "focus"
                                                          : "break";
  const char *l = state == IDLE ? "idle" : state == BREAK ? "red"
                                                          : (el < phaseLen * 3 / 4 ? "green" : "yellow");
  int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  float chipTemp = temperatureRead();
  if (isnan(chipTemp))
    chipTemp = -99;
  char b[900];
  snprintf(b, sizeof b,
           "{\"s\":\"%s\",\"l\":\"%s\",\"set\":%u,\"rem\":%lu,\"len\":%lu,\"src\":%u,\"pot\":%d,\"fast\":%d,"
           "\"rssi\":%d,\"up\":%lu,\"heap\":%u,\"alarm\":%lu,\"at\":\"%s\",\"sess\":%lu,\"abort\":%lu,\"tot\":%lu,"
           "\"temp\":%.1f,\"raw\":%d,\"ip\":\"%s\",\"w\":[%u,%u,%u,%u,%u,%u,%u]}",
           s, l, setMin, (unsigned long)rem, (unsigned long)len, srcPot, (int)potAvg, fast ? 1 : 0,
           rssi, (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), (unsigned long)alarmId, alarmType,
           (unsigned long)st.sessions, (unsigned long)st.aborted, (unsigned long)st.totalMin,
           chipTemp, potRaw, ip.toString().c_str(), st.m[0], st.m[1], st.m[2], st.m[3], st.m[4], st.m[5], st.m[6]);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", b);
}

void handleSpeed()
{
  static char buf[12288];
  static bool init = false;
  if (!init)
  {
    memset(buf, 'a', sizeof buf);
    init = true;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(sizeof buf);
  server.send(200, "application/octet-stream", "");
  server.sendContent(buf, sizeof buf);
}

void setupRoutes()
{
  server.on("/", []()
            { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/api/state", handleState);
  server.on("/api/speed", handleSpeed);
  server.on("/api/set", []()
            {
    if (state == IDLE && server.hasArg("min")) { setMin = constrain(server.arg("min").toInt(), 10, 60); srcPot = 0; potRef = potAvg; }
    server.send(200, "text/plain", "ok"); });
  server.on("/api/start", []()
            { startFocus(); server.send(200, "text/plain", "ok"); });
  server.on("/api/stop", []()
            { stopAll();    server.send(200, "text/plain", "ok"); });
  server.on("/api/fast", []()
            { if (state == IDLE) fast = server.arg("on") == "1"; server.send(200, "text/plain", "ok"); });
  server.on("/api/ledtest", []()
            { ledTestStart = millis(); server.send(200, "text/plain", "ok"); });
  server.onNotFound([]()
                    { server.send(404, "text/plain", "404"); });
}

// ---------- Setup / Loop ----------
void setup()
{
  setCpuFrequencyMhz(160); // weniger Abwärme (Standard 240 MHz), WLAN läuft stabil
  Serial.begin(115200);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(POT_PIN, ADC_11db); // voller Bereich 0–3,3 V
  bootAnimation();

  prefs.begin("smartdesk", false);
  if (prefs.getBytesLength("st") == sizeof(st))
    prefs.getBytes("st", &st, sizeof(st));
  else
    memset(&st, 0, sizeof(st));

  for (int i = 0; i < 30; i++)
  {
    long s = 0;
    for (int k = 0; k < 8; k++)
      s += analogRead(POT_PIN);
    potAvg = s / 8.0f;
    delay(5);
  }
  potRef = potAvg;
  setMin = potMinutes();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WLAN verbinden");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000)
  {
    digitalWrite(LED_YELLOW, !digitalRead(LED_YELLOW));
    delay(300);
    Serial.print(".");
  }
  setLeds(0, 0, 0);
  if (WiFi.status() == WL_CONNECTED)
  {
    WiFi.setSleep(true);               // Modem-Sleep spart Energie/Wärme
    WiFi.setTxPower(WIFI_POWER_11dBm); // reicht im Nahbereich völlig, weniger Hitze
    Serial.printf("\nOnline: http://%s\n", WiFi.localIP().toString().c_str());
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
  }
  else
  {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SmartDesk-Setup");
    Serial.printf("\nKein WLAN -> Access Point 'SmartDesk-Setup', http://%s\n", WiFi.softAPIP().toString().c_str());
  }
  if (MDNS.begin("smartdesk"))
    MDNS.addService("http", "tcp", 80);

  setupRoutes();
  server.begin();
  Serial.println("=== SmartDesk bereit ===");
}

void loop()
{
  server.handleClient();
  readPot();
  tick();
  updateLeds();
  delay(2); // CPU darf kurz schlafen -> deutlich kühler
}

// ======================= DASHBOARD =======================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartDesk</title>
<style>
:root{--bg:#070b14;--card:rgba(17,24,39,.72);--bd:rgba(148,163,184,.15);--tx:#e6edf7;--mu:#7f8ca3;--g:#22e58b;--y:#ffc53d;--r:#ff4d5e;--c:#38bdf8;--acc:#38bdf8}
*{box-sizing:border-box;margin:0}
body{background:radial-gradient(1100px 560px at 8% -10%,#12294a66,transparent),radial-gradient(900px 500px at 100% 0,#2a124a55,transparent),var(--bg);color:var(--tx);font:14px/1.45 system-ui,"Segoe UI",Roboto,sans-serif;min-height:100vh;padding:18px}
body::before{content:"";position:fixed;inset:0;background:linear-gradient(rgba(255,255,255,.03) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.03) 1px,transparent 1px);background-size:38px 38px;pointer-events:none;-webkit-mask-image:radial-gradient(circle at 50% 25%,#000,transparent 75%);mask-image:radial-gradient(circle at 50% 25%,#000,transparent 75%)}
.mono{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}
header,main{max-width:1200px;margin:0 auto;position:relative}
header{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:16px}
.logo{display:flex;align-items:center;gap:10px;font-weight:700;letter-spacing:.18em;font-size:14px}
.logo i{width:32px;height:32px;border-radius:10px;background:linear-gradient(135deg,var(--c),#8b5cf6);box-shadow:0 0 24px #38bdf866;display:grid;place-items:center;font:700 12px ui-monospace,monospace;letter-spacing:0}
.pill{display:flex;align-items:center;gap:9px;padding:8px 15px;border-radius:99px;border:1px solid var(--bd);background:var(--card);backdrop-filter:blur(10px);font-size:12px;letter-spacing:.1em}
.dot{width:9px;height:9px;border-radius:50%;background:var(--r);color:var(--r)}
.on .dot{background:var(--g);color:var(--g);animation:pulse 1.8s infinite}
@keyframes pulse{0%{box-shadow:0 0 0 0 currentColor}80%,100%{box-shadow:0 0 0 10px transparent}}
main{display:grid;grid-template-columns:minmax(320px,430px) 1fr;gap:16px;transition:.5s}
@media(max-width:900px){main{grid-template-columns:1fr}}
body.off main{filter:grayscale(1) brightness(.6)}
.card{background:var(--card);border:1px solid var(--bd);border-radius:18px;padding:18px;backdrop-filter:blur(12px);animation:rise .7s both}
.card:nth-child(2){animation-delay:.08s}.card:nth-child(3){animation-delay:.16s}
@keyframes rise{from{opacity:0;transform:translateY(16px)}}
h3{font-size:11px;letter-spacing:.18em;color:var(--mu);text-transform:uppercase;margin-bottom:14px;font-weight:600}
.right{display:grid;gap:16px;align-content:start}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px}
.kpi{padding:12px 14px;border-radius:14px;background:#0b1220aa;border:1px solid var(--bd)}
.kpi small{display:block;color:var(--mu);font-size:10px;letter-spacing:.14em;text-transform:uppercase}
.kpi b{font:600 24px ui-monospace,Consolas,monospace}
.kpi em{font-style:normal;color:var(--mu);font-size:12px;margin-left:3px}
.ringwrap{position:relative;width:min(100%,300px);margin:4px auto 10px}
.ringwrap svg{width:100%;display:block;filter:drop-shadow(0 0 16px color-mix(in srgb,var(--acc) 45%,transparent))}
.trk{fill:none;stroke:#1b2639;stroke-width:12}
.arc{fill:none;stroke:var(--acc);stroke-width:12;stroke-linecap:round;stroke-dasharray:703.7;stroke-dashoffset:0;transform:rotate(-90deg);transform-origin:130px 130px;transition:stroke-dashoffset .3s linear,stroke .6s}
.tick{fill:none;stroke:#0b1220;stroke-width:14;stroke-dasharray:1.5 15.5}
.center{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center;padding:0 38px}
#phase{font-size:11px;letter-spacing:.3em;color:var(--acc);transition:.5s}
#time{font-size:clamp(46px,14vw,64px);font-weight:600;letter-spacing:-.02em;text-shadow:0 0 22px color-mix(in srgb,var(--acc) 55%,transparent)}
#sub{font-size:12px;color:var(--mu);min-height:34px}
.lamps{display:flex;justify-content:center;gap:14px;margin:6px 0 16px}
.lamps b{width:22px;height:22px;border-radius:50%;opacity:.14;transition:.5s;background:currentColor}
.lamps b.on{opacity:1;box-shadow:0 0 18px currentColor,0 0 40px currentColor}
.ctl{display:grid;gap:12px}
.row{display:flex;justify-content:space-between;align-items:center;gap:10px}
.badge{font-size:10px;letter-spacing:.14em;padding:3px 9px;border-radius:99px;border:1px solid var(--bd);color:var(--c);background:#38bdf812}
input[type=range]{width:100%;accent-color:var(--c);height:6px}
input[type=range]:disabled{opacity:.35}
.btns{display:grid;grid-template-columns:2fr 1fr;gap:10px}
button{font:600 13px system-ui;letter-spacing:.1em;padding:13px;border-radius:12px;border:1px solid var(--bd);background:#111b2e;color:var(--tx);cursor:pointer;transition:.25s}
button:hover{transform:translateY(-1px);border-color:var(--c)}
button.pri{background:linear-gradient(135deg,#0ea5e9,#6366f1);border:0;box-shadow:0 8px 26px #4f46e555}
button:disabled{opacity:.35;pointer-events:none}
label.sw{display:flex;gap:8px;align-items:center;color:var(--mu);font-size:12px;cursor:pointer}
.chart{display:flex;align-items:flex-end;gap:10px;height:150px;padding-top:10px}
.bar{flex:1;display:flex;flex-direction:column;justify-content:flex-end;align-items:center;height:100%;gap:6px;font-size:11px;color:var(--mu)}
.bar i{width:100%;border-radius:8px 8px 3px 3px;background:linear-gradient(180deg,var(--c),#6366f1);min-height:3px;height:0;transition:height 1.1s cubic-bezier(.2,.9,.2,1);box-shadow:0 0 14px #38bdf833}
.bar:last-child i{background:linear-gradient(180deg,var(--g),#0ea5a0)}
.bar b{color:var(--tx);font:600 11px ui-monospace,monospace}
.spark{width:100%;height:54px;margin-top:10px}
.sig{display:inline-flex;gap:3px;align-items:flex-end;height:20px}
.sig i{width:5px;background:#1f2b40;border-radius:2px}.sig i.on{background:var(--g)}
.sig i:nth-child(1){height:6px}.sig i:nth-child(2){height:11px}.sig i:nth-child(3){height:16px}.sig i:nth-child(4){height:20px}
.sci{display:grid;gap:8px;color:#b5c0d3;font-size:13px}
.sci div{display:flex;gap:10px;align-items:flex-start}.sci b{width:10px;height:10px;border-radius:50%;margin-top:5px;flex:none;background:currentColor;box-shadow:0 0 10px currentColor}
.sci small{color:var(--mu);display:block;margin-top:4px}
ul{list-style:none;padding:0;max-height:150px;overflow:auto;font-size:12px;color:#b5c0d3}
li{padding:6px 0;border-bottom:1px solid var(--bd);animation:rise .4s both}li span{color:var(--mu);margin-right:8px;font-family:ui-monospace,monospace}
#toasts{position:fixed;right:16px;bottom:16px;display:grid;gap:8px;z-index:30}
.toast{padding:12px 16px;border-radius:12px;background:#0f1a2ee6;border:1px solid var(--bd);border-left:3px solid var(--y);backdrop-filter:blur(10px);animation:rise .4s both;max-width:300px}
.ov{position:fixed;inset:0;z-index:40;display:grid;place-items:center;background:rgba(4,7,14,.86);backdrop-filter:blur(8px);transition:opacity .7s,visibility .7s;text-align:center;padding:20px}
.ov.hide{opacity:0;visibility:hidden}
#welcome h1{font-size:clamp(26px,6vw,44px);font-weight:300;letter-spacing:.04em;animation:rise .9s both}
#welcome h1 b{font-weight:700;background:linear-gradient(90deg,var(--c),#a78bfa);-webkit-background-clip:text;background-clip:text;color:transparent}
#welcome p{color:var(--mu);margin-top:10px;font-family:ui-monospace,monospace;font-size:13px;animation:rise .9s .5s both}
.ld{width:180px;height:3px;background:#1b2639;border-radius:3px;margin:22px auto 0;overflow:hidden}.ld i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--c),#a78bfa);animation:ld 2.2s .3s forwards}
@keyframes ld{to{width:100%}}
#alarm .box{max-width:420px;padding:34px 30px;border-radius:24px;background:#0d1424;border:1px solid var(--ac,var(--r));box-shadow:0 0 70px color-mix(in srgb,var(--ac,var(--r)) 40%,transparent);animation:rise .5s both}
#alarm .ic{width:84px;height:84px;border-radius:50%;margin:0 auto 18px;background:var(--ac,var(--r));animation:glow 1.1s infinite;box-shadow:0 0 30px var(--ac,var(--r))}
@keyframes glow{50%{transform:scale(1.12);box-shadow:0 0 70px var(--ac,var(--r))}}
#alarm h2{letter-spacing:.2em;margin-bottom:8px}#alarm p{color:#b5c0d3;margin-bottom:22px}
#banner{display:none;max-width:1200px;margin:0 auto 12px;padding:10px 16px;border-radius:12px;background:#ff4d5e22;border:1px solid var(--r);color:#ffb3bb;position:relative}
body.off #banner{display:block}
</style></head><body>
<div class="ov" id="welcome"><div><h1 id="wl">Willkommen</h1><p id="wp">› initialisiere SmartDesk …</p><div class="ld"><i></i></div></div></div>
<div class="ov hide" id="alarm"><div class="box"><div class="ic"></div><h2 id="at">PAUSE!</h2><p id="am"></p><button class="pri" onclick="closeAlarm()" style="width:100%">VERSTANDEN</button></div></div>
<div id="toasts"></div>

<header>
 <div class="logo"><i>SD</i>SMARTDESK</div>
 <div style="display:flex;gap:10px;flex-wrap:wrap">
  <div class="pill mono" id="clock">--:--:--</div>
  <div class="pill" id="net"><span class="dot"></span><span id="nettxt">VERBINDE …</span><span class="mono" id="netms" style="color:var(--mu)"></span></div>
 </div>
</header>
<div id="banner">⚠ ESP32 nicht erreichbar – Timer läuft auf dem Gerät weiter, LEDs bleiben maßgeblich.</div>

<main>
<section class="card">
 <h3>Fokus-Timer</h3>
 <div class="ringwrap">
  <svg viewBox="0 0 260 260"><circle class="tick" cx="130" cy="130" r="112"/><circle class="trk" cx="130" cy="130" r="112"/><circle class="arc" id="arc" cx="130" cy="130" r="112"/></svg>
  <div class="center"><div id="phase" class="mono">BEREIT</div><div id="time" class="mono">25:00</div><div id="sub"></div></div>
 </div>
 <div class="lamps"><b id="lr" style="color:var(--r)"></b><b id="ly" style="color:var(--y)"></b><b id="lg" style="color:var(--g)"></b></div>
 <div class="ctl">
  <div class="row"><span>Lernzeit <b class="mono" id="sv">25 min</b></span><span class="badge mono" id="src">POTI</span></div>
  <input type="range" id="sl" min="10" max="60" step="1" value="25">
  <div class="row" style="color:var(--mu);font-size:12px"><span id="brk">Nächste Pause: 5 min</span><span id="cyc"></span></div>
  <div class="btns"><button class="pri" id="bs" onclick="api('start')">START</button><button id="bx" onclick="api('stop')">STOPP</button></div>
  <label class="sw"><input type="checkbox" id="fast"> Testmodus (1 Min = 1 Sek, wird nicht gespeichert)</label>
 </div>
</section>

<div class="right">
 <section class="card"><h3>Auswertung</h3>
  <div class="grid">
   <div class="kpi"><small>Heute</small><b id="k1">0</b><em>min</em></div>
   <div class="kpi"><small>Einheiten</small><b id="k2">0</b></div>
   <div class="kpi"><small>Gesamt</small><b id="k3">0</b><em>h</em></div>
   <div class="kpi"><small>Abschlussquote</small><b id="k4">–</b><em>%</em></div>
  </div>
  <h3 style="margin-top:18px">Letzte 7 Tage · Fokusminuten</h3>
  <div class="chart" id="chart"></div>
 </section>

 <section class="card"><h3>Verbindung · ESP32</h3>
  <div class="grid">
   <div class="kpi"><small>Latenz</small><b id="c1">–</b><em>ms</em></div>
   <div class="kpi"><small>Ø / Max</small><b id="c2" style="font-size:17px">–</b></div>
   <div class="kpi"><small>WLAN-Signal</small><div class="sig" id="sig"><i></i><i></i><i></i><i></i></div> <span class="mono" id="rssi" style="font-size:12px;color:var(--mu)"></span></div>
   <div class="kpi"><small>Chip-Temperatur</small><b id="c5">–</b><em>°C</em></div>
   <div class="kpi"><small>Poti (ADC)</small><b id="c6" style="font-size:18px">–</b><em id="c7"></em></div>
   <div class="kpi"><small>LED-Selbsttest</small><button onclick="api('ledtest')" style="padding:6px 10px;font-size:10px;margin-top:4px">ROT · GELB · GRÜN</button></div>
   <div class="kpi"><small>Durchsatz</small><b id="c4" style="font-size:18px">–</b><button onclick="speed()" id="sb" style="padding:4px 10px;margin-left:6px;font-size:10px">TEST</button></div>
  </div>
  <svg class="spark" id="spark" viewBox="0 0 300 54" preserveAspectRatio="none"><polyline id="pl" fill="none" stroke="#38bdf8" stroke-width="2" vector-effect="non-scaling-stroke"/></svg>
  <div class="row mono" style="font-size:11px;color:var(--mu);margin-top:6px"><span id="ip">IP –</span><span id="up">Laufzeit –</span><span id="hp">Heap –</span></div>
 </section>

 <section class="card"><h3>Lernlogik (wissenschaftsbasiert)</h3>
  <div class="sci">
   <div style="color:var(--g)"><b></b><span style="color:var(--tx)"><u style="text-decoration:none"><strong>Grün · 0–75 %</strong></u> – tiefe Konzentration, beste Aufnahme.</span></div>
   <div style="color:var(--y)"><b></b><span style="color:var(--tx)"><strong>Gelb · 75–100 %</strong> – Aufmerksamkeit sinkt; Aufgabe sauber abschließen.</span></div>
   <div style="color:var(--r)"><b></b><span style="color:var(--tx)"><strong>Rot · Pause</strong> – 20 % der Lernzeit (5–15 min), jede 4. Einheit 25 min.<small>Bewegen, Wasser trinken, Blick in die Ferne (20-20-20), kein Handy. Basis: Pomodoro-Technik &amp; ultradiane Leistungszyklen.</small></span></div>
  </div>
 </section>

 <section class="card"><h3>Ereignisprotokoll</h3><ul id="log"></ul></section>
</div>
</main>

<script>
const $=id=>document.getElementById(id),C=703.7;
const COL={idle:'#38bdf8',green:'#22e58b',yellow:'#ffc53d',red:'#ff4d5e'};
let ringT,S=null,lastAlarm=null,lastLight=null,lat=[],fails=0,online=null,drag=false,audio,t0boot=performance.now();

/* ---------- Helfer ---------- */
const pad=n=>String(n).padStart(2,'0');
const fmt=s=>pad(Math.floor(s/60))+':'+pad(Math.floor(s%60));
function log(m){const li=document.createElement('li');li.innerHTML='<span>'+new Date().toLocaleTimeString('de-AT')+'</span>'+m;$('log').prepend(li);while($('log').children.length>30)$('log').lastChild.remove()}
function toast(m){const t=document.createElement('div');t.className='toast';t.textContent=m;$('toasts').append(t);setTimeout(()=>{t.style.transition='.5s';t.style.opacity=0;setTimeout(()=>t.remove(),500)},5000)}
function num(el,v){const o=el._v||0;el._v=v;const s=performance.now();(function f(){const p=Math.min(1,(performance.now()-s)/700);el.textContent=Math.round(o+(v-o)*(1-Math.pow(1-p,3)));if(p<1)requestAnimationFrame(f)})()}
function beep(kind){try{audio=audio||new(window.AudioContext||webkitAudioContext)();const q=kind==='break'?[880,660,880,660,880,660]:[523,659,784];q.forEach((f,i)=>{const o=audio.createOscillator(),g=audio.createGain();o.type='square';o.frequency.value=f;o.connect(g);g.connect(audio.destination);const t=audio.currentTime+i*.28;g.gain.setValueAtTime(.0001,t);g.gain.exponentialRampToValueAtTime(.9,t+.02);g.gain.exponentialRampToValueAtTime(.0001,t+.24);o.start(t);o.stop(t+.26)})}catch(e){}}
document.addEventListener('click',()=>{try{audio=audio||new(window.AudioContext||webkitAudioContext)();audio.resume()}catch(e){}},{once:true});

/* ---------- Begrüßung ---------- */
const h=new Date().getHours();
$('wl').innerHTML=(h<11?'Guten Morgen':h<18?'Guten Tag':'Guten Abend')+', <b>willkommen am SmartDesk</b>';
setTimeout(()=>$('welcome').classList.add('hide'),2800);
setInterval(()=>$('clock').textContent=new Date().toLocaleTimeString('de-AT'),1000);

/* ---------- Alarm ---------- */
function alarm(type){
 const br=type==='break';
 $('alarm').style.setProperty('--ac',br?'#ff4d5e':'#22e58b');
 $('at').textContent=br?'PAUSE JETZT!':'PAUSE VORBEI';
 $('am').textContent=br?'Die Ampel ist ROT. Du hast gut gearbeitet – steh auf, beweg dich, trink etwas und gönn deinen Augen eine Pause.':'Du bist wieder erholt. Die Ampel ist bereit – starte die nächste Lerneinheit, wenn du soweit bist.';
 $('alarm').classList.remove('hide');beep(type);clearInterval(ringT);let rn=0;ringT=setInterval(()=>{if(++rn>=4)return clearInterval(ringT);beep(type)},2600);if(navigator.vibrate)navigator.vibrate([300,150,300]);
 document.title=(br?'🔴 PAUSE – ':'🟢 BEREIT – ')+'SmartDesk';log(br?'ALARM: Rot – Pause erforderlich':'Pause beendet – bereit');
}
function closeAlarm(){clearInterval(ringT);$('alarm').classList.add('hide');document.title='SmartDesk'}

/* ---------- Steuerung ---------- */
const api=p=>fetch('/api/'+p).then(poll).catch(()=>toast('Befehl fehlgeschlagen'));
$('sl').addEventListener('input',e=>{drag=true;$('sv').textContent=e.target.value+' min';brkTxt(+e.target.value)});
$('sl').addEventListener('change',e=>{fetch('/api/set?min='+e.target.value).then(()=>{drag=false;poll()})});
$('fast').addEventListener('change',e=>api('fast?on='+(e.target.checked?1:0)));
function brkMin(set,next){return next%4===0?25:Math.min(15,Math.max(5,Math.round(set*.2)))}
function brkTxt(set){if(!S)return;const n=S.sess+1;$('brk').textContent='Nächste Pause: '+brkMin(set,n)+' min'+(n%4===0?' (lang)':'');$('cyc').textContent='Einheit '+(((S.sess)%4)+1)+' / 4'}

/* ---------- Polling & Verbindung ---------- */
function setOnline(v){
 if(v===online)return;if(online!==null)log(v?'ESP32 wieder online':'ESP32 offline');online=v;
 $('net').classList.toggle('on',v);document.body.classList.toggle('off',!v);
 $('nettxt').textContent=v?'ESP32 ONLINE':'ESP32 OFFLINE';if(!v)$('netms').textContent='';
}
async function poll(){
 const t=performance.now();
 try{
  const r=await fetch('/api/state',{cache:'no-store',signal:AbortSignal.timeout(2500)});const j=await r.json();
  const ms=performance.now()-t;fails=0;lat.push(ms);if(lat.length>40)lat.shift();
  j.t0=performance.now();S=j;setOnline(true);$('netms').textContent=ms.toFixed(0)+' ms';stats(ms);render();
 }catch(e){if(++fails>=2)setOnline(false)}
}
setInterval(poll,1000);poll();

function stats(ms){
 $('c1').textContent=ms.toFixed(0);
 const avg=lat.reduce((a,b)=>a+b,0)/lat.length;$('c2').textContent=avg.toFixed(0)+' / '+Math.max(...lat).toFixed(0)+' ms';
 const mx=Math.max(60,...lat);$('pl').setAttribute('points',lat.map((v,i)=>(i/39*300).toFixed(1)+','+(52-v/mx*48).toFixed(1)).join(' '));
}
async function speed(){
 const b=$('sb');b.disabled=true;$('c4').textContent='…';let by=0,tm=0;
 try{for(let i=0;i<3;i++){const t=performance.now();const r=await fetch('/api/speed?'+i,{cache:'no-store'});const a=await r.arrayBuffer();tm+=performance.now()-t;by+=a.byteLength}
  $('c4').textContent=(by/1024/(tm/1000)).toFixed(0)+' KB/s'}catch(e){$('c4').textContent='Fehler'}
 b.disabled=false;
}

/* ---------- Rendering ---------- */
function render(){
 const s=S;
 if(lastAlarm===null){lastAlarm=s.alarm;log('Dashboard verbunden · '+s.ip);$('wp').textContent='› ESP32 online · '+s.ip+' · bereit'}
 else if(s.alarm!==lastAlarm){lastAlarm=s.alarm;alarm(s.at)}
 if(s.l!==lastLight){
  if(s.l==='yellow')toast('Gelb: Aufmerksamkeit lässt nach – Aufgabe bald abschließen.');
  if(s.l==='green'&&lastLight==='idle')log('Fokus gestartet · '+s.set+' min');
  lastLight=s.l;
 }
 const col=COL[s.l];document.documentElement.style.setProperty('--acc',col);
 $('lr').classList.toggle('on',s.l==='red');$('ly').classList.toggle('on',s.l==='yellow');$('lg').classList.toggle('on',s.l==='green'||s.l==='idle');
 const idle=s.s==='idle';
 $('phase').textContent=idle?'BEREIT':s.s==='break'?'PAUSE':s.l==='yellow'?'ENDSPURT':'FOKUS';
 $('sub').textContent=idle?'Zeit per Poti oder Slider wählen, dann START.':s.s==='break'?'Bewegen · trinken · Blick in die Ferne.':s.l==='yellow'?'Aufmerksamkeit sinkt – Aufgabe abschließen.':'Volle Konzentration – du bist im Flow.';
 $('sl').disabled=!idle;$('bs').disabled=!idle;$('bx').disabled=idle&&false;$('fast').disabled=!idle;$('fast').checked=!!s.fast;
 if(!drag){$('sl').value=s.set;$('sv').textContent=s.set+' min'}
 $('src').textContent=s.src?'POTENTIOMETER':'MANUELL';brkTxt(s.set);
 /* Auswertung */
 const today=s.w[6];num($('k1'),today);num($('k2'),s.sess);$('k3').textContent=(s.tot/60).toFixed(1);
 const q=s.sess+s.abort;$('k4').textContent=q?Math.round(s.sess/q*100):'–';
 const mx=Math.max(60,...s.w);const d=new Date();
 if(!$('chart').children.length)$('chart').innerHTML=s.w.map(()=>'<div class="bar"><b></b><i></i><span></span></div>').join('');
 [...$('chart').children].forEach((el,i)=>{const dd=new Date(d);dd.setDate(d.getDate()-(6-i));el.querySelector('b').textContent=s.w[i];el.querySelector('i').style.height=(s.w[i]/mx*100)+'%';el.querySelector('span').textContent=i===6?'Heute':dd.toLocaleDateString('de-AT',{weekday:'short'})});
 /* Temperatur & Poti */
 const T=s.temp,te=$('c5');te.textContent=T>-50?T.toFixed(1):'–';te.style.color=T>70?'var(--r)':T>55?'var(--y)':'var(--g)';
 $('c6').textContent=s.raw;$('c7').textContent=(s.raw/4095*3.3).toFixed(2)+' V';
 if(T>70&&!window.hot){window.hot=1;toast('ESP32 sehr warm ('+T.toFixed(0)+' °C) – Netzteil/Kabel prüfen, Belüftung sicherstellen.')}if(T<65)window.hot=0;
 /* Verbindung */
 const n=s.rssi>-55?4:s.rssi>-65?3:s.rssi>-75?2:s.rssi?1:0;[...$('sig').children].forEach((e,i)=>e.classList.toggle('on',i<n));
 $('rssi').textContent=s.rssi?s.rssi+' dBm':'AP-Modus';$('ip').textContent='IP '+s.ip;
 const u=s.up;$('up').textContent='Laufzeit '+Math.floor(u/3600)+'h '+pad(Math.floor(u/60)%60)+'m';$('hp').textContent='Heap '+(s.heap/1024).toFixed(0)+' KB';
}
/* weiche Anzeige zwischen den Polls */
setInterval(()=>{
 if(!S||!online)return;const idle=S.s==='idle';
 const rem=idle?S.set*60:Math.max(0,S.rem-(performance.now()-S.t0)/1000*(S.fast?60:1));
 $('time').textContent=fmt(rem);$('arc').style.strokeDashoffset=idle?0:C*(1-rem/S.len);
},200);
</script></body></html>)rawliteral";
