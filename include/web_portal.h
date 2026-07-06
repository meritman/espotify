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

const char* CURRENT_VERSION = "v1.1.0-pre";

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

// Shared CSS
static const char CSS[] PROGMEM = R"rawCSS(
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;background:#0d0d0d;color:#e0e0e0;min-height:100vh;display:flex;justify-content:center;align-items:flex-start;padding:16px}
.card{background:#1a1a2e;border-radius:16px;padding:24px;max-width:420px;width:100%;box-shadow:0 8px 32px rgba(0,0,0,.6)}
h1{font-size:1.4em;margin-bottom:4px;color:#1DB954}
.sub{color:#888;font-size:.85em;margin-bottom:20px}
.net{display:flex;align-items:center;justify-content:space-between;padding:12px 14px;margin:6px 0;background:#16213e;border-radius:10px;cursor:pointer;transition:background .2s}
.net:hover{background:#1a3a5c}
.net .name{font-weight:600;font-size:.95em}
.net .db{font-size:.75em;color:#1DB954}
.bars{display:flex;gap:2px;align-items:flex-end;height:16px}
.bars span{width:4px;background:#333;border-radius:1px}
.bars span.on{background:#1DB954}
.b1{height:4px}.b2{height:8px}.b3{height:12px}.b4{height:16px}
input[type=text],input[type=password]{width:100%;padding:12px 14px;border:1px solid #333;border-radius:10px;background:#0d1b2a;color:#fff;font-size:.95em;margin:8px 0;outline:none;transition:border .2s}
input:focus{border-color:#1DB954}
button,.btn{display:block;width:100%;padding:14px;border:none;border-radius:10px;background:#1DB954;color:#000;font-weight:700;font-size:1em;cursor:pointer;transition:background .2s;text-align:center;text-decoration:none;margin-top:12px}
button:hover,.btn:hover{background:#1ed760}
button.sec{background:#333;color:#fff}
button.sec:hover{background:#444}
button.danger{background:#e74c3c;color:#fff}
button.danger:hover{background:#c0392b}
.guide{background:#0d1b2a;border-left:3px solid #1DB954;padding:12px 14px;border-radius:0 10px 10px 0;margin:14px 0;font-size:.82em;line-height:1.6;color:#aaa}
.guide a{color:#1DB954;text-decoration:none}
.guide ol{padding-left:18px;margin:6px 0}
.success{text-align:center;padding:30px 0}
.success h2{color:#1DB954;font-size:1.5em;margin-bottom:10px}
.ip-box{background:#0d1b2a;border:2px solid #1DB954;border-radius:12px;padding:16px;text-align:center;margin:16px 0}
.ip-box a{color:#1DB954;font-size:1.3em;font-weight:700;text-decoration:none}
.now{text-align:center;padding:16px 0}
.now .track{font-size:1.1em;font-weight:700;color:#fff}
.now .artist{font-size:.9em;color:#1DB954;margin-top:4px}
.slider-row{display:flex;align-items:center;gap:12px;margin:10px 0}
.slider-row input[type=range]{flex:1;accent-color:#1DB954}
.slider-row .val{min-width:40px;text-align:right;font-size:.9em;color:#1DB954}
.pins{margin-top:16px;font-size:.82em}
.pins table{width:100%;border-collapse:collapse;margin-top:8px}
.pins td,.pins th{padding:6px 10px;border:1px solid #333;text-align:center}
.pins th{background:#16213e;color:#1DB954;font-weight:600}
.pins td:first-child{color:#1DB954;font-weight:600}
.err{color:#e74c3c;font-size:.85em;margin:8px 0}
.ok{color:#1DB954;font-size:.85em;margin:8px 0}
label{font-size:.85em;color:#aaa;margin-top:10px;display:block}
.loading{text-align:center;padding:20px;color:#888}
.sep{border:none;border-top:1px solid #222;margin:20px 0}
)rawCSS";

// WiFi Scan Page
static void handleWiFiPage() {
  int n = WiFi.scanNetworks();
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESPotify Setup</title><style>");
  html += FPSTR(CSS);
  html += F("</style></head><body><div class='card'><h1>ESPotify Setup</h1><p class='sub'>Select your WiFi network</p>");

  if (n <= 0) {
    html += F("<p class='loading'>No networks found. <a href='/' style='color:#1DB954'>Refresh</a></p>");
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
      // Basic HTML escape for quotes just in case
      String safeSsid = nets[i].ssid;
      safeSsid.replace("\"", "&quot;");
      html += safeSsid;
      html += F("\">");
      html += F("<div><div class='name'>");
      html += nets[i].ssid;
      html += F("</div><div class='db'>");
      html += String(nets[i].rssi);
      html += F(" dBm");
      if (nets[i].open) html += F(" (open)");
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

  html += F("<div id='pw' style='display:none;margin-top:16px'>"
    "<label>Network: <strong id='sn'></strong></label>"
    "<input type='hidden' id='ssid'>"
    "<input type='password' id='pass' placeholder='WiFi Password'>"
    "<button onclick='go()'>Connect</button>"
    "<p id='msg'></p></div>"
    "<script>"
    "function sel(e){var s=e.getAttribute('data-ssid');document.getElementById('ssid').value=s;document.getElementById('sn').textContent=s;"
    "document.getElementById('pw').style.display='block';}"
    "function go(){var s=document.getElementById('ssid').value,p=document.getElementById('pass').value;"
    "document.getElementById('msg').innerHTML='<p class=\"loading\">Connecting...</p>';"
    "fetch('/connect?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))"
    ".then(r=>r.json()).then(d=>{if(d.ok){document.querySelector('.card').innerHTML="
    "'<div class=\"success\"><h2>WiFi Connected!</h2><p class=\"sub\">Your device is now on the network</p>'"
    "+'<div class=\"ip-box\"><p style=\"color:#888;font-size:.85em\">Access setup at:</p>'"
    "+'<a href=\"http://'+d.ip+'/\" target=\"_blank\">http://'+d.ip+'/</a></div>'"
    "+'<p style=\"color:#666;font-size:.8em;margin-top:12px\">AP will shut down shortly.<br>Connect to your WiFi and visit the IP above.</p></div>'"
    "}else{document.getElementById('msg').innerHTML="
    "'<p class=\"err\">'+d.error+'</p>'}}).catch(()=>{document.getElementById('msg').innerHTML="
    "'<p class=\"err\">Connection failed</p>'})}"
    "</script></div></body></html>");

  server.send(200, "text/html", html);
}

// WiFi Connect Handler
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

// Spotify Setup Page
static void handleSpotifySetup() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESPotify - Spotify Setup</title><style>");
  html += FPSTR(CSS);
  html += F("</style></head><body><div class='card'><h1>Spotify Setup</h1><p class='sub'>Connect your Spotify account</p>");

  // Guide section
  html += F("<div class='guide'><strong>How to get your Spotify credentials:</strong><ol>"
    "<li>Go to <a href='https://developer.spotify.com/dashboard' target='_blank'>Spotify Developer Dashboard</a></li>"
    "<li>Click <strong>Create App</strong></li>"
    "<li>Set app name to anything (e.g. \"My TFT Display\")</li>"
    "<li>Set <strong>Redirect URI</strong> to:<br><code style='color:#1DB954;background:#000;padding:2px 6px;border-radius:4px'>http://127.0.0.1:8080/callback</code></li>"
    "<li>Check <strong>Web API</strong> checkbox</li>"
    "<li>Save, then open your app's <strong>Settings</strong></li>"
    "<li>Copy <strong>Client ID</strong> and <strong>Client Secret</strong> below</li>"
    "</ol></div>");

  html += F("<label>Client ID</label><input type='text' id='cid' placeholder='e.g. 07863120c68b...'>"
    "<label>Client Secret</label><input type='text' id='csec' placeholder='e.g. 9bfcfff01141...'>"
    "<button class='sec' onclick='linkSpotify()' id='linkBtn'>Link Spotify Account</button>"
    "<hr class='sep'>"
    "<div class='guide' style='margin-top:14px'><strong>After linking:</strong><br>"
    "Spotify will redirect to <code style='color:#1DB954'>http://127.0.0.1:8080/callback?code=...</code><br>"
    "This page won't load (that's normal). Copy the <strong>entire URL</strong> from your browser's address bar and paste it below.</div>"
    "<label>Paste Callback URL</label><input type='text' id='cburl' placeholder='http://127.0.0.1:8080/callback?code=AQD...'>"
    "<button onclick='saveSpotify()'>Complete Setup</button>"
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
    "document.getElementById('msg').innerHTML='<p class=\"loading\">Exchanging token...</p>';"
    "fetch('/spotify_save?cid='+encodeURIComponent(cid)+'&csec='+encodeURIComponent(csec)+'&cburl='+encodeURIComponent(cburl))"
    ".then(r=>r.json()).then(d=>{"
    "if(d.ok){document.querySelector('.card').innerHTML="
    "'<div class=\"success\"><h2>Setup Complete!</h2><p class=\"sub\">Your Spotify account is linked.</p>'"
    "+'<p style=\"color:#888;margin:10px 0\">Redirecting to control panel...</p></div>';"
    "setTimeout(()=>location.href='/',2000)"
    "}else{document.getElementById('msg').innerHTML='<p class=\"err\">'+d.error+'</p>'}"
    "}).catch(()=>{document.getElementById('msg').innerHTML='<p class=\"err\">Request failed</p>'})}"
    "</script></div></body></html>");

  server.send(200, "text/html", html);
}

// Spotify Token Exchange Handler
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

// Control Panel Page
static void handleControlPanel() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESPotify - Controls</title><style>");
  html += FPSTR(CSS);
  html += F("</style></head><body><div class='card'><h1>ESPotify</h1><p class='sub'>Now Playing</p>"
    "<div class='now'><div class='track' id='track'>-</div><div class='artist' id='artist'>-</div></div>"
    "<hr class='sep'>"
    "<label>Brightness</label>"
    "<div class='slider-row'>"
    "<input type='range' id='bright' min='10' max='255' step='5' value='255' oninput='bvUp()' onchange='saveBright()'>"
    "<span class='val' id='bval'>255</span>"
    "</div>"
    "<hr class='sep'>"
    "<label>Screen Timeout (seconds, 0 = never off, disabled when music activity)</label>"
    "<div class='slider-row'>"
    "<input type='range' id='tout' min='0' max='300' step='10' value='0' oninput='tvUp()' onchange='saveTout()'>"
    "<span class='val' id='tval'>0s</span>"
    "</div>"
    "<hr class='sep'>"
    "<button class='sec' onclick=\"fetch('/wake')\" style='margin-bottom:10px'>Wake Display</button>"
    "<label>Wake Button (GPIO pin, default: 0 = BOOT)</label>"
    "<div class='slider-row'>"
    "<input type='text' id='wpin' value='0' style='width:60px;text-align:center;margin:0'>"
    "<button class='sec' onclick='saveWake()' style='width:auto;padding:10px 20px;margin:0'>Set</button>"
    "</div>"
    "<hr class='sep'>"
    "<div class='pins'><strong style='color:#1DB954'>Pin Connections - ESP32 / ST7735 TFT</strong>"
    "<table><tr><th>Function</th><th>ESP32 Pin</th><th>TFT Pin</th></tr>"
    "<tr><td>MOSI (SDA)</td><td>GPIO 23</td><td>SDA</td></tr>"
    "<tr><td>SCK (SCL)</td><td>GPIO 18</td><td>SCK</td></tr>"
    "<tr><td>CS</td><td>GPIO 5</td><td>CS</td></tr>"
    "<tr><td>DC / A0</td><td>GPIO 2</td><td>DC</td></tr>"
    "<tr><td>RST</td><td>GPIO 4</td><td>RST</td></tr>"
    "<tr><td>Backlight</td><td>GPIO 22</td><td>BL / LED</td></tr>"
    "<tr><td>VCC</td><td>3.3V</td><td>VCC</td></tr>"
    "<tr><td>GND</td><td>GND</td><td>GND</td></tr>"
    "</table></div>"
    "<hr class='sep'>"
    "<button class='sec' onclick=\"fetch('/restart').then(()=>{this.textContent='Restarting...';setTimeout(()=>location.reload(),3000)})\">Restart Device</button>"
    "<button class='danger' onclick=\"if(confirm('Reset all settings? Device will reboot into setup mode.'))fetch('/reset').then(()=>setTimeout(()=>location.reload(),3000))\">Factory Reset</button>"
    "<button class='sec' onclick='checkOTA()' id='otaBtn' style='margin-top:10px'>Check for Updates</button>"
    "<script>"
    "function upd(d){document.getElementById('track').textContent=d.track||'-';"
    "document.getElementById('artist').textContent=d.artist||'-'}"
    "function bvUp(){document.getElementById('bval').textContent=document.getElementById('bright').value}"
    "function tvUp(){document.getElementById('tval').textContent=document.getElementById('tout').value+'s'}"
    "function saveBright(){fetch('/brightness?v='+document.getElementById('bright').value)}"
    "function saveTout(){fetch('/timeout?v='+document.getElementById('tout').value)}"
    "function saveWake(){fetch('/wakepin?v='+document.getElementById('wpin').value).then(r=>r.json()).then(d=>{if(d.ok)alert('Wake pin set to GPIO '+document.getElementById('wpin').value)})}"
    "function checkOTA(){"
    "let b=document.getElementById('otaBtn');b.textContent='Checking...';"
    "fetch('/ota_check').then(r=>r.json()).then(d=>{"
    "if(d.ok){"
    "  let h='';"
    "  if(d.stable_ver){"
    "    if(d.stable_ver===d.current) h+='<p class=\"ok\">Stable is up to date ('+d.stable_ver+')</p>';"
    "    else h+='<button class=\"btn\" onclick=\"doOTA(\\''+d.stable_url+'\\')\">Flash Stable ('+d.stable_ver+')</button>';"
    "  }"
    "  if(d.pre_ver){"
    "    if(d.pre_ver===d.current) h+='<p class=\"ok\">Pre-release is up to date ('+d.pre_ver+')</p>';"
    "    else h+='<button class=\"btn sec\" onclick=\"doOTA(\\''+d.pre_url+'\\')\">Flash Pre-release ('+d.pre_ver+')</button>';"
    "  }"
    "  if(h==='') h='<p>No updates found.</p>';"
    "  b.outerHTML='<div id=\"otaOpts\">'+h+'</div>';"
    "}else{b.textContent='Error';setTimeout(()=>b.textContent='Check for Updates',3000);}"
    "}).catch(()=>{b.textContent='Error';setTimeout(()=>b.textContent='Check for Updates',3000)})}"
    "function doOTA(url){"
    "let o=document.getElementById('otaOpts')||document.getElementById('otaBtn');"
    "o.innerHTML='<button class=\"btn sec\">Updating... Do not unplug!</button>';"
    "fetch('/ota_install?url='+encodeURIComponent(url));"
    "setTimeout(()=>location.reload(),35000);"
    "}"
    "function poll(){fetch('/status').then(r=>r.json()).then(d=>{upd(d);document.getElementById('tout').value=d.timeout||0;tvUp();document.getElementById('bright').value=d.brightness||255;bvUp();document.getElementById('wpin').value=d.wakepin}).catch(()=>{})}"
    "poll();setInterval(poll,3000);"
    "</script></div></body></html>");

  server.send(200, "text/html", html);
}

// Brightness Handler
static void handleBrightness() {
  uint8_t val = server.arg("v").toInt();
  if (val < 10) val = 10;  // minimum brightness
  NVStore::saveBrightness(val);
  ::applyBrightness(val);
  server.send(200, "application/json", "{\"ok\":true}");
}

// Status Handler (for polling)
static void handleStatus() {
  String json = "{";
  json += "\"track\":\"" + String(::spotify.title) + "\",";
  json += "\"artist\":\"" + String(::spotify.artist) + "\",";
  json += "\"playing\":" + String(::spotify.isPlaying ? "true" : "false") + ",";
  json += "\"timeout\":" + String(NVStore::creds.screenTimeout) + ",";
  json += "\"brightness\":" + String(NVStore::creds.brightness) + ",";
  json += "\"wakepin\":" + String(NVStore::creds.wakePin);
  json += "}";
  server.send(200, "application/json", json);
}

// Timeout Save Handler
static void handleTimeout() {
  uint16_t val = server.arg("v").toInt();
  NVStore::saveScreenTimeout(val);
  ::applyScreenTimeout(val);
  server.send(200, "application/json", "{\"ok\":true}");
}

// Factory Reset Handler
static void handleReset() {
  NVStore::clearAll();
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>");
  html += FPSTR(CSS);
  html += F("</style></head><body><div class='card'><div class='success'>"
    "<h2>Factory Reset</h2>"
    "<p class='sub'>All credentials cleared from NVS.</p>"
    "<p style='color:#888;margin:10px 0'>Device is rebooting into setup mode...</p>"
    "<p class='loading'>Please connect to <strong>ESPotify-Setup</strong> WiFi after reboot.</p>"
    "</div></div></body></html>");
  server.send(200, "text/html", html);
  delay(1500);
  ESP.restart();
}

// Restart Handler
static void handleRestart() {
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

// Wake Pin Handler
static void handleWakePin() {
  uint8_t pin = server.arg("v").toInt();
  NVStore::saveWakePin(pin);
  server.send(200, "application/json", "{\"ok\":true}");
}

// Wake Display Handler
static void handleWake() {
  ::wakeScreen();
  server.send(200, "application/json", "{\"ok\":true}");
}

// OTA Handlers
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

// Captive Portal Redirect
static void handleNotFound() {
  if (apActive) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// Root Router
static void handleRoot() {
  if (!NVStore::hasWiFi() || apActive) {
    handleWiFiPage();
  } else if (!NVStore::hasSpotify()) {
    handleSpotifySetup();
  } else {
    handleControlPanel();
  }
}

// Public API

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
