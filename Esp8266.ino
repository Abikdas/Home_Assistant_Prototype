/*
  ╔══════════════════════════════════════════════════════════╗
  ║  DESK CONTROL ESP8266  v2  —  NodeMCU                   ║
  ║  D1=GPIO5  D2=GPIO4  D5=GPIO14  D6=GPIO12  D8=GPIO15   ║
  ║  Onboard LED: GPIO2  (active LOW)                       ║
  ╠══════════════════════════════════════════════════════════╣
  ║  SERIAL -> Arduino @ 115200  (TX only, binary frames)   ║
  ║                                                          ║
  ║  FRAME:  A5 | CMD | LEN | PAYLOAD[LEN] | CHK            ║
  ║          CHK = CMD ^ LEN ^ every payload byte           ║
  ║                                                          ║
  ║    10 WIFI [s]     0 connecting, 1 connected, 2 failed  ║
  ║    12 LED  [n,on]  n = 0..4                             ║
  ║    13 SESS [on]                                         ║
  ║    14 SHUT [1/0]   1 = shutdown, 0 = wake               ║
  ║    15 BEAT [mask, mode, wifi, ip0..ip3]   every 5 s     ║
  ║         mode: 0 normal, 1 session, 2 shutdown           ║
  ║                                                          ║
  ║  The Arduino owns all text + animation.                 ║
  ║  The ESP only reports what happens.                     ║
  ╚══════════════════════════════════════════════════════════╝
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// ── config ────────────────────────────────────────────────────
const char* SSID = "Nothing 3a";
const char* PASS = "mina mina shimawa";

// ── pin map ───────────────────────────────────────────────────
const int LPIN[5] = {5, 4, 14, 12, 15}; // D1 D2 D5 D6 D8
#define ONBOARD_LED 2

// ── protocol ──────────────────────────────────────────────────
#define HDR     0xA5
#define C_WIFI  0x10
#define C_LED   0x12
#define C_SESS  0x13
#define C_SHUT  0x14
#define C_BEAT  0x15

enum { W_CONNECTING = 0, W_OK = 1, W_FAIL = 2 };

#define BEAT_MS          5000UL   // periodic state sync to the Arduino
#define WIFI_TIMEOUT_MS  15000UL  // boot-time connect limit
#define WIFI_DEBOUNCE_MS 3000UL   // link must stay changed this long before we report it

// ── state ─────────────────────────────────────────────────────
bool     lstate[5]  = {false, false, false, false, false};
bool     sessionOn  = false;
bool     isShutdown = false;

uint8_t  wifiLast   = W_CONNECTING;  // last wifi state reported to the Arduino
uint8_t  wifiCand   = W_CONNECTING;  // candidate state being debounced
uint32_t wifiCandT  = 0;

uint32_t beatT      = 0;
uint32_t heartT     = 0;
bool     heartS     = false;

ESP8266WebServer server(80);

// ── serial tx ─────────────────────────────────────────────────
void sendFrame(uint8_t cmd, const uint8_t* p, uint8_t len) {
  uint8_t chk = cmd ^ len;
  Serial.write((uint8_t)HDR);
  Serial.write(cmd);
  Serial.write(len);
  for (uint8_t i = 0; i < len; i++) {
    Serial.write(p[i]);
    chk ^= p[i];
  }
  Serial.write(chk);
}

uint8_t wifiNow() {
  return (WiFi.status() == WL_CONNECTED) ? W_OK : W_FAIL;
}

void sendWifi(uint8_t s) {
  sendFrame(C_WIFI, &s, 1);
}

// Full-state sync. Lets the Arduino self-heal if it reset or missed a frame.
void sendBeat() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 5; i++) if (lstate[i]) mask |= (uint8_t)(1 << i);

  uint8_t p[7];
  p[0] = mask;
  p[1] = isShutdown ? 2 : (sessionOn ? 1 : 0);
  p[2] = wifiNow();

  IPAddress ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
  for (uint8_t i = 0; i < 4; i++) p[3 + i] = ip[i];

  sendFrame(C_BEAT, p, 7);
  beatT = millis();
}

// ── handlers ──────────────────────────────────────────────────

// While in shutdown only /wake is accepted, so the Arduino's sleeping
// state and the ESP's state can never disagree.
bool blockedByShutdown() {
  if (!isShutdown) return false;
  server.send(409, "text/plain", "SHUTDOWN");
  return true;
}

void setLed(int n, bool on) {
  digitalWrite(LPIN[n], on ? HIGH : LOW);
  lstate[n] = on;
  uint8_t p[2] = { (uint8_t)n, (uint8_t)(on ? 1 : 0) };
  sendFrame(C_LED, p, 2);
}

void handleLed(bool on) {
  if (blockedByShutdown()) return;
  if (!server.hasArg("led")) { server.send(400, "text/plain", "NO LED"); return; }
  int n = server.arg("led").toInt();
  if (n < 0 || n > 4)         { server.send(400, "text/plain", "BAD LED"); return; }
  setLed(n, on);
  server.send(200, "text/plain", "OK");
}

void handleOn()  { handleLed(true);  }
void handleOff() { handleLed(false); }

void handleSessionOn() {
  if (blockedByShutdown()) return;
  sessionOn = true;
  for (uint8_t i = 0; i < 5; i++) { digitalWrite(LPIN[i], HIGH); lstate[i] = true; }
  uint8_t on = 1;
  sendFrame(C_SESS, &on, 1);
  server.send(200, "text/plain", "OK");
}

void handleSessionOff() {
  if (blockedByShutdown()) return;
  sessionOn = false;
  for (uint8_t i = 0; i < 5; i++) { digitalWrite(LPIN[i], LOW); lstate[i] = false; }
  uint8_t off = 0;
  sendFrame(C_SESS, &off, 1);
  server.send(200, "text/plain", "OK");
}

void handleShutdown() {
  if (!isShutdown) {
    isShutdown = true;
    sessionOn  = false;
    for (uint8_t i = 0; i < 5; i++) { digitalWrite(LPIN[i], LOW); lstate[i] = false; }
    uint8_t one = 1;
    sendFrame(C_SHUT, &one, 1);
  }
  server.send(200, "text/plain", "OK");
}

// Wake: Arduino plays its own boot scene. The beat right after restores
// mode + IP so the long-press IP screen works again immediately.
void handleWake() {
  if (isShutdown) {
    isShutdown = false;
    uint8_t zero = 0;
    sendFrame(C_SHUT, &zero, 1);
    sendBeat();
  }
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ── HTML page (split for dynamic state injection) ─────────────
// PAGE_A: everything up to and including <script>\n
// Injected: var st=[...],isSes=...,isSd=...;\n
// PAGE_B: rest of the JS and closing HTML
// This lets lstate[] and session/shutdown be reflected in the UI
// immediately on page load — no stale all-off default.

const char PAGE_A[] PROGMEM = R"raw(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk Control</title>
<style>
:root{
  --bg:#07091a;--card:#0f1322;--acc:#6366f1;--acc2:#8b5cf6;
  --glow:rgba(99,102,241,.22);--grn:#10b981;--red:#ef4444;
  --txt:#f1f5f9;--mut:#64748b;--bdr:rgba(255,255,255,.07);
  --ff:'Segoe UI',system-ui,sans-serif;--mono:'Consolas',monospace;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:var(--ff);background:var(--bg);color:var(--txt);
  min-height:100vh;display:flex;justify-content:center;
  padding:24px 14px 56px;transition:background .8s;}
body.sd{background:#000;}
body::before{content:'';position:fixed;inset:0;pointer-events:none;
  background:radial-gradient(ellipse 60% 40% at 50% 0%,rgba(99,102,241,.07),transparent);
  transition:opacity .8s;}
body.sd::before{opacity:0;}
.w{width:100%;max-width:420px;}
h1{text-align:center;font-size:24px;font-weight:700;letter-spacing:-.5px;margin-bottom:5px;}
h1 span{background:linear-gradient(135deg,#a5b4fc,var(--acc2));
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.sub{text-align:center;font-family:var(--mono);font-size:10px;
  color:var(--mut);letter-spacing:1.5px;margin-bottom:20px;}
.sb{display:flex;align-items:center;gap:8px;background:var(--card);
  border:1px solid var(--bdr);border-radius:10px;padding:10px 14px;
  margin-bottom:14px;font-size:11px;font-family:var(--mono);}
.dot{width:7px;height:7px;border-radius:50%;background:var(--mut);
  flex-shrink:0;transition:all .4s;}
.dot.on {background:var(--grn);box-shadow:0 0 8px var(--grn);}
.dot.ses{background:var(--acc);box-shadow:0 0 10px var(--acc);animation:dpulse 1.4s infinite;}
.dot.off{background:var(--red);box-shadow:0 0 7px var(--red);}
@keyframes dpulse{0%,100%{opacity:1}50%{opacity:.25}}
#stxt{flex:1;color:var(--mut);transition:color .3s;}
.cnt{font-size:10px;color:var(--mut);background:rgba(255,255,255,.05);
  border-radius:5px;padding:2px 8px;}
.cb{display:flex;gap:9px;margin-bottom:14px;}
.mbtn{flex:1;padding:13px;border-radius:12px;border:1px solid var(--bdr);
  background:var(--card);color:var(--txt);font-family:var(--ff);
  font-size:13px;font-weight:600;cursor:pointer;transition:all .25s;
  position:relative;overflow:hidden;}
.mbtn:hover{transform:translateY(-2px);box-shadow:0 8px 24px rgba(0,0,0,.4);}
.mbtn:active{transform:scale(.97);}
.bses{background:linear-gradient(135deg,var(--acc),var(--acc2));
  border-color:transparent;box-shadow:0 0 18px var(--glow);}
.bses.act{background:linear-gradient(135deg,var(--grn),#059669);
  box-shadow:0 0 22px rgba(16,185,129,.3);}
.bsd{border-color:rgba(239,68,68,.2);}
.bsd:hover{border-color:var(--red);}
.bsd.act{background:linear-gradient(135deg,var(--red),#dc2626);
  border-color:transparent;animation:sdp 1.8s infinite;}
@keyframes sdp{0%,100%{box-shadow:0 0 18px rgba(239,68,68,.4)}
  50%{box-shadow:0 0 36px rgba(239,68,68,.65)}}
.card{background:var(--card);border:1px solid var(--bdr);border-radius:14px;
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:10px;cursor:pointer;position:relative;overflow:hidden;
  transition:all .25s;}
.card:hover{border-color:rgba(255,255,255,.13);transform:translateY(-2px);
  box-shadow:0 8px 24px rgba(0,0,0,.3);}
.card:active{transform:scale(.98);}
.card.on{background:linear-gradient(135deg,rgba(99,102,241,.14),rgba(139,92,246,.09));
  border-color:rgba(99,102,241,.3);box-shadow:0 0 18px var(--glow);}
.card.on::after{content:'';position:absolute;top:0;left:0;right:0;height:1.5px;
  background:linear-gradient(90deg,transparent,var(--acc),transparent);
  animation:shine 2.2s infinite;}
@keyframes shine{0%{transform:translateX(-100%)}100%{transform:translateX(100%)}}
.cl{display:flex;align-items:center;gap:13px;padding:18px;flex:1;}
.ico{width:38px;height:38px;border-radius:10px;background:rgba(99,102,241,.08);
  display:flex;align-items:center;justify-content:center;font-size:18px;
  transition:all .25s;flex-shrink:0;}
.card.on .ico{background:rgba(99,102,241,.2);animation:ibounce .4s ease;}
@keyframes ibounce{0%,100%{transform:scale(1)}50%{transform:scale(1.18) rotate(6deg)}}
.nm{font-size:14px;font-weight:600;letter-spacing:-.1px;transition:color .25s;}
.card.on .nm{color:#fff;text-shadow:0 0 8px rgba(99,102,241,.4);}
.ind{width:7px;height:7px;border-radius:50%;background:var(--mut);
  margin-right:18px;opacity:.2;transition:all .35s;flex-shrink:0;}
.card.on .ind{opacity:1;background:var(--grn);
  box-shadow:0 0 9px rgba(16,185,129,.8),0 0 18px rgba(16,185,129,.3);
  animation:blink 2s infinite;}
@keyframes blink{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.4;transform:scale(.85)}}
.rpl{position:absolute;border-radius:50%;background:rgba(255,255,255,.2);
  transform:scale(0);animation:rp .5s ease-out;pointer-events:none;}
@keyframes rp{to{transform:scale(4);opacity:0}}
.ov{display:none;position:fixed;inset:0;z-index:10;
  background:rgba(7,9,26,.97);backdrop-filter:blur(20px);
  flex-direction:column;align-items:center;justify-content:center;gap:18px;}
.ov.show{display:flex;animation:ovIn .4s ease;}
@keyframes ovIn{from{opacity:0;transform:scale(.95)}to{opacity:1;transform:scale(1)}}
.badge{font-size:11px;font-family:var(--mono);letter-spacing:2px;color:var(--acc);
  border:1px solid rgba(99,102,241,.3);padding:5px 15px;border-radius:5px;
  animation:badgep 2s infinite;}
@keyframes badgep{0%,100%{box-shadow:0 0 0 0 rgba(99,102,241,0)}
  50%{box-shadow:0 0 0 6px rgba(99,102,241,.12)}}
.orbit{position:relative;width:92px;height:92px;}
.orbit-ring{position:absolute;inset:0;border-radius:50%;
  border:2px solid rgba(99,102,241,.25);animation:ospin 4s linear infinite;}
.orbit-dot{position:absolute;top:-5px;left:50%;transform:translateX(-50%);
  width:10px;height:10px;border-radius:50%;
  background:var(--acc);box-shadow:0 0 12px var(--acc);}
.orbit-inner{position:absolute;inset:16px;border-radius:50%;
  background:linear-gradient(135deg,rgba(99,102,241,.2),rgba(139,92,246,.1));
  display:flex;align-items:center;justify-content:center;font-size:24px;
  border:1px solid rgba(99,102,241,.2);}
@keyframes ospin{to{transform:rotate(360deg)}}
.otitle{font-size:34px;font-weight:700;letter-spacing:-1px;text-align:center;
  animation:tz .5s ease;}
@keyframes tz{from{transform:scale(.8);opacity:0}to{transform:scale(1);opacity:1}}
.otitle span{background:linear-gradient(135deg,#a5b4fc,#c4b5fd);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.osub{font-size:11px;color:var(--mut);font-family:var(--mono);letter-spacing:.5px;}
.eq{display:flex;gap:4px;align-items:flex-end;height:28px;}
.eqb{width:8px;border-radius:3px 3px 0 0;
  background:linear-gradient(to top,var(--acc),var(--acc2));
  animation:eqa 1s ease-in-out infinite;}
.eqb:nth-child(1){animation-duration:.72s;animation-delay:0s;}
.eqb:nth-child(2){animation-duration:.9s;animation-delay:.1s;}
.eqb:nth-child(3){animation-duration:.6s;animation-delay:.2s;}
.eqb:nth-child(4){animation-duration:1.1s;animation-delay:.05s;}
.eqb:nth-child(5){animation-duration:.8s;animation-delay:.15s;}
.eqb:nth-child(6){animation-duration:.75s;animation-delay:.3s;}
.eqb:nth-child(7){animation-duration:.95s;animation-delay:.08s;}
@keyframes eqa{0%,100%{height:5px;opacity:.4}50%{height:26px;opacity:1}}
.xbtn{padding:11px 28px;border-radius:10px;background:rgba(99,102,241,.14);
  border:1px solid rgba(99,102,241,.28);color:var(--txt);font-family:var(--ff);
  font-size:13px;font-weight:600;cursor:pointer;transition:all .22s;}
.xbtn:hover{background:rgba(99,102,241,.26);transform:translateY(-1px);}
.sds{display:none;position:fixed;inset:0;z-index:11;background:#000;
  flex-direction:column;align-items:center;justify-content:center;gap:18px;}
.sds.show{display:flex;animation:sdIn .7s ease;}
@keyframes sdIn{from{opacity:0}to{opacity:1}}
.scanlines{position:fixed;inset:0;pointer-events:none;
  background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,0,0,.14) 2px,rgba(0,0,0,.14) 4px);}
.sdmoon{font-size:52px;animation:mop 2.5s ease-in-out infinite;}
@keyframes mop{0%,100%{filter:drop-shadow(0 0 8px rgba(239,68,68,.25));transform:scale(1);}
  50%{filter:drop-shadow(0 0 24px rgba(239,68,68,.65));transform:scale(1.08);}}
.sdtitle{font-size:22px;font-weight:700;letter-spacing:3px;
  font-family:var(--mono);color:var(--red);animation:glitch 3.5s infinite;}
@keyframes glitch{
  0%,93%,100%{text-shadow:none;transform:none;}
  94%{text-shadow:2px 0 #f00,-2px 0 #0ff;transform:translateX(2px);}
  95%{text-shadow:-2px 0 #f00,2px 0 #0ff;transform:translateX(-2px);}
  96%{text-shadow:none;transform:none;}
  97%{text-shadow:3px 0 #f00,-3px 0 #0ff;transform:skewX(5deg);}
  98%{text-shadow:none;transform:none;}}
.sdsub{font-size:11px;color:#2a2a2a;font-family:var(--mono);letter-spacing:.5px;
  animation:sf 4s ease-in-out infinite;}
@keyframes sf{0%,100%{color:#2a2a2a}50%{color:#444}}
.wbtn{padding:10px 24px;border-radius:10px;background:rgba(239,68,68,.08);
  border:1px solid rgba(239,68,68,.22);color:var(--red);font-family:var(--ff);
  font-size:13px;font-weight:600;cursor:pointer;transition:all .22s;}
.wbtn:hover{background:rgba(239,68,68,.18);transform:translateY(-1px);}
@media(max-width:400px){.cb{flex-direction:column;}}
</style>
</head>
<body>
<div class="w">
  <h1><span>Desk</span> Control</h1>
  <p class="sub">AMBIENT SYSTEM &middot; v2</p>
  <div class="sb">
    <div class="dot" id="dot"></div>
    <span id="stxt">System ready</span>
    <span class="cnt"><span id="nc">0</span>/5</span>
  </div>
  <div class="cb">
    <button class="mbtn bses" id="bses" onclick="sesMode(event)">&#9889; Session</button>
    <button class="mbtn bsd"  id="bsd"  onclick="sdMode(event)">&#127769; Shutdown</button>
  </div>
  <div class="card" id="c0" onclick="tog(0,this,event)">
    <div class="cl"><div class="ico">&#128161;</div><div class="nm">Primary Light</div></div>
    <div class="ind"></div>
  </div>
  <div class="card" id="c1" onclick="tog(1,this,event)">
    <div class="cl"><div class="ico">&#128262;</div><div class="nm">Accent Light</div></div>
    <div class="ind"></div>
  </div>
  <div class="card" id="c2" onclick="tog(2,this,event)">
    <div class="cl"><div class="ico">&#10024;</div><div class="nm">Ambient Light</div></div>
    <div class="ind"></div>
  </div>
  <div class="card" id="c3" onclick="tog(3,this,event)">
    <div class="cl"><div class="ico">&#127769;</div><div class="nm">Night Light</div></div>
    <div class="ind"></div>
  </div>
  <div class="card" id="c4" onclick="tog(4,this,event)">
    <div class="cl"><div class="ico">&#9889;</div><div class="nm">Power Strip</div></div>
    <div class="ind"></div>
  </div>
</div>
<div class="ov" id="ov">
  <div class="badge">&#9679; SESSION ACTIVE</div>
  <div class="orbit">
    <div class="orbit-ring"><div class="orbit-dot"></div></div>
    <div class="orbit-inner">&#9889;</div>
  </div>
  <div class="otitle"><span>All Systems</span><br>Online</div>
  <div class="osub">FULL DESK &middot; SESSION LIVE</div>
  <div class="eq">
    <div class="eqb"></div><div class="eqb"></div><div class="eqb"></div>
    <div class="eqb"></div><div class="eqb"></div><div class="eqb"></div>
    <div class="eqb"></div>
  </div>
  <button class="xbtn" onclick="endSes()">End Session</button>
</div>
<div class="sds" id="sds">
  <div class="scanlines"></div>
  <div class="sdmoon">&#127769;</div>
  <div class="sdtitle">SHUTDOWN MODE</div>
  <div class="sdsub">ALL SYSTEMS OFFLINE &middot; ZZZ</div>
  <button class="wbtn" onclick="wake()">Wake Up</button>
</div>
<script>
)raw";

// PAGE_B: JS body — state vars are injected by handleRoot() before this
const char PAGE_B[] PROGMEM = R"raw(
function rpl(e,el){
  var r=document.createElement('span');r.className='rpl';
  var b=el.getBoundingClientRect(),s=Math.max(b.width,b.height);
  r.style.cssText='width:'+s+'px;height:'+s+'px;left:'+(e.clientX-b.left-s/2)+'px;top:'+(e.clientY-b.top-s/2)+'px';
  el.appendChild(r);setTimeout(function(){r.remove();},550);
}
function upd(){
  var n=st.filter(Boolean).length;
  document.getElementById('nc').textContent=n;
  var d=document.getElementById('dot'),t=document.getElementById('stxt');
  if(isSd){d.className='dot off';t.textContent='System offline';t.style.color='#ef4444';}
  else if(isSes){d.className='dot ses';t.textContent='Session live';t.style.color='#a5b4fc';}
  else if(n===0){d.className='dot';t.textContent='System ready';t.style.color='';}
  else{d.className='dot on';t.textContent=n+' output'+(n>1?'s':'')+' active';t.style.color='';}
}
function applyState(){
  for(var i=0;i<5;i++) document.getElementById('c'+i).classList.toggle('on',!!st[i]);
  if(isSes){document.getElementById('bses').classList.add('act');document.getElementById('ov').classList.add('show');}
  if(isSd){document.getElementById('bsd').classList.add('act');document.body.classList.add('sd');
    setTimeout(function(){document.getElementById('sds').classList.add('show');},100);}
  upd();
}
function tog(n,el,e){
  if(isSd)return;
  rpl(e,el);
  st[n]=st[n]?0:1;
  el.classList.toggle('on',!!st[n]);
  fetch('/'+(st[n]?'on':'off')+'?led='+n);
  upd();
}
function sesMode(e){
  if(isSd)return;
  rpl(e,document.getElementById('bses'));
  isSes=!isSes;
  document.getElementById('bses').classList.toggle('act',isSes);
  if(isSes){
    for(var i=0;i<5;i++){st[i]=1;document.getElementById('c'+i).classList.add('on');}
    fetch('/session_on');
    document.getElementById('ov').classList.add('show');
    upd();
  } else { endSes(); }
}
function endSes(){
  isSes=false;
  document.getElementById('ov').classList.remove('show');
  document.getElementById('bses').classList.remove('act');
  for(var i=0;i<5;i++){st[i]=0;document.getElementById('c'+i).classList.remove('on');}
  fetch('/session_off');
  upd();
}
function sdMode(e){
  rpl(e,document.getElementById('bsd'));
  isSd=true;isSes=false;
  document.getElementById('bsd').classList.add('act');
  document.getElementById('bses').classList.remove('act');
  document.getElementById('ov').classList.remove('show');
  document.body.classList.add('sd');
  for(var i=0;i<5;i++){st[i]=0;document.getElementById('c'+i).classList.remove('on');}
  fetch('/shutdown');
  upd();
  setTimeout(function(){document.getElementById('sds').classList.add('show');},650);
}
function wake(){
  isSd=false;
  document.getElementById('sds').classList.remove('show');
  document.getElementById('bsd').classList.remove('act');
  document.body.classList.remove('sd');
  fetch('/wake');
  upd();
}
applyState();
</script>
</body>
</html>)raw";

// ── dynamic page handler ──────────────────────────────────────
// Injects current lstate[], sessionOn, isShutdown into JS vars
// so the page loads already reflecting the actual board state
void handleRoot() {
  char stbuf[80];
  snprintf(stbuf, sizeof(stbuf),
    "var st=[%u,%u,%u,%u,%u],isSes=%s,isSd=%s;\n",
    lstate[0]?1:0, lstate[1]?1:0, lstate[2]?1:0,
    lstate[3]?1:0, lstate[4]?1:0,
    sessionOn   ? "true" : "false",
    isShutdown  ? "true" : "false");

  // Chunked transfer: PAGE_A + injected state + PAGE_B
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent_P(PAGE_A);
  server.sendContent(stbuf);
  server.sendContent_P(PAGE_B);
  server.sendContent(""); // finish chunked
}

// ── setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  for (uint8_t i = 0; i < 5; i++) {
    pinMode(LPIN[i], OUTPUT);
    digitalWrite(LPIN[i], LOW);
  }
  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, HIGH); // off initially

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(SSID, PASS);

  // Tell the Arduino we are connecting. The Nano's bootloader can still be
  // starting when the ESP boots, so repeat the frame every ~2 s while we wait.
  // The Arduino ignores duplicates, so this is harmless.
  sendWifi(W_CONNECTING);
  uint32_t wStart = millis(), lastTell = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wStart < WIFI_TIMEOUT_MS) {
    digitalWrite(ONBOARD_LED, !digitalRead(ONBOARD_LED));
    delay(250);
    if (millis() - lastTell >= 2000UL) { sendWifi(W_CONNECTING); lastTell = millis(); }
  }

  wifiLast = wifiNow();
  wifiCand = wifiLast;
  digitalWrite(ONBOARD_LED, wifiLast == W_OK ? LOW : HIGH);  // on = connected
  sendWifi(wifiLast);
  sendBeat();                                                // carries the IP

  server.on("/",            handleRoot);
  server.on("/on",          handleOn);
  server.on("/off",         handleOff);
  server.on("/session_on",  handleSessionOn);
  server.on("/session_off", handleSessionOff);
  server.on("/shutdown",    handleShutdown);
  server.on("/wake",        handleWake);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ── loop ──────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  // Heartbeat LED blink every 1.4 s (only while connected)
  if (millis() - heartT > 1400UL) {
    heartS = !heartS;
    digitalWrite(ONBOARD_LED, (WiFi.status() == WL_CONNECTED && heartS) ? LOW : HIGH);
    heartT = millis();
  }

  // WiFi lost / regained: report once the change has been stable for 3 s
  uint8_t w = wifiNow();
  if (w == wifiLast) {
    wifiCand = wifiLast;
  } else if (w != wifiCand) {
    wifiCand = w; wifiCandT = millis();
  } else if (millis() - wifiCandT >= WIFI_DEBOUNCE_MS) {
    wifiLast = w;
    sendWifi(w);
    sendBeat();   // new IP after a reconnect
  }

  // Periodic full-state sync (also runs during shutdown, with mode = 2)
  if (millis() - beatT >= BEAT_MS) sendBeat();
}
