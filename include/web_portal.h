#pragma once
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "nvs_store.h"

#include <Update.h>

// Web Portal (Captive Portal + Setup + Controls)

const char* CURRENT_VERSION = "v1.1.2-pre";

// Forward declarations from main (global scope)
extern String base64Encode(String text);
extern String spotifyAccessToken;
extern void refreshSpotifyToken();
extern SpotifyData spotify;
extern void applyScreenTimeout(uint16_t seconds);
extern void applyBrightness(uint8_t val);
extern void wakeScreen();

namespace Portal {

static WebServer server(80);
static DNSServer dnsServer;
static bool apActive = false;
static bool setupComplete = false;
static String assignedIP = "";

// ═══════════════════════════════════════════
//  Shared CSS — Glassmorphism + Animated BG
// ═══════════════════════════════════════════
static const char CSS[] PROGMEM = R"rawCSS(
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;color:#e0e0e0;min-height:100vh;display:flex;justify-content:center;align-items:flex-start;padding:16px;background:#0a0a1a;background:linear-gradient(135deg,#0a0a1a,#1a0a2e,#0a1628,#0d0d2b,#0a0a1a);background-size:400% 400%;animation:bg 15s ease infinite}
@keyframes bg{0%,100%{background-position:0% 50%}50%{background-position:100% 50%}}
@keyframes fadeIn{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:translateY(0)}}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
@keyframes spin{to{transform:rotate(360deg)}}
.card{background:rgba(26,26,46,.82);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);border:1px solid rgba(29,185,84,.12);border-radius:20px;padding:28px;max-width:440px;width:100%;box-shadow:0 8px 40px rgba(0,0,0,.5),0 0 80px rgba(29,185,84,.04);animation:fadeIn .5s ease}
h1{font-size:1.5em;margin-bottom:4px;color:#1DB954;letter-spacing:-.5px}
.sub{color:#777;font-size:.85em;margin-bottom:20px}
.net{display:flex;align-items:center;justify-content:space-between;padding:14px 16px;margin:6px 0;background:rgba(22,33,62,.5);border:1px solid rgba(255,255,255,.04);border-left:3px solid transparent;border-radius:12px;cursor:pointer;transition:all .25s ease}
.net:hover,.net.active{background:rgba(29,185,84,.08);border-left-color:#1DB954;transform:translateX(4px)}
.net.active{border-color:rgba(29,185,84,.25)}
.net .name{font-weight:600;font-size:.95em}
.net .meta{font-size:.72em;color:#666;margin-top:2px}
.bars{display:flex;gap:2px;align-items:flex-end;height:18px}
.bars span{width:4px;background:rgba(255,255,255,.08);border-radius:2px;transition:background .3s}
.bars span.on{background:#1DB954}
.b1{height:4px}.b2{height:8px}.b3{height:12px}.b4{height:16px}
input[type=text],input[type=password]{width:100%;padding:13px 16px;border:1px solid rgba(255,255,255,.08);border-radius:12px;background:rgba(10,22,40,.7);color:#fff;font-size:.95em;margin:8px 0;outline:none;transition:all .3s}
input:focus{border-color:#1DB954;box-shadow:0 0 20px rgba(29,185,84,.12)}
button,.btn{display:block;width:100%;padding:14px;border:none;border-radius:12px;background:linear-gradient(135deg,#1DB954,#1ed760);color:#000;font-weight:700;font-size:1em;cursor:pointer;transition:all .25s;text-align:center;text-decoration:none;margin-top:12px}
button:hover,.btn:hover{transform:translateY(-2px);box-shadow:0 6px 24px rgba(29,185,84,.25)}
button:active,.btn:active{transform:translateY(0)}
button.sec{background:rgba(255,255,255,.07);color:#ccc;border:1px solid rgba(255,255,255,.08)}
button.sec:hover{background:rgba(255,255,255,.12);box-shadow:0 4px 16px rgba(255,255,255,.04)}
button.danger{background:linear-gradient(135deg,#e74c3c,#c0392b);color:#fff}
button.danger:hover{box-shadow:0 6px 20px rgba(231,76,60,.25)}
.guide{background:rgba(10,22,40,.5);border-left:3px solid #1DB954;padding:14px 16px;border-radius:0 12px 12px 0;margin:14px 0;font-size:.82em;line-height:1.7;color:#999}
.guide a{color:#1DB954;text-decoration:none}
.guide ol{padding-left:18px;margin:6px 0}
.success{text-align:center;padding:30px 0}
.success h2{color:#1DB954;font-size:1.5em;margin-bottom:10px}
.ip-box{background:rgba(10,22,40,.7);border:1px solid rgba(29,185,84,.3);border-radius:14px;padding:18px;text-align:center;margin:16px 0;box-shadow:0 0 30px rgba(29,185,84,.08)}
.ip-box a{color:#1DB954;font-size:1.3em;font-weight:700;text-decoration:none}
.now{text-align:center;padding:20px 0}
.now .track{font-size:1.15em;font-weight:700;color:#fff}
.now .artist{font-size:.9em;color:#1DB954;margin-top:6px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:middle}
.dot.on{background:#1DB954;box-shadow:0 0 8px #1DB954;animation:pulse 2s infinite}
.dot.off{background:#555}
.slider-row{display:flex;align-items:center;gap:12px;margin:10px 0}
.slider-row input[type=range]{flex:1;-webkit-appearance:none;height:6px;border-radius:3px;background:rgba(255,255,255,.08);outline:none}
.slider-row input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;background:#1DB954;cursor:pointer;box-shadow:0 0 10px rgba(29,185,84,.35)}
.slider-row .val{min-width:40px;text-align:right;font-size:.9em;color:#1DB954;font-weight:600}
.sect{background:rgba(22,33,62,.3);border-radius:14px;padding:16px;margin:12px 0;border:1px solid rgba(255,255,255,.04)}
.sect-title{font-size:.8em;color:#1DB954;font-weight:600;text-transform:uppercase;letter-spacing:.5px;margin-bottom:10px}
details{margin-top:16px}
details summary{cursor:pointer;color:#1DB954;font-weight:600;font-size:.85em;padding:8px 0;outline:none;list-style:none}
details summary::before{content:'▸ ';transition:transform .2s}
details[open] summary::before{content:'▾ '}
details table{width:100%;border-collapse:collapse;margin-top:8px;font-size:.82em}
details td,details th{padding:7px 10px;border:1px solid rgba(255,255,255,.06);text-align:center}
details th{background:rgba(22,33,62,.5);color:#1DB954;font-weight:600}
details td:first-child{color:#1DB954;font-weight:600}
.err{color:#e74c3c;font-size:.85em;margin:8px 0}
.ok{color:#1DB954;font-size:.85em;margin:8px 0}
label{font-size:.85em;color:#777;margin-top:12px;display:block}
.loading{text-align:center;padding:20px;color:#888}
.sep{border:none;border-top:1px solid rgba(255,255,255,.05);margin:20px 0}
.ver{text-align:center;font-size:.7em;color:#444;margin-top:20px;letter-spacing:.5px}
.spinner{display:inline-block;width:16px;height:16px;border:2px solid rgba(255,255,255,.2);border-top-color:#1DB954;border-radius:50%;animation:spin .8s linear infinite;vertical-align:middle;margin-right:8px}
)rawCSS";

// ═══════════════════════════════════════════
//  WiFi Scan Page
// ═══════════════════════════════════════════
static void handleWiFiPage() {
  int n = WiFi.scanNetworks();
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESPotify Setup</title><style>");
  html += FPSTR(CSS);
  html += F("</style></head><body><div class='card'>"
    "<h1>&#127925; ESPotify</h1><p class='sub'>Select your WiFi network to get started</p>");

  if (n <= 0) {
    html += F("<p class='loading'>No networks found. <a href='/' style='color:#1DB954'>Scan again</a></p>");
  } else {
    // Deduplicate + sort by signal
    struct Net { String ssid; int rssi; bool open; };
    Net nets[20];
    int count = 0;
    for (int i = 0; i < n && count < 20; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      bool dup = false;
      for (int j = 0; j < count; j++) {
        if (nets[j].ssid == s) { dup = true; if (WiFi.RSSI(i) > nets[j].rssi) nets[j].rssi = WiFi.RSSI(i); break; }
      }
      if (!dup) { nets[count] = {s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN}; count++; }
    }
    // Sort by RSSI desc
    for (int i = 0; i < count - 1; i++)
      for (int j = i + 1; j < count; j++)
        if (nets[j].rssi > nets[i].rssi) { Net t = nets[i]; nets[i] = nets[j]; nets[j] = t; }

    for (int i = 0; i < count; i++) {
      int bars = (nets[i].rssi > -50) ? 4 : (nets[i].rssi > -65) ? 3 : (nets[i].rssi > -75) ? 2 : 1;
      html += F("<div class='net' role='button' onclick=\"sel(this)\" data-ssid=\"");
      String safeSsid = nets[i].ssid;
      safeSsid.replace("\"", "&quot;");
      html += safeSsid;
      html += F("\">");
      html += F("<div><div class='name'>");
      if (!nets[i].open) html += F("&#128274; ");
      html += nets[i].ssid;
      html += F("</div><div class='meta'>");
      html += String(nets[i].rssi);
      html += F(" dBm");
      if (nets[i].open) html += F(" &middot; Open");
      html += F("</div></div><div class='bars'>");
      for (int b = 1; b <= 4; b++) {
        html += F("<span class='b");
        html += String(b);
        if (b <= bars) html += F(" on");
        html += F("'></span>");
      }
      html += F("</div></div>");
    }
  }

  html += F("<div id='pw' style='display:none;margin-top:16px;animation:fadeIn .3s ease'>"
    "<div class='sect'>"
    "<label>Network: <strong id='sn' style='color:#1DB954'></strong></label>"
    "<input type='hidden' id='ssid'>"
    "<input type='password' id='pass' placeholder='Enter WiFi password'>"
    "<button onclick='go()'>Connect</button>"
    "</div>"
    "<p id='msg'></p></div>"
    "<script>"
    "var prev=null;"
    "function sel(e){var s=e.getAttribute('data-ssid');document.getElementById('ssid').value=s;document.getElementById('sn').textContent=s;"
    "document.getElementById('pw').style.display='block';if(prev)prev.classList.remove('active');e.classList.add('active');prev=e;}"
    "function go(){var s=document.getElementById('ssid').value,p=document.getElementById('pass').value;"
    "document.getElementById('msg').innerHTML='<p class=\"loading\"><span class=\"spinner\"></span>Connecting...</p>';"
    "fetch('/connect?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))"
    ".then(r=>r.json()).then(d=>{if(d.ok){document.querySelector('.card').innerHTML="
    "'<div class=\"success\"><h2>&#10003; WiFi Connected!</h2><p class=\"sub\">Your device is now on the network</p>'"
    "+'<div class=\"ip-box\"><p style=\"color:#777;font-size:.85em\">Access setup at:</p>'"
    "+'<a href=\"http://'+d.ip+'/\" target=\"_blank\">http://'+d.ip+'/</a></div>'"
    "+'<p style=\"color:#555;font-size:.8em;margin-top:12px\">AP will shut down shortly.<br>Connect to your WiFi and visit the IP above.</p></div>'"
    "}else{document.getElementById('msg').innerHTML="
    "'<p class=\"err\">'+d.error+'</p>'}}).catch(()=>{document.getElementById('msg').innerHTML="
    "'<p class=\"err\">Connection failed</p>'})"
    "}</script></div></body></html>");

  server.send(200, "text/html", html);
}

// ═══════════════════════════════════════════
//  WiFi Connect Handler
// ═══════════════════════════════════════════
static void handleConnect() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  if (ssid.length() == 0) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"No SSID provided\"}");
    return;
  }

  // Try connecting in STA mode while keeping AP alive
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    assignedIP = WiFi.localIP().toString();
    NVStore::saveWiFi(ssid, pass);

    String json = "{\"ok\":true,\"ip\":\"" + assignedIP + "\"}";
    server.send(200, "application/json", json);

    // Schedule AP shutdown after 30 seconds (handled in loop)
    setupComplete = true;
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Could not connect. Check password and try again.\"}");
  }
}

// ═══════════════════════════════════════════
//  Spotify Setup Page
// ═══════════════════════════════════════════
static void handleSpotifySetup() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESPotify - Spotify Setup</title><style>");
  html += FPSTR(CSS);
  html += F("</style></head><body><div class='card'>"
    "<h1>&#127925; Spotify Setup</h1><p class='sub'>Connect your Spotify account</p>");

  // Guide section
  html += F("<div class='guide'><strong>How to get your Spotify credentials:</strong><ol>"
    "<li>Go to <a href='https://developer.spotify.com/dashboard' target='_blank'>Spotify Developer Dashboard</a></li>"
    "<li>Click <strong>Create App</strong></li>"
    "<li>Set app name to anything (e.g. \"My TFT Display\")</li>"
    "<li>Set <strong>Redirect URI</strong> to:<br><code style='color:#1DB954;background:rgba(0,0,0,.4);padding:3px 8px;border-radius:6px;font-size:.95em'>http://127.0.0.1:8080/callback</code></li>"
    "<li>Check <strong>Web API</strong> checkbox</li>"
    "<li>Save, then open your app's <strong>Settings</strong></li>"
    "<li>Copy <strong>Client ID</strong> and <strong>Client Secret</strong> below</li>"
    "</ol></div>");

  html += F("<div class='sect'>"
    "<div class='sect-title'>Step 1 &mdash; Enter Credentials</div>"
    "<label>Client ID</label><input type='text' id='cid' placeholder='e.g. 07863120c68b...'>"
    "<label>Client Secret</label><input type='text' id='csec' placeholder='e.g. 9bfcfff01141...'>"
    "<button class='sec' onclick='linkSpotify()' id='linkBtn'>Open Spotify Authorization</button>"
    "</div>"
    "<hr class='sep'>"
    "<div class='sect'>"
    "<div class='sect-title'>Step 2 &mdash; Paste Callback</div>"
    "<div class='guide' style='margin-top:0;margin-bottom:12px'>"
    "Spotify will redirect to <code style='color:#1DB954'>http://127.0.0.1:8080/callback?code=...</code><br>"
    "This page won't load (that's normal). Copy the <strong>entire URL</strong> from your browser and paste it below.</div>"
    "<label>Callback URL</label><input type='text' id='cburl' placeholder='http://127.0.0.1:8080/callback?code=AQD...'>"
    "<button onclick='saveSpotify()'>Complete Setup</button>"
    "</div>"
    "<p id='msg'></p>"
    "<script>"
    "function linkSpotify(){"
    "var cid=document.getElementById('cid').value.trim();"
    "if(!cid){alert('Enter Client ID first');return;}"
    "var scope='user-read-currently-playing%20user-read-playback-state%20user-modify-playback-state';"
    "var redir=encodeURIComponent('http://127.0.0.1:8080/callback');"
    "var url='https://accounts.spotify.com/authorize?client_id='+cid+'&response_type=code&redirect_uri='+redir+'&scope='+scope;"
    "window.open(url,'_blank')}"
    "function saveSpotify(){"
    "var cid=document.getElementById('cid').value.trim(),"
    "csec=document.getElementById('csec').value.trim(),"
    "cburl=document.getElementById('cburl').value.trim();"
    "if(!cid||!csec||!cburl){document.getElementById('msg').innerHTML='<p class=\"err\">All fields required</p>';return;}"
    "document.getElementById('msg').innerHTML='<p class=\"loading\"><span class=\"spinner\"></span>Exchanging token...</p>';"
    "fetch('/spotify_save?cid='+encodeURIComponent(cid)+'&csec='+encodeURIComponent(csec)+'&cburl='+encodeURIComponent(cburl))"
    ".then(r=>r.json()).then(d=>{"
    "if(d.ok){document.querySelector('.card').innerHTML="
    "'<div class=\"success\"><h2>&#10003; Setup Complete!</h2><p class=\"sub\">Your Spotify account is linked.</p>'"
    "+'<p style=\"color:#777;margin:10px 0\">Redirecting to control panel...</p></div>';"
    "setTimeout(()=>location.href='/',2000)"
    "}else{document.getElementById('msg').innerHTML='<p class=\"err\">'+d.error+'</p>'}"
    "}).catch(()=>{document.getElementById('msg').innerHTML='<p class=\"err\">Request failed</p>'})"
    "}</script></div></body></html>");

  server.send(200, "text/html", html);
}

// ═══════════════════════════════════════════
//  Spotify Token Exchange Handler
// ═══════════════════════════════════════════
static void handleSpotifySave() {
  String cid   = server.arg("cid");
  String csec  = server.arg("csec");
  String cburl = server.arg("cburl");

  // Parse auth code from callback URL
  String code = "";
  int codeIdx = cburl.indexOf("code=");
  if (codeIdx != -1) {
    code = cburl.substring(codeIdx + 5);
    int ampIdx = code.indexOf('&');
    if (ampIdx != -1) code = code.substring(0, ampIdx);
  }

  if (code.length() == 0) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Could not find authorization code in URL. Make sure you pasted the full callback URL.\"}");
    return;
  }

  // Exchange code for refresh token
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15000);

  HTTPClient http;
  http.begin(client, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String auth = cid + ":" + csec;
  http.addHeader("Authorization", "Basic " + base64Encode(auth));

  String payload = "grant_type=authorization_code&code=" + code + "&redirect_uri=http%3A%2F%2F127.0.0.1%3A8080%2Fcallback";

  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, http.getStream());
    if (!error && doc.containsKey("refresh_token")) {
      String refreshToken = doc["refresh_token"].as<String>();
      NVStore::saveSpotify(cid, csec, refreshToken);

      // Also set the access token immediately so Spotify starts working
      spotifyAccessToken = doc["access_token"].as<String>();

      http.end();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    } else {
      http.end();
      String errMsg = error ? String(error.c_str()) : "No refresh_token in response";
      server.send(200, "application/json", "{\"ok\":false,\"error\":\"Token parse error: " + errMsg + "\"}");
      return;
    }
  } else {
    String body = http.getString();
    http.end();
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Spotify returned HTTP " + String(httpCode) + ". Check Client ID and Secret.\"}");
    return;
  }
}

// ═══════════════════════════════════════════
//  Control Panel Page
// ═══════════════════════════════════════════
static void handleControlPanel() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESPotify - Controls</title><style>");
  html += FPSTR(CSS);
  html += F("</style></head><body><div class='card'>"
    "<h1>&#127925; ESPotify</h1><p class='sub'>Control Panel</p>"
    // Now Playing section
    "<div class='sect'>"
    "<div class='now'><span class='dot off' id='pdot'></span>"
    "<span class='track' id='track'>-</span>"
    "<div class='artist' id='artist'>-</div></div>"
    "</div>"
    // Brightness section
    "<div class='sect'>"
    "<div class='sect-title'>&#9728; Brightness</div>"
    "<div class='slider-row'>"
    "<input type='range' id='bright' min='10' max='255' step='5' value='255' oninput='bvUp()' onchange='saveBright()'>"
    "<span class='val' id='bval'>255</span>"
    "</div></div>"
    // Timeout section
    "<div class='sect'>"
    "<div class='sect-title'>&#9203; Screen Timeout</div>"
    "<label style='margin-top:0'>Seconds (0 = always on, disabled during playback)</label>"
    "<div class='slider-row'>"
    "<input type='range' id='tout' min='0' max='300' step='10' value='0' oninput='tvUp()' onchange='saveTout()'>"
    "<span class='val' id='tval'>0s</span>"
    "</div></div>"
    // Wake + Pin section
    "<div class='sect'>"
    "<div class='sect-title'>&#128161; Display Wake</div>"
    "<button class='sec' onclick=\"fetch('/wake')\" style='margin-top:0;margin-bottom:10px'>Wake Display Now</button>"
    "<label>Wake Button GPIO (default: 0 = BOOT)</label>"
    "<div class='slider-row'>"
    "<input type='text' id='wpin' value='0' style='width:60px;text-align:center;margin:0'>"
    "<button class='sec' onclick='saveWake()' style='width:auto;padding:10px 20px;margin:0'>Set</button>"
    "</div></div>"
    // Pin reference (collapsible)
    "<details>"
    "<summary>Pin Connections &mdash; ESP32 / ST7735 TFT</summary>"
    "<table><tr><th>Function</th><th>ESP32 Pin</th><th>TFT Pin</th></tr>"
    "<tr><td>MOSI (SDA)</td><td>GPIO 23</td><td>SDA</td></tr>"
    "<tr><td>SCK (SCL)</td><td>GPIO 18</td><td>SCK</td></tr>"
    "<tr><td>CS</td><td>GPIO 5</td><td>CS</td></tr>"
    "<tr><td>DC / A0</td><td>GPIO 2</td><td>DC</td></tr>"
    "<tr><td>RST</td><td>GPIO 4</td><td>RST</td></tr>"
    "<tr><td>Backlight</td><td>GPIO 22</td><td>BL / LED</td></tr>"
    "<tr><td>VCC</td><td>3.3V</td><td>VCC</td></tr>"
    "<tr><td>GND</td><td>GND</td><td>GND</td></tr>"
    "</table></details>"
    "<hr class='sep'>"
    // Device actions
    "<button class='sec' onclick=\"fetch('/restart').then(()=>{this.textContent='Restarting...';setTimeout(()=>location.reload(),3000)})\">Restart Device</button>"
    "<button class='danger' onclick=\"if(confirm('Reset all settings? Device will reboot into setup mode.'))fetch('/reset').then(()=>setTimeout(()=>location.reload(),3000))\">Factory Reset</button>"
    "<button class='sec' onclick='checkOTA()' id='otaBtn' style='margin-top:10px'>Check for Updates</button>"
    "<p class='ver'>ESPotify ");
  html += CURRENT_VERSION;
  html += F("</p>"
    "<script>"
    "function upd(d){document.getElementById('track').textContent=d.track||'-';"
    "document.getElementById('artist').textContent=d.artist||'-';"
    "var dot=document.getElementById('pdot');if(d.playing){dot.className='dot on'}else{dot.className='dot off'}}"
    "function bvUp(){document.getElementById('bval').textContent=document.getElementById('bright').value}"
    "function tvUp(){document.getElementById('tval').textContent=document.getElementById('tout').value+'s'}"
    "function saveBright(){fetch('/brightness?v='+document.getElementById('bright').value)}"
    "function saveTout(){fetch('/timeout?v='+document.getElementById('tout').value)}"
    "function saveWake(){fetch('/wakepin?v='+document.getElementById('wpin').value).then(r=>r.json()).then(d=>{if(d.ok)alert('Wake pin set to GPIO '+document.getElementById('wpin').value)})}"
    "function checkOTA(){"
    "let b=document.getElementById('otaBtn');b.innerHTML='<span class=\"spinner\"></span>Checking...';"
    "fetch('/ota_check').then(r=>r.json()).then(d=>{"
    "if(d.ok){"
    "  let h='';"
    "  if(d.stable_ver){"
    "    if(d.stable_ver===d.current) h+='<p class=\"ok\">&#10003; Stable is up to date ('+d.stable_ver+')</p>';"
    "    else h+='<button class=\"btn\" onclick=\"doOTA(\\''+d.stable_url+'\\')\">"
    "Flash Stable ('+d.stable_ver+')</button>';"
    "  }"
    "  if(d.pre_ver){"
    "    if(d.pre_ver===d.current) h+='<p class=\"ok\">&#10003; Pre-release is up to date ('+d.pre_ver+')</p>';"
    "    else h+='<button class=\"btn sec\" onclick=\"doOTA(\\''+d.pre_url+'\\')\">"
    "Flash Pre-release ('+d.pre_ver+')</button>';"
    "  }"
    "  if(h==='') h='<p class=\"ok\">No updates available.</p>';"
    "  b.outerHTML='<div id=\"otaOpts\">'+h+'</div>';"
    "}else{b.textContent='Error';setTimeout(()=>{b.textContent='Check for Updates';b.innerHTML='Check for Updates'},3000);}"
    "}).catch(()=>{b.textContent='Error';setTimeout(()=>{b.textContent='Check for Updates';b.innerHTML='Check for Updates'},3000)})}"
    "function doOTA(url){"
    "let o=document.getElementById('otaOpts')||document.getElementById('otaBtn');"
    "o.innerHTML='<button class=\"btn sec\"><span class=\"spinner\"></span>Updating... Do not unplug!</button>';"
    "fetch('/ota_install?url='+encodeURIComponent(url));"
    "setTimeout(()=>location.reload(),35000);"
    "}"
    "function poll(){fetch('/status').then(r=>r.json()).then(d=>{upd(d);document.getElementById('tout').value=d.timeout||0;tvUp();document.getElementById('bright').value=d.brightness||255;bvUp();document.getElementById('wpin').value=d.wakepin}).catch(()=>{})}"
    "poll();setInterval(poll,3000);"
    "</script></div></body></html>");

  server.send(200, "text/html", html);
}

// ═══════════════════════════════════════════
//  Brightness Handler
// ═══════════════════════════════════════════
static void handleBrightness() {
  uint8_t val = server.arg("v").toInt();
  if (val < 10) val = 10;  // minimum brightness
  NVStore::saveBrightness(val);
  ::applyBrightness(val);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ═══════════════════════════════════════════
//  Status Handler (for polling) — JSON-safe
// ═══════════════════════════════════════════
static void handleStatus() {
  String track = String(::spotify.title);
  String artist = String(::spotify.artist);
  // Escape quotes to prevent broken JSON
  track.replace("\"", "\\\"");
  artist.replace("\"", "\\\"");
  track.replace("\\", "\\\\");
  artist.replace("\\", "\\\\");

  String json = "{";
  json += "\"track\":\"" + track + "\",";
  json += "\"artist\":\"" + artist + "\",";
  json += "\"playing\":" + String(::spotify.isPlaying ? "true" : "false") + ",";
  json += "\"timeout\":" + String(NVStore::creds.screenTimeout) + ",";
  json += "\"brightness\":" + String(NVStore::creds.brightness) + ",";
  json += "\"wakepin\":" + String(NVStore::creds.wakePin);
  json += "}";
  server.send(200, "application/json", json);
}

// ═══════════════════════════════════════════
//  Timeout Save Handler
// ═══════════════════════════════════════════
static void handleTimeout() {
  uint16_t val = server.arg("v").toInt();
  NVStore::saveScreenTimeout(val);
  ::applyScreenTimeout(val);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ═══════════════════════════════════════════
//  Factory Reset Handler
// ═══════════════════════════════════════════
static void handleReset() {
  NVStore::clearAll();
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>");
  html += FPSTR(CSS);
  html += F("</style></head><body><div class='card'><div class='success'>"
    "<h2 style='color:#e74c3c'>&#9888; Factory Reset</h2>"
    "<p class='sub'>All credentials have been cleared.</p>"
    "<p style='color:#777;margin:10px 0'>Device is rebooting into setup mode...</p>"
    "<p class='loading'><span class='spinner'></span>Please connect to <strong>ESPotify-Setup</strong> WiFi after reboot.</p>"
    "</div></div></body></html>");
  server.send(200, "text/html", html);
  delay(1500);
  ESP.restart();
}

// ═══════════════════════════════════════════
//  Restart Handler
// ═══════════════════════════════════════════
static void handleRestart() {
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

// ═══════════════════════════════════════════
//  Wake Pin Handler
// ═══════════════════════════════════════════
static void handleWakePin() {
  uint8_t pin = server.arg("v").toInt();
  NVStore::saveWakePin(pin);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ═══════════════════════════════════════════
//  Wake Display Handler
// ═══════════════════════════════════════════
static void handleWake() {
  ::wakeScreen();
  server.send(200, "application/json", "{\"ok\":true}");
}

// ═══════════════════════════════════════════
//  OTA Handlers
// ═══════════════════════════════════════════
static String otaUrl = "";

static void handleOTACheck() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(500, "application/json", "{\"ok\":false}");
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000); // 10s for TLS handshake
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, "https://raw.githubusercontent.com/meritman/espotify/main/version.json");
  
  int code = http.GET();
  if (code < 0) {
    delay(500); // Wait half a second and retry
    code = http.GET();
  }
  if (code == 200) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, http.getStream());
    
    String stableVer = doc["stable"]["version"] | "";
    String stableUrl = doc["stable"]["url"] | "";
    String preVer = doc["pre"]["version"] | "";
    String preUrl = doc["pre"]["url"] | "";
    
    String resp = "{\"ok\":true, \"current\":\"" + String(CURRENT_VERSION) + "\",";
    resp += "\"stable_ver\":\"" + stableVer + "\", \"stable_url\":\"" + stableUrl + "\",";
    resp += "\"pre_ver\":\"" + preVer + "\", \"pre_url\":\"" + preUrl + "\"}";
    
    server.send(200, "application/json", resp);
  } else {
    server.send(500, "application/json", "{\"ok\":false}");
  }
  http.end();
}

static void handleOTAInstall() {
  String url = server.arg("url");
  if (url.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  otaUrl = url;
  
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);
  
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);
  HTTPClient http;
  http.setTimeout(10000);
  
  http.begin(client, otaUrl);
  int code = http.GET();
  if (code < 0) {
    delay(500);
    code = http.GET();
  }
  
  if (code == 200 || code == 301 || code == 302) {
    if (code == 301 || code == 302) {
      otaUrl = http.getLocation();
      http.end();
      http.begin(client, otaUrl);
      code = http.GET();
      if (code < 0) {
        delay(500);
        code = http.GET();
      }
    }
  }

  if (code == 200) {
    int len = http.getSize();
    if (Update.begin(len)) {
      WiFiClient * stream = http.getStreamPtr();
      size_t written = Update.writeStream(*stream);
      if (written == len) {
        if (Update.end()) {
          if (Update.isFinished()) {
            ESP.restart();
          }
        }
      }
    }
  }
  http.end();
}

// ═══════════════════════════════════════════
//  Captive Portal Redirect
// ═══════════════════════════════════════════
static void handleNotFound() {
  if (apActive) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// ═══════════════════════════════════════════
//  Root Router
// ═══════════════════════════════════════════
static void handleRoot() {
  if (!NVStore::hasWiFi() || apActive) {
    handleWiFiPage();
  } else if (!NVStore::hasSpotify()) {
    handleSpotifySetup();
  } else {
    handleControlPanel();
  }
}

// ═══════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════

inline void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESPotify-Setup");
  delay(100);

  // DNS server for captive portal redirect
  dnsServer.start(53, "*", WiFi.softAPIP());
  apActive = true;

  server.on("/", handleRoot);
  server.on("/connect", handleConnect);
  server.on("/spotify_setup", handleSpotifySetup);
  server.on("/spotify_save", handleSpotifySave);
  server.on("/status", handleStatus);
  server.on("/timeout", handleTimeout);
  server.on("/brightness", handleBrightness);
  server.on("/reset", handleReset);
  server.on("/restart", handleRestart);
  server.on("/wakepin", handleWakePin);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[Portal] AP started: ESPotify-Setup @ " + WiFi.softAPIP().toString());
}

inline void startSTA() {
  server.on("/", handleRoot);
  server.on("/spotify_setup", handleSpotifySetup);
  server.on("/spotify_save", handleSpotifySave);
  server.on("/status", handleStatus);
  server.on("/timeout", handleTimeout);
  server.on("/brightness", handleBrightness);
  server.on("/reset", handleReset);
  server.on("/restart", handleRestart);
  server.on("/wakepin", handleWakePin);
  server.on("/wake", handleWake);
  server.on("/ota_check", handleOTACheck);
  server.on("/ota_install", handleOTAInstall);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[Portal] Web server started @ " + WiFi.localIP().toString());
}

inline void loop() {
  if (apActive) dnsServer.processNextRequest();
  server.handleClient();
}

inline void shutdownAP() {
  if (apActive) {
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apActive = false;
    Serial.println("[Portal] AP shutdown");
  }
}

}  // namespace Portal
