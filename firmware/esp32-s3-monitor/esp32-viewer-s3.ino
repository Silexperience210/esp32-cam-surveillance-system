/*
 * ESP32-S3 4827S043 — Moniteur multi-cameras tactile v2
 * ------------------------------------------------------
 * - Portail web de configuration WiFi (rien a coder)
 * - AUTO-SCAN du reseau : detecte les ESP32-CAM tout seul
 * - Clavier tactile : ajout manuel d'une camera par IP
 * - Vue grille multi-cams + plein ecran au tap
 *
 * Hardware : ESP32-4827S043C (ecran RGB 480x272 + tactile GT911)
 * Librairie : LovyanGFX
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <driver/i2c.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "esp_http_server.h"
#include "esp_heap_caps.h"

// ===================== CONFIGURATION =====================
// Rien a modifier ici : le WiFi se configure via le portail web,
// les cameras via l'ecran tactile (scan auto ou ajout manuel).

const int MAX_CAMS = 4;
const unsigned long GRID_CYCLE_MS = 900;    // tour complet de la grille
const int HTTP_TIMEOUT_MS = 2500;
const int PROBE_TIMEOUT_MS = 600;           // timeout par IP pendant le scan
const int ADD_TIMEOUT_MS = 2000;            // timeout pour l'ajout manuel (IP precise)

// ============ DRIVER ECRAN : ESP32-4827S043 (480x272 RGB) ===========

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB     _bus_instance;
  lgfx::Panel_RGB   _panel_instance;
  lgfx::Light_PWM   _light_instance;
  lgfx::Touch_GT911 _touch_instance;

  LGFX(void) {
    {
      auto cfg = _panel_instance.config();
      cfg.memory_width  = 480;
      cfg.memory_height = 272;
      cfg.panel_width   = 480;
      cfg.panel_height  = 272;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _panel_instance.config_detail();
      cfg.use_psram = 1;
      _panel_instance.config_detail(cfg);
    }
    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;
      cfg.pin_d0  = GPIO_NUM_8;   // B0
      cfg.pin_d1  = GPIO_NUM_3;   // B1
      cfg.pin_d2  = GPIO_NUM_46;  // B2
      cfg.pin_d3  = GPIO_NUM_9;   // B3
      cfg.pin_d4  = GPIO_NUM_1;   // B4
      cfg.pin_d5  = GPIO_NUM_5;   // G0
      cfg.pin_d6  = GPIO_NUM_6;   // G1
      cfg.pin_d7  = GPIO_NUM_7;   // G2
      cfg.pin_d8  = GPIO_NUM_15;  // G3
      cfg.pin_d9  = GPIO_NUM_16;  // G4
      cfg.pin_d10 = GPIO_NUM_4;   // G5
      cfg.pin_d11 = GPIO_NUM_45;  // R0
      cfg.pin_d12 = GPIO_NUM_48;  // R1
      cfg.pin_d13 = GPIO_NUM_47;  // R2
      cfg.pin_d14 = GPIO_NUM_21;  // R3
      cfg.pin_d15 = GPIO_NUM_14;  // R4
      cfg.pin_henable = GPIO_NUM_40;  // DE
      cfg.pin_vsync   = GPIO_NUM_41;
      cfg.pin_hsync   = GPIO_NUM_39;
      cfg.pin_pclk    = GPIO_NUM_42;
      cfg.freq_write  = 9000000;
      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 1;
      cfg.hsync_pulse_width = 1;
      cfg.hsync_back_porch  = 43;
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 3;
      cfg.vsync_pulse_width = 1;
      cfg.vsync_back_porch  = 12;
      cfg.pclk_active_neg   = 1;
      _bus_instance.config(cfg);
    }
    _panel_instance.setBus(&_bus_instance);
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = GPIO_NUM_2;
      _light_instance.config(cfg);
    }
    _panel_instance.light(&_light_instance);
    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;
      cfg.x_max = 480;
      cfg.y_min = 0;
      cfg.y_max = 272;
      cfg.pin_int = GPIO_NUM_18;
      cfg.pin_rst = GPIO_NUM_38;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = I2C_NUM_1;
      cfg.pin_sda = GPIO_NUM_19;
      cfg.pin_scl = GPIO_NUM_20;
      cfg.freq = 400000;
      cfg.i2c_addr = 0x5D;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX tft;

// ============================ ETAT GLOBAL ===========================

struct Cam { String name; String ip; };
Cam cams[4];
int nbCams = 0;

String cfgSsid, cfgPass;
bool configMode = false;

enum ViewMode { MODE_GRID, MODE_FULL, MODE_SCAN, MODE_ADD };
ViewMode viewMode = MODE_GRID;
int fullCamIndex = 0;
bool camOnline[4] = {false, false, false, false};
int gridCursor = 0;
unsigned long lastGridFetch = 0;

// saisie clavier
String inputIp;
String addMessage;
unsigned long addMessageMs = 0;

// bouton effacer (double tap confirmation)
bool pendingClear = false;
unsigned long pendingClearMs = 0;

const size_t JPG_BUF_SIZE = 512 * 1024;  // FIX : UXGA q=6 = 250-450 Ko
uint8_t* jpgBuf = nullptr;
size_t jpgLen = 0;

// ================== CONFIGURATION (flash NVS) =======================

void loadConfig() {
  Preferences prefs;
  prefs.begin("viewer", true);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  prefs.end();
}

void saveWifi() {
  Preferences prefs;
  prefs.begin("viewer", false);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.end();
}

void loadCams() {
  Preferences prefs;
  prefs.begin("viewer", true);
  nbCams = prefs.getUChar("ncam", 0);
  if (nbCams > MAX_CAMS) nbCams = MAX_CAMS;
  for (int i = 0; i < nbCams; i++) {
    cams[i].name = prefs.getString(("cam" + String(i) + "n").c_str(), "");
    cams[i].ip   = prefs.getString(("cam" + String(i) + "i").c_str(), "");
  }
  prefs.end();
}

void saveCams() {
  Preferences prefs;
  prefs.begin("viewer", false);
  prefs.putUChar("ncam", nbCams);
  for (int i = 0; i < nbCams; i++) {
    prefs.putString(("cam" + String(i) + "n").c_str(), cams[i].name);
    prefs.putString(("cam" + String(i) + "i").c_str(), cams[i].ip);
  }
  prefs.end();
}

// ==================== PORTAIL WEB CONFIG WIFI =======================

String urlDecode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < s.length()) {
      char h[3] = { s[i + 1], s[i + 2], 0 };
      out += (char)strtol(h, NULL, 16);
      i += 2;
    } else out += s[i];
  }
  return out;
}

String postParam(const String& body, const char* key) {
  String k = String(key) + "=";
  int i = body.indexOf(k);
  if (i < 0) return "";
  int start = i + k.length();
  int end = body.indexOf('&', start);
  if (end < 0) end = body.length();
  return urlDecode(body.substring(start, end));
}

String configPage() {
  String h = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Moniteur - Config WiFi</title><style>"
    "body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:20px auto;padding:0 15px}"
    "input{width:100%;padding:10px;margin:5px 0 15px;border-radius:6px;border:1px solid #444;background:#222;color:#eee;box-sizing:border-box}"
    "button{width:100%;padding:12px;background:#1976d2;color:#fff;border:0;border-radius:6px;font-size:16px;cursor:pointer}"
    "label{color:#aaa;font-size:14px}h2{text-align:center}"
    ".info{background:#1b3a1b;padding:10px;border-radius:6px;font-size:13px;margin-bottom:15px}"
    "</style></head><body>");
  h += "<h2>&#x1F4FA; Moniteur cameras — WiFi</h2>";
  h += "<div class='info'>Configure le WiFi du moniteur. Les cameras s'ajoutent ensuite directement sur l'ecran tactile (scan automatique ou ajout manuel).</div>";
  h += F("<form method='POST' action='/save'>"
    "<label>Nom du reseau WiFi (SSID)</label>"
    "<input name='ssid' list='nets' value='");
  h += cfgSsid;
  h += F("' required><datalist id='nets'></datalist>"
    "<label>Mot de passe WiFi</label>"
    "<input name='pass' type='password' placeholder='(laisser vide = inchange)'>"
    "<button type='submit'>&#x1F4BE; Enregistrer et redemarrer</button></form>"
    "<script>fetch('/scan').then(r=>r.json()).then(d=>{var dl=document.getElementById('nets');"
    "d.forEach(function(n){var o=document.createElement('option');o.value=n.ssid;o.label=n.rssi+' dBm';dl.appendChild(o);});});</script>"
    "</body></html>");
  return h;
}

static esp_err_t config_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  String page = configPage();
  return httpd_resp_send(req, page.c_str(), page.length());
}

static esp_err_t scan_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  int n = WiFi.scanNetworks();
  if (n > 20) n = 20;
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t save_handler(httpd_req_t* req) {
  char buf[512];
  int len = min((int)sizeof(buf) - 1, (int)req->content_len);
  int ret = httpd_req_recv(req, buf, len);
  if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
  if (ret >= (int)sizeof(buf)) ret = (int)sizeof(buf) - 1;  // FIX overflow
  buf[ret] = '\0';
  String body = buf;
  String s = postParam(body, "ssid");
  if (s.length()) cfgSsid = s;
  s = postParam(body, "pass");
  if (s.length()) cfgPass = s;
  saveWifi();

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req,
    "<html><head><meta charset='utf-8'></head>"
    "<body style='font-family:sans-serif;background:#111;color:#eee;text-align:center;padding-top:50px'>"
    "<h2>&#x2705; WiFi sauvegarde !</h2><p>Redemarrage... Regarde l'ecran du moniteur.</p></body></html>",
    HTTPD_RESP_USE_STRLEN);
  delay(2000);
  ESP.restart();
  return ESP_OK;
}

void startConfigPortal() {
  configMode = true;
  String mac = WiFi.macAddress();
  String apName = "ESP32-Viewer-Setup-" + mac.substring(12, 14) + mac.substring(15, 17);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str(), "12345678");
  Serial.println("\n=== MODE CONFIGURATION ===");
  Serial.println("Reseau WiFi : " + apName + "  (mdp : 12345678)");
  Serial.print("Portail : http://");
  Serial.println(WiFi.softAPIP());

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 4;
  httpd_handle_t server = NULL;
  httpd_uri_t form = { .uri = "/",     .method = HTTP_GET,  .handler = config_handler, .user_ctx = NULL };
  httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET,  .handler = scan_handler,   .user_ctx = NULL };
  httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler,   .user_ctx = NULL };
  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &form);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &save);
  }

  // Ecran d'instructions
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setFont(&fonts::Font4);
  tft.drawCentreString("Configuration requise", 240, 40);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.drawCentreString("1. Connecte-toi au WiFi :", 240, 95);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawCentreString(apName, 240, 118);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("mot de passe : 12345678", 240, 141);
  tft.drawCentreString("2. Ouvre dans ton navigateur :", 240, 180);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawCentreString("http://192.168.4.1", 240, 203);
  tft.setTextColor(0x7BC8, TFT_BLACK);
  tft.drawCentreString("Configure le WiFi puis le moniteur redemarre", 240, 240);
}

// ============================ RESEAU ================================

// Telecharge une photo JPEG depuis /capture d'une camera
bool fetchCapture(const String& ip) {
  HTTPClient http;
  http.begin("http://" + ip + "/capture");
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  int size = http.getSize();  // FIX : utilise Content-Length au lieu de timeout
  if (size <= 0 || (size_t)size > JPG_BUF_SIZE) { http.end(); return false; }

  WiFiClient* stream = http.getStreamPtr();
  size_t total = 0;
  unsigned long start = millis();
  while (total < (size_t)size && millis() - start < 3000) {
    int avail = stream->available();
    if (avail > 0) {
      int r = stream->readBytes(jpgBuf + total, min((size_t)avail, (size_t)size - total));
      if (r > 0) total += r;
    } else delay(1);
  }
  http.end();
  jpgLen = total;
  return (total == (size_t)size && jpgBuf[0] == 0xFF && jpgBuf[1] == 0xD8);
}

// Interroge /info d'une IP : true si c'est une ESP32-CAM (recupere son nom)
bool probeCam(const String& ip, String& name, int timeoutMs) {
  HTTPClient http;
  http.begin("http://" + ip + "/info");
  http.setTimeout(timeoutMs);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  String body = http.getString();
  http.end();
  if (body.indexOf("ESP32-CAM") < 0) return false;
  int n = body.indexOf("\"name\":\"");
  if (n >= 0) {
    int e = body.indexOf("\"", n + 8);
    if (e > n + 8) name = body.substring(n + 8, e);
    else name = ip;
  } else name = ip;
  return true;
}

// Cherche un motif dans un flux
bool findInStream(WiFiClient* s, const char* pat, uint32_t timeoutMs) {
  size_t plen = strlen(pat);
  size_t matched = 0;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (s->available()) {
      char c = (char)s->read();
      if (c == pat[matched]) {
        matched++;
        if (matched == plen) return true;
      } else {
        matched = (c == pat[0]) ? 1 : 0;
      }
    } else delay(1);
  }
  return false;
}

// Lit UNE frame JPEG du flux MJPEG dans jpgBuf
bool readOneFrame(WiFiClient* s) {
  if (!findInStream(s, "Content-Length:", 3000)) return false;
  char numBuf[12];
  int ni = 0;
  unsigned long start = millis();
  // peek() : ne pas consommer le \r final (sinon le \r\n\r\n suivant est casse)
  while (ni < 11 && millis() - start < 1000) {
    if (s->available()) {
      char c = (char)s->peek();
      if (c >= '0' && c <= '9') { numBuf[ni++] = (char)s->read(); }
      else if (ni > 0) break;          // fin du nombre, \r reste dans le flux
      else s->read();                  // ignore les espaces
    } else delay(1);
  }
  numBuf[ni] = 0;
  long len = atol(numBuf);
  if (len <= 0 || len > (long)JPG_BUF_SIZE) return false;
  if (!findInStream(s, "\r\n\r\n", 1000)) return false;
  size_t got = 0;
  start = millis();
  while (got < (size_t)len && millis() - start < 4000) {
    if (s->available()) {
      size_t chunk = min((size_t)s->available(), (size_t)len - got);
      int r = s->readBytes(jpgBuf + got, chunk);
      if (r > 0) got += r;
    } else delay(1);
  }
  jpgLen = got;
  return (got == (size_t)len && jpgBuf[0] == 0xFF && jpgBuf[1] == 0xD8);
}

// ============================ INTERFACE =============================

const int TOOLBAR_Y = 240;   // barre de boutons en bas (32 px)
const int GRID_H = 240;      // zone grille au-dessus

// ------- barre d'outils : [Scanner] [Ajouter] [Effacer] -------
void drawToolbar() {
  tft.fillRect(0, TOOLBAR_Y, 480, 32, 0x2104);
  const char* labels[3] = { "Scanner", "+ Ajouter", "Effacer" };
  uint16_t colors[3] = { 0x041F, 0x040F, 0x800F };  // bleu, vert, rouge fonce
  for (int i = 0; i < 3; i++) {
    int bx = 8 + i * 158;
    tft.fillRoundRect(bx, TOOLBAR_Y + 4, 144, 24, 5, colors[i]);
    tft.setTextColor(TFT_WHITE, colors[i]);
    tft.setFont(&fonts::Font2);
    tft.drawCentreString(labels[i], bx + 72, TOOLBAR_Y + 8);
  }
}

int toolbarAt(int tx, int ty) {
  if (ty < TOOLBAR_Y) return -1;
  for (int i = 0; i < 3; i++) {
    int bx = 8 + i * 158;
    if (tx >= bx && tx < bx + 144) return i;
  }
  return -1;
}

// ------- grille de cameras -------
void tileRect(int idx, int &x, int &y, int &w, int &h) {
  if (nbCams <= 2) {
    w = 480 / max(nbCams, 1); h = GRID_H;
    x = idx * w; y = 0;
  } else {
    w = 240; h = GRID_H / 2;
    x = (idx % 2) * 240;
    y = (idx / 2) * (GRID_H / 2);
  }
}

int tileAt(int tx, int ty) {
  for (int i = 0; i < nbCams; i++) {
    int x, y, w, h;
    tileRect(i, x, y, w, h);
    if (tx >= x && tx < x + w && ty >= y && ty < y + h) return i;
  }
  return -1;
}

void drawTileLabel(int idx) {
  int x, y, w, h;
  tileRect(idx, x, y, w, h);
  tft.setClipRect(x, y, w, h);
  tft.fillRect(x, y + h - 20, w, 20, TFT_BLACK);
  tft.setTextColor(camOnline[idx] ? TFT_WHITE : TFT_RED, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.setCursor(x + 6, y + h - 17);
  tft.print(cams[idx].name);
  if (!camOnline[idx]) tft.print(" (hors ligne)");
  tft.clearClipRect();
}

void drawOfflineTile(int idx) {
  int x, y, w, h;
  tileRect(idx, x, y, w, h);
  tft.setClipRect(x, y, w, h);
  tft.fillRect(x, y, w, h, 0x2104);
  tft.setTextColor(TFT_RED, 0x2104);
  tft.setFont(&fonts::Font4);
  tft.drawCentreString("OFFLINE", x + w / 2, y + h / 2 - 22);
  tft.clearClipRect();
  drawTileLabel(idx);
}

void drawTileJpg(int idx) {
  int x, y, w, h;
  tileRect(idx, x, y, w, h);
  tft.setClipRect(x, y, w, h);
  int jx = x + (w - 200) / 2;
  int jy = y + (h - 150) / 2;
  tft.drawJpg(jpgBuf, jpgLen, jx, jy, 200, 150, 0, 0, lgfx::jpeg_div::JPEG_DIV_8);  // FIX : UXGA/8 = 200x150 exact
  tft.clearClipRect();
  drawTileLabel(idx);
}

void drawGridFrame() {
  tft.fillScreen(TFT_BLACK);
  if (nbCams == 0) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setFont(&fonts::Font4);
    tft.drawCentreString("Aucune camera", 240, 80);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setFont(&fonts::Font2);
    tft.drawCentreString("Touche [Scanner] pour detecter les cameras", 240, 125);
    tft.drawCentreString("ou [+ Ajouter] pour saisir une IP", 240, 145);
  } else {
    for (int i = 0; i < nbCams; i++) {
      int x, y, w, h;
      tileRect(i, x, y, w, h);
      tft.drawRect(x, y, w, h, 0x4208);
      drawTileLabel(i);
    }
  }
  drawToolbar();
}

void drawFullJpg() {
  // image 400x272 a x=40 : les bandes x<40 et x>440 ne sont pas touchees
  tft.drawJpg(jpgBuf, jpgLen, 40, 0, 400, 272, 0, 14, lgfx::jpeg_div::JPEG_DIV_4);  // FIX : UXGA/4 = 400x300, offY=14 centre
}

// ------- bouton LED (plein ecran, bande laterale gauche) -------
bool fullLedState = false;

void drawLedButton() {
  uint16_t col = fullLedState ? TFT_YELLOW : 0x4208;
  tft.fillRoundRect(4, 100, 32, 60, 6, col);
  tft.setTextColor(fullLedState ? TFT_BLACK : TFT_WHITE, col);
  tft.setFont(&fonts::Font2);
  tft.drawCentreString("LED", 20, 122);
}

void fetchLedState() {
  HTTPClient http;
  http.begin("http://" + cams[fullCamIndex].ip + "/led");
  http.setTimeout(1500);
  if (http.GET() == HTTP_CODE_OK) {
    String body = http.getString();
    fullLedState = body.indexOf("\"led\":true") >= 0;
  }
  http.end();
}

void toggleCamLed() {
  HTTPClient http;
  http.begin("http://" + cams[fullCamIndex].ip + "/led?state=toggle");
  http.setTimeout(1500);
  if (http.GET() == HTTP_CODE_OK) {
    String body = http.getString();
    fullLedState = body.indexOf("\"led\":true") >= 0;
    Serial.printf("LED %s : %s\n", cams[fullCamIndex].name.c_str(), fullLedState ? "ON" : "OFF");
  }
  http.end();
  drawLedButton();
}

// ------- ecran scan -------
void drawScanProgress(int i, const String& currentIp) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setFont(&fonts::Font4);
  tft.drawCentreString("Scan du reseau...", 240, 50);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.drawCentreString(currentIp, 240, 100);
  tft.drawCentreString(String(nbCams) + " camera(s) trouvee(s)", 240, 130);
  // barre de progression
  tft.drawRect(40, 170, 400, 22, TFT_WHITE);
  tft.fillRect(42, 172, (396 * i) / 254, 18, 0x040F);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(String((i * 100) / 254) + " %", 240, 172);
  // bouton annuler
  tft.fillRoundRect(180, 215, 120, 32, 6, 0x800F);
  tft.setTextColor(TFT_WHITE, 0x800F);
  tft.drawCentreString("Annuler", 240, 223);
}

// ------- clavier numerique (ajout manuel) -------
const int KEY_W = 116, KEY_H = 40, KEY_GAP = 6;
const int KEY_X0 = 52, KEY_Y0 = 88;
const char KEYS[4][3] = { {'1','2','3'}, {'4','5','6'}, {'7','8','9'}, {'.','0','<'} };

void drawIpField() {
  tft.fillRect(40, 36, 292, 40, TFT_BLACK);
  tft.drawRect(40, 36, 292, 40, TFT_WHITE);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setFont(&fonts::Font4);
  tft.setCursor(48, 44);
  tft.print(inputIp.length() ? inputIp : " ");
}

void drawNumpad() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.drawCentreString("Ajouter une camera (IP)", 240, 12);
  drawIpField();
  // boutons OK / X
  tft.fillRoundRect(340, 36, 64, 40, 6, 0x040F);
  tft.setTextColor(TFT_WHITE, 0x040F);
  tft.setFont(&fonts::Font2);
  tft.drawCentreString("OK", 372, 48);
  tft.fillRoundRect(412, 36, 52, 40, 6, 0x800F);
  tft.setTextColor(TFT_WHITE, 0x800F);
  tft.drawCentreString("X", 438, 48);
  // touches
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      int kx = KEY_X0 + c * (KEY_W + KEY_GAP);
      int ky = KEY_Y0 + r * (KEY_H + KEY_GAP);
      tft.fillRoundRect(kx, ky, KEY_W, KEY_H, 6, 0x2104);
      tft.setTextColor(TFT_WHITE, 0x2104);
      tft.setFont(&fonts::Font4);
      tft.drawCentreString(String(KEYS[r][c]), kx + KEY_W / 2, ky + 8);
    }
  }
  if (addMessage.length()) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setFont(&fonts::Font2);
    tft.drawCentreString(addMessage, 240, 262);
  }
}

// retourne la touche : '0'-'9', '.', '<' (effacer), 'O' (OK), 'C' (annuler), 0 = rien
char numpadKeyAt(int tx, int ty) {
  if (ty >= 36 && ty < 76) {
    if (tx >= 340 && tx < 404) return 'O';
    if (tx >= 412 && tx < 464) return 'C';
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      int kx = KEY_X0 + c * (KEY_W + KEY_GAP);
      int ky = KEY_Y0 + r * (KEY_H + KEY_GAP);
      if (tx >= kx && tx < kx + KEY_W && ty >= ky && ty < ky + KEY_H) return KEYS[r][c];
    }
  }
  return 0;
}

void waitRelease() {
  uint16_t tx, ty;
  while (tft.getTouch(&tx, &ty)) delay(10);
}

// ============================ MODES =================================

void updateTile(int idx) {
  if (fetchCapture(cams[idx].ip)) {
    bool wasOffline = !camOnline[idx];
    camOnline[idx] = true;
    drawTileJpg(idx);
    if (wasOffline) drawTileLabel(idx);
  } else {
    if (camOnline[idx]) {
      camOnline[idx] = false;
      drawOfflineTile(idx);
    }
  }
}

void startScan() {
  viewMode = MODE_SCAN;
  Cam found[MAX_CAMS];
  int nbFound = 0;
  bool cancelled = false;

  // 1) Decouverte mDNS (instantane) : les cameras a jour s'annoncent
  drawScanProgress(0, "Recherche mDNS...");
  int found = MDNS.queryService("esp32cam", "tcp");
  Serial.printf("mDNS : %d camera(s) detectee(s)\n", found);
  for (int i = 0; i < found && nbCams < MAX_CAMS; i++) {
    String ip = MDNS.IP(i).toString();
    String name;
    if (!probeCam(ip, name, ADD_TIMEOUT_MS)) name = MDNS.hostname(i);
    found[nbFound].ip = ip;
    found[nbFound].name = name;
    nbFound++;
    Serial.printf("  -> %s (%s)\n", name.c_str(), ip.c_str());
  }

  // 2) Fallback : scan IP complet si aucune camera mDNS trouvee
  if (nbFound > 0 && !cancelled) {
    for (int i = 0; i < nbFound; i++) cams[i] = found[i];
    nbCams = nbFound;
    saveCams();
    for (int i = 0; i < 4; i++) camOnline[i] = false;
    gridCursor = 0;
    viewMode = MODE_GRID;
    drawGridFrame();
    return;
  }

  IPAddress ip = WiFi.localIP();
  String subnet = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + ".";
  int myLast = ip[3];

  for (int i = 1; i <= 254 && !cancelled; i++) {
    if (i == myLast) continue;
    drawScanProgress(i, subnet + String(i));
    String name;
    if (probeCam(subnet + String(i), name, PROBE_TIMEOUT_MS)) {
      if (nbCams < MAX_CAMS) {
        cams[nbCams].ip = subnet + String(i);
        cams[nbCams].name = name;
        nbCams++;
        Serial.printf("Camera trouvee : %s (%s)\n", name.c_str(), (subnet + String(i)).c_str());
      }
    }
    // bouton Annuler (zone 180,215,120,32)
    uint16_t tx, ty;
    if (tft.getTouch(&tx, &ty)) {
      if (tx >= 180 && tx < 300 && ty >= 215 && ty < 247) {
        waitRelease();
        cancelled = true;
      }
    }
    delay(1);
  }
  saveCams();
  for (int i = 0; i < 4; i++) camOnline[i] = false;
  gridCursor = 0;
  viewMode = MODE_GRID;
  drawGridFrame();
}

void startAdd() {
  viewMode = MODE_ADD;
  inputIp = "";
  addMessage = "";
  drawNumpad();
}

void addTask() {
  uint16_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) return;
  char key = numpadKeyAt(tx, ty);
  waitRelease();
  if (!key) return;

  if (key >= '0' && key <= '9' && inputIp.length() < 15) {
    inputIp += key;
    drawIpField();
  } else if (key == '.' && inputIp.length() < 15 && inputIp.length() > 0 &&
             !inputIp.endsWith(".")) {
    inputIp += '.';
    drawIpField();
  } else if (key == '<') {
    inputIp.remove(inputIp.length() - 1);
    drawIpField();
  } else if (key == 'C') {
    viewMode = MODE_GRID;
    drawGridFrame();
  } else if (key == 'O') {
    if (inputIp.length() < 7) {
      addMessage = "IP incomplete !";
    } else {
      addMessage = "Test de " + inputIp + " ...";
      drawNumpad();
      String name;
      if (probeCam(inputIp, name, ADD_TIMEOUT_MS)) {
        if (nbCams < MAX_CAMS) {
          cams[nbCams].ip = inputIp;
          cams[nbCams].name = name;
          nbCams++;
          saveCams();
          addMessage = "Camera ajoutee : " + name;
        } else {
          addMessage = "Maximum 4 cameras !";
        }
      } else {
        addMessage = "Aucune camera a cette IP";
      }
      drawNumpad();
      delay(1500);
      if (nbCams > 0 && addMessage.startsWith("Camera ajoutee")) {
        viewMode = MODE_GRID;
        drawGridFrame();
      } else {
        addMessage = "";
        drawNumpad();
      }
    }
  }
}

void gridTask() {
  if (nbCams > 0) {
    unsigned long interval = GRID_CYCLE_MS / max(nbCams, 1);
    if (millis() - lastGridFetch >= interval) {
      lastGridFetch = millis();
      updateTile(gridCursor);
      gridCursor = (gridCursor + 1) % nbCams;
    }
  }

  uint16_t tx, ty;
  if (tft.getTouch(&tx, &ty)) {
    int btn = toolbarAt(tx, ty);
    if (btn >= 0) {
      waitRelease();
      if (btn == 0) startScan();
      else if (btn == 1) startAdd();
      else if (btn == 2) {
        // Effacer : double tap pour confirmer
        if (pendingClear && millis() - pendingClearMs < 3000) {
          nbCams = 0;
          saveCams();
          pendingClear = false;
          for (int i = 0; i < 4; i++) camOnline[i] = false;
          drawGridFrame();
        } else {
          pendingClear = true;
          pendingClearMs = millis();
          tft.fillRect(0, TOOLBAR_Y, 480, 32, 0x800F);
          tft.setTextColor(TFT_WHITE, 0x800F);
          tft.setFont(&fonts::Font2);
          tft.drawCentreString("Retouche [Effacer] pour confirmer", 240, TOOLBAR_Y + 8);
          delay(300);
          waitRelease();
          drawToolbar();
        }
      }
    } else if (nbCams > 0) {
      int idx = tileAt(tx, ty);
      if (idx >= 0) {
        waitRelease();
        fullCamIndex = idx;
        viewMode = MODE_FULL;
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setFont(&fonts::Font2);
        tft.drawCentreString(cams[idx].name + " - connexion...", 240, 130);
      }
    }
  }
}

void fullScreenTask() {
  HTTPClient http;
  http.begin("http://" + cams[fullCamIndex].ip + "/stream");
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setFont(&fonts::Font2);
    tft.drawCentreString("Camera hors ligne - tapez pour revenir", 240, 130);
    while (true) {
      uint16_t tx, ty;
      if (tft.getTouch(&tx, &ty)) { waitRelease(); break; }
      delay(20);
    }
    viewMode = MODE_GRID;
    drawGridFrame();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  Serial.printf("Stream plein ecran : %s\n", cams[fullCamIndex].name.c_str());
  fetchLedState();
  drawLedButton();
  unsigned long frames = 0, fpsStart = millis();

  while (http.connected()) {
    uint16_t tx, ty;
    if (tft.getTouch(&tx, &ty)) {
      if (tx < 40 && ty >= 90 && ty < 170) {
        waitRelease();
        toggleCamLed();        // tap sur le bouton LED
      } else {
        waitRelease();
        break;                 // tap ailleurs : retour grille
      }
    }
    if (!readOneFrame(stream)) break;
    drawFullJpg();
    frames++;
    if (millis() - fpsStart >= 10000) {
      Serial.printf("FPS : %.1f\n", frames / 10.0f);
      frames = 0; fpsStart = millis();
    }
    if (WiFi.status() != WL_CONNECTED) break;
  }
  http.end();
  viewMode = MODE_GRID;
  drawGridFrame();
}

// ============================ SETUP / LOOP ==========================

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Moniteur multi-cams v2 ===");

  jpgBuf = (uint8_t*)heap_caps_malloc(JPG_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!jpgBuf) {
    Serial.println("ERREUR : PSRAM introuvable !");
    while (true) delay(1000);
  }

  tft.init();
  tft.fillScreen(TFT_BLACK);
  tft.setBrightness(255);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::Font4);
  tft.drawCentreString("Moniteur cameras", 240, 100);

  loadConfig();

  bool wifiOk = false;
  if (cfgSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    WiFi.setSleep(false);
    tft.setFont(&fonts::Font2);
    tft.drawCentreString("Connexion WiFi...", 240, 150);
    Serial.print("Connexion WiFi '" + cfgSsid + "'");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(400);
      Serial.print(".");
    }
    Serial.println();
    wifiOk = (WiFi.status() == WL_CONNECTED);
  }

  if (!wifiOk) {
    startConfigPortal();
    return;
  }

  Serial.print("WiFi OK : ");
  Serial.println(WiFi.localIP());

  loadCams();
  Serial.printf("%d camera(s) configuree(s)\n", nbCams);

  // mDNS : permet de decouvrir les cameras du reseau
  if (MDNS.begin("esp32-viewer")) Serial.println("mDNS actif (viewer)");

  tft.fillScreen(TFT_BLACK);
  drawGridFrame();

  if (nbCams == 1) {
    fullCamIndex = 0;
    viewMode = MODE_FULL;
  }
}

void loop() {
  if (configMode) {
    // FIX : retry WiFi toutes les 2 min (meme bug que la camera)
    static unsigned long lastTry = 0;
    if (cfgSsid.length() > 0 && millis() - lastTry > 120000) {
      lastTry = millis();
      Serial.println("Portail actif : nouvelle tentative WiFi...");
      WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi retrouve -> redemarrage en mode normal");
        delay(500);
        ESP.restart();
      }
    }
    delay(100);
    return;
  }

  // reconnexion WiFi automatique
  static unsigned long lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry > 10000) {
    lastWifiRetry = millis();
    Serial.println("WiFi perdu, reconnexion...");
    WiFi.disconnect();
    WiFi.reconnect();
  }

  switch (viewMode) {
    case MODE_GRID: gridTask(); break;
    case MODE_FULL: fullScreenTask(); break;
    case MODE_ADD:  addTask(); break;
    case MODE_SCAN: break;  // startScan() est bloquant, appele depuis gridTask
  }
  delay(1);
}
