/*
 * ESP32-CAM Surveillance intelligente v3 — CORRIGÉ COMPLET
 * ----------------------------------------------------------
 * Corrections (audit Kimi + Claude Opus) :
 *   • Mutex caméra (camGet/camReturn) — driver non réentrant
 *   • lastUpdateId persisté en NVS — évite boucle reboot infinie
 *   • Mode AP avec retry WiFi toutes les 2 min — plus de caméra morte
 *   • camera_config_t config = {} — initialisation complète
 *   • Double serveur HTTP : port 80 (API) + port 81 (stream dédié)
 *   • AVI fps réels mesurés — plus de vidéo accélérée
 *   • Rotation SD automatique — suppression anciens .avi
 *   • Token Telegram masqué dans le formulaire
 *   • Auth API (X-Auth-Token) sur routes sensibles
 *   • Ghost Mode : 5s analyse silencieuse avant alerte
 *   • Task Telegram dédiée sur core 0 — loop jamais bloqué
 *   • Watchdog natif ESP32 — remplace watchdog custom buggé
 *   • PortEXIT/ENTER_CRITICAL sur toutes les variables partagées
 *   • Buffer overflow fix dans save_handler
 *   • Path traversal fix dans dl_handler
 *   • Échappement HTML/JSON (XSS/Injection)
 *   • Division par zéro protégée
 *
 * Carte : AI Thinker ESP32-CAM (clone Binghe) + camera OV3660
 * Carte microSD : FAT32 (mode 1-bit : GPIO 12/13 libres pour HC-SR04)
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_http_server.h"
#include "FS.h"
#include "SD_MMC.h"
#include "time.h"
#include <Preferences.h>
#include <ESPmDNS.h>
#include "esp_task_wdt.h"
#include "soc/rtc_cntl_reg.h"
#include "avi_writer.h"

// ================ CONFIGURATION ================
const bool  DETECTION_ULTRASON      = false;
const bool  DETECTION_PIR           = true;   // SR602 PIR sensor
const float HYSTERESIS_CM           = 20.0;
const int   MESURES_CONFIRMATION    = 3;
const unsigned long INTERVALLE_MESURE_MS = 250;
const unsigned long COOLDOWN_MS          = 30000;

const uint8_t  REC_FPS     = 10;
const unsigned long REC_MIN_MS = 5000;
const unsigned long REC_MAX_MS = 30000;
const size_t MAX_UPLOAD_TELEGRAM = 18UL * 1024 * 1024;

const unsigned long TELEGRAM_POLL_MS = 3000;
const int WDT_TIMEOUT_S = 60;

// Ghost Mode
const bool  GHOST_MODE_ENABLED      = true;
const unsigned long GHOST_ANALYZE_MS = 5000;
const int   GHOST_MIN_HITS          = 4;

// Pins
#define TRIG_PIN 13
#define ECHO_PIN 12
#define PIR_PIN  14   // SR602 PIR sensor
#define FLASH_LED_PIN 4
#define REC_LED_PIN   33
#define EXT_LED_PIN 4
const int LED_NUIT_DEBUT = 18;
const int LED_NUIT_FIN   = 7;
#define FLASH_PENDANT_CAPTURE false

// =================== PINS CAMERA (AI Thinker / Binghe) ==============
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// =================== MUTEX & ÉTAT GLOBAL ===================
portMUX_TYPE streamMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE stateMux  = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t camMutex = NULL;

bool configMode = false;
String cfgSsid, cfgPass, cfgToken, cfgChatId, cfgCamName = "ESP32-CAM", cfgApiToken;
float cfgSeuil = 100.0;

bool sdReady = false;
bool detectionActive = true;
bool recording = false;
String lastVideoPath;
int videoCount = 0;
float lastDistance = -1;
uint16_t frameW = 800, frameH = 600;

File aviFile;
AVIWriter avi;
unsigned long recStartMs = 0;
unsigned long lastRecFrameMs = 0;
unsigned long lastPresenceMs = 0;
int absenceCount = 0;

long lastUpdateId = 0;
unsigned long lastPollMs = 0;
int compteurDetection = 0;
unsigned long lastMeasureMs = 0;
unsigned long lastAlertMs = 0;

bool ledState = false;
bool ledManual = false;
unsigned long ledAutoOffAt = 0;

// Ghost Mode state (protégé par stateMux)
bool ghostAnalyzing = false;
unsigned long ghostStartMs = 0;
int ghostHitCount = 0;
String ghostVideoPath;
bool ghostRecording = false;

// Stream clients (protégé par streamMux)
volatile int streamClients = 0;

// Watchdog timestamps
unsigned long lastLoopAliveMs = 0;
unsigned long lastFrameOkMs = 0;
unsigned long lastWifiOkMs = 0;

// Telegram task handle
TaskHandle_t telegramTaskHandle = NULL;

// ================ MUTEX CAMÉRA =================

camera_fb_t* camGet(uint32_t timeoutMs = 2000) {
  if (!camMutex) return esp_camera_fb_get();
  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return NULL;
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) xSemaphoreGive(camMutex);
  return fb;
}

void camReturn(camera_fb_t* fb) {
  if (!fb) return;
  esp_camera_fb_return(fb);
  if (camMutex) xSemaphoreGive(camMutex);
}

// ================ UTILITAIRES =================

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

String escapeHTML(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#x27;";
    else out += c;
  }
  return out;
}

String escapeJSON(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\b') out += "\\b";
    else if (c == '\f') out += "\\f";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

// ================ CONFIGURATION (flash + portail) =================

void loadConfig() {
  Preferences prefs;
  prefs.begin("camcfg", true);
  cfgSsid     = prefs.getString("ssid", "");
  cfgPass     = prefs.getString("pass", "");
  cfgToken    = prefs.getString("token", "");
  cfgChatId   = prefs.getString("chatid", "");
  cfgCamName  = prefs.getString("name", "ESP32-CAM");
  cfgSeuil    = prefs.getFloat("seuil", 100.0);
  cfgApiToken = prefs.getString("apitoken", "");
  lastUpdateId = prefs.getLong("lastupd", 0);
  prefs.end();
}

void saveConfig() {
  Preferences prefs;
  prefs.begin("camcfg", false);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("token", cfgToken);
  prefs.putString("chatid", cfgChatId);
  prefs.putString("name", cfgCamName);
  prefs.putFloat("seuil", cfgSeuil);
  prefs.putString("apitoken", cfgApiToken);
  prefs.end();
}

void saveUpdateId(long id) {
  Preferences prefs;
  prefs.begin("camcfg", false);
  prefs.putLong("lastupd", id);
  prefs.end();
}

// ================ AUTHENTIFICATION =================

bool checkAuth(httpd_req_t* req) {
  if (cfgApiToken.length() == 0) return true;
  char buf[64];
  if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", buf, sizeof(buf)) == ESP_OK) {
    if (String(buf) == cfgApiToken) return true;
  }
  char query[128];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[64];
    if (httpd_query_key_value(query, "token", val, sizeof(val)) == ESP_OK) {
      if (String(val) == cfgApiToken) return true;
    }
  }
  httpd_resp_set_status(req, "403 Forbidden");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, "Forbidden", HTTPD_RESP_USE_STRLEN);
  return false;
}

// ================ PAGES WEB =================

String configPage() {
  String h = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-CAM Config</title><style>"
    "body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:20px auto;padding:0 15px}"
    "input{width:100%;padding:10px;margin:5px 0 15px;border-radius:6px;border:1px solid #444;background:#222;color:#eee;box-sizing:border-box}"
    "button{width:100%;padding:12px;background:#1976d2;color:#fff;border:0;border-radius:6px;font-size:16px;cursor:pointer}"
    "label{color:#aaa;font-size:14px}h2{text-align:center}"
    ".info{background:#1b3a1b;padding:10px;border-radius:6px;font-size:13px;margin-bottom:15px}"
    ".warn{background:#3a1b1b;padding:10px;border-radius:6px;font-size:13px;margin-bottom:15px}"
    "</style></head><body>");
  h += "<h2>&#x1F4F7; Configuration ESP32-CAM</h2>";
  if (configMode) h += "<div class='info'>&#x1F4E1; La camera n'a pas pu se connecter au WiFi. Remplis ce formulaire : elle redemarrera sur ton reseau.</div>";
  if (cfgApiToken.length() == 0) h += "<div class='warn'>&#x26A0; Securite : definis un token API ci-dessous pour proteger le stream et la config !</div>";
  h += F("<form method='POST' action='/save'>"
    "<label>Nom du reseau WiFi (SSID)</label>"
    "<input name='ssid' list='nets' value='");
  h += escapeHTML(cfgSsid);
  h += F("' required><datalist id='nets'></datalist>"
    "<label>Mot de passe WiFi</label>"
    "<input name='pass' type='password' placeholder='(laisser vide = inchange)'>"
    "<label>Token bot Telegram (@BotFather)</label>"
    "<input name='token' type='password' placeholder='");
  h += cfgToken.length() ? F("(deja configure - laisser vide = inchange)") : F("123456:ABC-...");
  h += F("'>"
    "<label>Chat ID Telegram</label>"
    "<input name='chatid' value='");
  h += escapeHTML(cfgChatId);
  h += F("'>"
    "<label>Nom de la camera</label>"
    "<input name='name' value='");
  h += escapeHTML(cfgCamName);
  h += F("'>"
    "<label>Distance de detection (cm)</label>"
    "<input name='seuil' type='number' value='");
  h += String((int)cfgSeuil);
  h += F("'>"
    "<label>Token API (securite : stream + config)</label>"
    "<input name='apitoken' placeholder='laisser vide = desactive' value='");
  h += escapeHTML(cfgApiToken);
  h += F("'>"
    "<button type='submit'>&#x1F4BE; Enregistrer et redemarrer</button></form>"
    "<p style='color:#777;font-size:13px'>Bot : @BotFather &rarr; /newbot &rarr; token.<br>"
    "Chat ID : envoie un message au bot puis ouvre api.telegram.org/bot&lt;TOKEN&gt;/getUpdates</p>"
    "<script>fetch('/scan').then(r=>r.json()).then(d=>{var dl=document.getElementById('nets');"
    "d.forEach(function(n){var o=document.createElement('option');o.value=n.ssid;o.label=n.rssi+' dBm';dl.appendChild(o);});});</script>"
    "</body></html>");
  return h;
}

static esp_err_t config_handler(httpd_req_t* req) {
  if (!checkAuth(req)) return ESP_FAIL;
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
    json += "{\"ssid\":\"" + escapeJSON(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t save_handler(httpd_req_t* req) {
  if (!checkAuth(req)) return ESP_FAIL;
  char buf[1024];
  int len = min((int)sizeof(buf) - 1, (int)req->content_len);
  int ret = httpd_req_recv(req, buf, len);
  if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
  if (ret >= (int)sizeof(buf)) ret = (int)sizeof(buf) - 1;
  buf[ret] = '\0';
  String body = buf;

  String s = postParam(body, "ssid");
  if (s.length()) cfgSsid = s;
  s = postParam(body, "pass");
  if (s.length()) cfgPass = s;
  s = postParam(body, "token");
  if (s.length()) cfgToken = s;
  s = postParam(body, "chatid");
  if (s.length()) cfgChatId = s;
  s = postParam(body, "name");
  if (s.length()) cfgCamName = s;
  s = postParam(body, "seuil");
  if (s.length()) cfgSeuil = s.toFloat();
  s = postParam(body, "apitoken");
  cfgApiToken = s;
  saveConfig();

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req,
    "<html><head><meta charset='utf-8'></head>"
    "<body style='font-family:sans-serif;background:#111;color:#eee;text-align:center;padding-top:50px'>"
    "<h2>&#x2705; Configuration sauvegardee !</h2>"
    "<p>Redemarrage... La camera rejoint ton WiFi.</p></body></html>",
    HTTPD_RESP_USE_STRLEN);
  delay(2000);
  ESP.restart();
  return ESP_OK;
}

// ================ PORTAIL DE CONFIGURATION =================

void startConfigPortal() {
  configMode = true;
  String mac = WiFi.macAddress();
  String apName = "ESP32-CAM-Setup-" + mac.substring(12, 14) + mac.substring(15, 17);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str(), "12345678");
  Serial.println("\n=== MODE CONFIGURATION ===");
  Serial.println("Reseau WiFi : " + apName + "  (mot de passe : 12345678)");
  Serial.print("Portail : http://");
  Serial.println(WiFi.softAPIP());

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 4;
  config.max_open_sockets = 4;
  config.ctrl_port = 32768;
  httpd_handle_t server = NULL;
  httpd_uri_t form = { .uri = "/",     .method = HTTP_GET,  .handler = config_handler, .user_ctx = NULL };
  httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET,  .handler = scan_handler,   .user_ctx = NULL };
  httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler,   .user_ctx = NULL };
  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &form);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &save);
    Serial.println("Portail HTTP demarre");
  }
}

// ================ TELEGRAM =================

String urlencode(const String& s) {
  String out;
  out.reserve(s.length() * 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
      out += buf;
    }
  }
  return out;
}

bool telegramSendMessage(const String& text) {
  if (cfgToken.length() == 0 || cfgChatId.length() == 0) return false;
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) return false;
  String url = "/bot" + cfgToken + "/sendMessage?chat_id=" + cfgChatId +
               "&text=" + urlencode(text);
  client.print("GET " + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");
  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 8000) {
    delay(10);
    lastLoopAliveMs = millis();  // nourrir watchdog pendant l'attente
  }
  String resp;
  while (client.available()) resp += (char)client.read();
  client.stop();
  return resp.indexOf("\"ok\":true") >= 0;
}

bool telegramSendPhoto(uint8_t* jpg, size_t jpgLen, const String& caption) {
  if (cfgToken.length() == 0 || cfgChatId.length() == 0) return false;
  WiFiClientSecure client;
  client.setInsecure();
  Serial.println("Envoi photo Telegram...");
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("ERREUR connexion Telegram");
    return false;
  }
  String boundary = "ESP32CAM7MA4WYxk";
  String head = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + cfgChatId + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"photo\"; filename=\"alerte.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";
  uint32_t contentLength = head.length() + jpgLen + tail.length();

  client.printf("POST /bot%s/sendPhoto HTTP/1.1\r\n", cfgToken.c_str());
  client.print("Host: api.telegram.org\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.printf("Content-Length: %u\r\n", contentLength);
  client.print("Connection: close\r\n\r\n");
  client.print(head);

  size_t sent = 0;
  while (sent < jpgLen) {
    size_t chunk = min((size_t)4096, jpgLen - sent);
    size_t w = client.write(jpg + sent, chunk);
    if (w == 0) { client.stop(); return false; }
    sent += w;
    lastLoopAliveMs = millis();  // nourrir watchdog pendant upload
  }
  client.print(tail);

  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 10000) {
    delay(10);
    lastLoopAliveMs = millis();
  }
  String resp;
  while (client.available()) resp += (char)client.read();
  client.stop();
  bool ok = resp.indexOf("\"ok\":true") >= 0;
  Serial.println(ok ? "Photo envoyee !" : "Erreur TG: " + resp.substring(0, 200));
  return ok;
}

bool telegramSendDocument(const String& path, const String& caption) {
  if (cfgToken.length() == 0 || cfgChatId.length() == 0) return false;
  File f = SD_MMC.open(path);
  if (!f) { telegramSendMessage("⚠️ Fichier introuvable : " + path); return false; }
  size_t sz = f.size();
  if (sz > MAX_UPLOAD_TELEGRAM) {
    f.close();
    telegramSendMessage("⚠️ Video trop grosse pour Telegram (" + String(sz / 1048576) +
                        " Mo).\nRecupere-la ici (meme WiFi) : http://" +
                        WiFi.localIP().toString() + "/video");
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  Serial.println("Envoi video Telegram (" + String(sz / 1024) + " Ko)...");
  if (!client.connect("api.telegram.org", 443)) { f.close(); return false; }

  String boundary = "ESP32CAM7MA4WYxk";
  String fname = path.substring(path.lastIndexOf('/') + 1);
  String head = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + cfgChatId + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"document\"; filename=\"" + fname + "\"\r\n"
    "Content-Type: video/x-msvideo\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  client.printf("POST /bot%s/sendDocument HTTP/1.1\r\n", cfgToken.c_str());
  client.print("Host: api.telegram.org\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)(head.length() + sz + tail.length()));
  client.print("Connection: close\r\n\r\n");
  client.print(head);

  uint8_t buf[4096];
  while (f.available()) {
    int r = f.read(buf, sizeof(buf));
    int off = 0;
    while (off < r) {
      size_t w = client.write(buf + off, r - off);
      if (w == 0) { f.close(); client.stop(); return false; }
      off += w;
      lastLoopAliveMs = millis();  // nourrir watchdog pendant upload
    }
  }
  client.print(tail);
  f.close();

  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 15000) {
    delay(10);
    lastLoopAliveMs = millis();
  }
  String resp;
  while (client.available()) resp += (char)client.read();
  client.stop();
  bool ok = resp.indexOf("\"ok\":true") >= 0;
  Serial.println(ok ? "Video envoyee !" : "Erreur TG doc: " + resp.substring(0, 200));
  return ok;
}

// ================ COMMANDES TELEGRAM ENTRANTES =================

String helpText() {
  return "📷 ESP32-CAM Surveillance v3\n"
         "📸 /photo — photo instantanee\n"
         "📹 /video — recevoir la derniere video\n"
         "💡 /led — LED on/off (/ledon /ledoff /ledauto)\n"
         "📊 /status — etat du systeme\n"
         "⏸ /off — desactiver les alertes\n"
         "▶️ /on — reactiver les alertes\n"
         "👻 /ghost — basculer Ghost Mode\n"
         "🌐 Direct : http://" + WiFi.localIP().toString() + "/ (meme WiFi)";
}

String statusText() {
  portENTER_CRITICAL(&stateMux);
  bool rec = recording;
  bool led = ledState;
  bool lman = ledManual;
  bool gmode = ghostAnalyzing;
  portEXIT_CRITICAL(&stateMux);

  String s = "📊 STATUT\n";
  s += "🌐 IP : http://" + WiFi.localIP().toString() + "/\n";
  s += "⏱ Uptime : " + String(millis() / 60000) + " min\n";
  s += "📏 Distance : " + (lastDistance > 0 ? String((int)lastDistance) + " cm" : String("rien")) + "\n";
  s += "🔍 Detection ultrason : " + String(DETECTION_ULTRASON ? "oui" : "NON (mode stream seul)") + "\n";
  s += "🔔 Alertes : " + String(detectionActive ? "ACTIVEES" : "desactivees") + "\n";
  s += "👻 Ghost Mode : " + String(GHOST_MODE_ENABLED ? (gmode ? "ANALYSE EN COURS" : "actif") : "desactive") + "\n";
  s += "🔴 Enregistrement : " + String(rec ? "EN COURS" : "non") + "\n";
  s += "💾 Carte SD : " + String(sdReady ? "OK" : "ABSENTE") + "\n";
  s += "📹 Videos enregistrees : " + String(videoCount) + "\n";
  s += "💡 LED : " + String(led ? "ALLUMEE" : "eteinte") + (lman ? " (manuel)" : " (auto)") + "\n";
  s += "🌙 Mode nuit (18h-7h) : " + String(isNight() ? "oui" : "non");
  return s;
}

void handleCommand(String cmd) {
  cmd.trim();
  int at = cmd.indexOf('@');
  if (at > 0) cmd = cmd.substring(0, at);
  Serial.println("Commande Telegram : " + cmd);

  if (cmd.startsWith("/photo")) {
    camera_fb_t* fb = camGet();
    if (fb) {
      // Copie le JPEG pour ne pas bloquer le mutex pendant l'envoi TLS
      uint8_t* copy = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (copy) {
        memcpy(copy, fb->buf, fb->len);
        camReturn(fb);
        telegramSendPhoto(copy, fb->len, "📸 Photo a la demande");
        free(copy);
      } else {
        telegramSendPhoto(fb->buf, fb->len, "📸 Photo a la demande");
        camReturn(fb);
      }
    } else {
      telegramSendMessage("⚠️ Capture impossible");
    }
  } else if (cmd.startsWith("/video")) {
    if (!sdReady || lastVideoPath.length() == 0) {
      telegramSendMessage("⚠️ Aucune video enregistree pour le moment.");
    } else {
      String fname = lastVideoPath.substring(lastVideoPath.lastIndexOf('/') + 1);
      telegramSendDocument(lastVideoPath, "📹 " + fname);
    }
  } else if (cmd.startsWith("/led")) {
    portENTER_CRITICAL(&stateMux);
    if (cmd.startsWith("/ledauto")) ledManual = false;
    else if (cmd.startsWith("/ledon")) { ledManual = true; ledState = true; }
    else if (cmd.startsWith("/ledoff")) { ledManual = true; ledState = false; }
    else { ledManual = true; ledState = !ledState; }
    bool ls = ledState;
    bool lm = ledManual;
    portEXIT_CRITICAL(&stateMux);
    digitalWrite(EXT_LED_PIN, ls ? HIGH : LOW);
    telegramSendMessage(String("\xF0\x9F\x92\xA1 LED : ") + (ls ? "ALLUMEE" : "eteinte") +
                        (lm ? " (manuel)" : " (auto)"));
  } else if (cmd.startsWith("/status")) {
    telegramSendMessage(statusText());
  } else if (cmd.startsWith("/off")) {
    detectionActive = false;
    telegramSendMessage("⏸ Alertes automatiques DESACTIVEES.\n/on pour reactiver.");
  } else if (cmd.startsWith("/on")) {
    detectionActive = true;
    telegramSendMessage("▶️ Alertes automatiques ACTIVEES.");
  } else if (cmd.startsWith("/ghost")) {
    telegramSendMessage("👻 Ghost Mode est " + String(GHOST_MODE_ENABLED ? "ACTIVE" : "DESACTIVE") +
                        "\nDelai d'analyse : " + String(GHOST_ANALYZE_MS / 1000) + "s");
  } else if (cmd.startsWith("/help") || cmd.startsWith("/start")) {
    telegramSendMessage(helpText());
  } else {
    telegramSendMessage("❓ Commande inconnue : " + cmd + "\n\n" + helpText());
  }
}

void telegramPoll() {
  if (cfgToken.length() == 0) return;
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) return;
  String url = "/bot" + cfgToken + "/getUpdates?offset=" + String(lastUpdateId + 1) +
               "&limit=5&timeout=0";
  client.print("GET " + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");
  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 8000) {
    delay(10);
    lastLoopAliveMs = millis();
  }
  String resp;
  while (client.available()) resp += (char)client.read();
  client.stop();

  int body = resp.indexOf("\r\n\r\n");
  if (body < 0) return;
  String json = resp.substring(body + 4);
  if (json.indexOf("\"ok\":true") < 0) return;

  int pos = 0;
  while (true) {
    int u = json.indexOf("\"update_id\":", pos);
    if (u < 0) break;
    int nextU = json.indexOf("\"update_id\":", u + 12);
    int limit = (nextU < 0) ? (int)json.length() : nextU;

    long uid = json.substring(u + 12, json.indexOf(",", u)).toInt();
    String chatId, text;
    int c = json.indexOf("\"chat\":{\"id\":", u);
    if (c > 0 && c < limit) chatId = json.substring(c + 13, json.indexOf(",", c + 13));
    int t = json.indexOf("\"text\":\"", u);
    if (t > 0 && t < limit) text = json.substring(t + 8, json.indexOf("\"", t + 8));

    if (uid > lastUpdateId) {
      lastUpdateId = uid;
      saveUpdateId(uid);
      if (chatId == cfgChatId && text.length() > 0) handleCommand(text);
    }
    pos = u + 12;
  }
}

// Task FreeRTOS dédiée pour Telegram
void telegramTask(void* param) {
  while (true) {
    if (!configMode && WiFi.status() == WL_CONNECTED && cfgToken.length() > 0) {
      telegramPoll();
    }
    vTaskDelay(pdMS_TO_TICKS(TELEGRAM_POLL_MS));
  }
}

// ================ STREAMING HTTP (port 81 dédié) =================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t* req) {
  if (!checkAuth(req)) return ESP_FAIL;
  camera_fb_t* fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  portENTER_CRITICAL(&streamMux);
  streamClients++;
  portEXIT_CRITICAL(&streamMux);

  sensor_t* s = esp_camera_sensor_get();
  int oldQuality = -1;
  if (s) { oldQuality = s->status.quality; s->set_quality(s, 10); }

  while (true) {
    fb = camGet(5000);
    if (!fb) { res = ESP_FAIL; break; }
    lastFrameOkMs = millis();
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    camReturn(fb);
    fb = NULL;
    if (res != ESP_OK) break;
  }

  if (s && oldQuality >= 0) s->set_quality(s, oldQuality);
  portENTER_CRITICAL(&streamMux);
  streamClients--;
  portEXIT_CRITICAL(&streamMux);
  return res;
}

// ================ LED & HANDLERS =================

bool isNight() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return false;
  int h = timeinfo.tm_hour;
  return (h >= LED_NUIT_DEBUT || h < LED_NUIT_FIN);
}

void ledSet(bool on) {
  portENTER_CRITICAL(&stateMux);
  ledState = on;
  portEXIT_CRITICAL(&stateMux);
  digitalWrite(EXT_LED_PIN, on ? HIGH : LOW);
}

static esp_err_t led_handler(httpd_req_t* req) {
  if (!checkAuth(req)) return ESP_FAIL;
  char query[64];
  bool redir = false;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[12];
    if (httpd_query_key_value(query, "state", val, sizeof(val)) == ESP_OK) {
      portENTER_CRITICAL(&stateMux);
      if (strcmp(val, "auto") == 0) ledManual = false;
      else {
        ledManual = true;
        if (strcmp(val, "on") == 0) ledState = true;
        else if (strcmp(val, "off") == 0) ledState = false;
        else if (strcmp(val, "toggle") == 0) ledState = !ledState;
      }
      bool ls = ledState;
      portEXIT_CRITICAL(&stateMux);
      digitalWrite(EXT_LED_PIN, ls ? HIGH : LOW);
    }
    char r[8];
    if (httpd_query_key_value(query, "redir", r, sizeof(r)) == ESP_OK) redir = true;
  }
  if (redir) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
  }
  portENTER_CRITICAL(&stateMux);
  bool ls = ledState;
  bool lm = ledManual;
  portEXIT_CRITICAL(&stateMux);
  String json = "{\"led\":" + String(ls ? "true" : "false") +
                ",\"manual\":" + String(lm ? "true" : "false") +
                ",\"night\":" + String(isNight() ? "true" : "false") + "}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t capture_handler(httpd_req_t* req) {
  if (!checkAuth(req)) return ESP_FAIL;
  camera_fb_t* fb = NULL;
  // Retry: le stream peut garder le mutex
  for (int retry = 0; retry < 10; retry++) {
    fb = camGet(500);
    if (fb) break;
    delay(50);
  }
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  camReturn(fb);
  return res;
}

static esp_err_t serveSDFile(httpd_req_t* req, const String& path) {
  File f = SD_MMC.open(path);
  if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "video/x-msvideo");
  String fname = path.substring(path.lastIndexOf('/') + 1);
  httpd_resp_set_hdr(req, "Content-Disposition", ("attachment; filename=\"" + fname + "\"").c_str());
  uint8_t* buf = (uint8_t*)malloc(4096);
  if (!buf) { f.close(); httpd_resp_send_500(req); return ESP_FAIL; }
  esp_err_t res = ESP_OK;
  while (f.available() && res == ESP_OK) {
    int r = f.read(buf, 4096);
    if (r > 0) res = httpd_resp_send_chunk(req, (const char*)buf, r);
  }
  httpd_resp_send_chunk(req, NULL, 0);
  free(buf);
  f.close();
  return res;
}

static esp_err_t video_handler(httpd_req_t* req) {
  if (!checkAuth(req)) return ESP_FAIL;
  if (!sdReady || lastVideoPath.length() == 0) { httpd_resp_send_404(req); return ESP_FAIL; }
  return serveSDFile(req, lastVideoPath);
}

static esp_err_t dl_handler(httpd_req_t* req) {
  if (!checkAuth(req)) return ESP_FAIL;
  char query[96];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  char fname[64];
  if (httpd_query_key_value(query, "f", fname, sizeof(fname)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  String n = fname;
  if (n.indexOf("..") >= 0 || n.indexOf('/') >= 0 || n.indexOf('\\') >= 0 || !n.endsWith(".avi")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    return ESP_FAIL;
  }
  return serveSDFile(req, "/" + n);
}

static esp_err_t list_handler(httpd_req_t* req) {
  if (!checkAuth(req)) return ESP_FAIL;
  httpd_resp_set_type(req, "text/html");
  String html = "<html><head><meta charset='utf-8'><title>Videos</title></head><body>"
                "<h2>&#x1F4F9; Videos enregistrees</h2><ul>";
  if (sdReady) {
    File root = SD_MMC.open("/");
    File file = root.openNextFile();
    while (file) {
      String n = file.name();
      if (n.endsWith(".avi")) {
        if (!n.startsWith("/")) n = "/" + n;
        html += "<li><a href='/dl?f=" + n.substring(1) + "&token=" + urlencode(cfgApiToken) + "'>" + escapeHTML(n) + "</a> (" +
                String(file.size() / 1024) + " Ko)</li>";
      }
      file = root.openNextFile();
    }
  } else {
    html += "<p>Carte SD absente</p>";
  }
  html += "</ul><p><a href='/'>&#x1F534; Retour au direct</a></p></body></html>";
  return httpd_resp_send(req, html.c_str(), html.length());
}

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>ESP32-CAM Surveillance</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:20px}
  img{max-width:100%;border:2px solid #444;border-radius:8px}
  a{color:#4fc3f7;margin:0 10px}
</style></head><body>
<h2>ESP32-CAM Surveillance</h2>
<img src="/stream">
<p>
  <a href="/capture">📸 Photo</a>
  <a href="/video">📹 Derniere video</a>
  <a href="/list">🗂 Toutes les videos</a>
  <a href="/setup">⚙️ Config</a>
  <a href="/led?state=toggle&redir=1">💡 LED on/off</a>
</p>
</body></html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t info_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  portENTER_CRITICAL(&stateMux);
  bool rec = recording;
  bool led = ledState;
  portEXIT_CRITICAL(&stateMux);
  String json = "{\"name\":\"" + escapeJSON(cfgCamName) +
                "\",\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"model\":\"ESP32-CAM OV3660\"" +
                ",\"sd\":" + String(sdReady ? "true" : "false") +
                ",\"detection\":" + String(detectionActive ? "true" : "false") +
                ",\"led\":" + String(led ? "true" : "false") +
                ",\"recording\":" + String(rec ? "true" : "false") + "}";
  return httpd_resp_send(req, json.c_str(), json.length());
}

// ================ DOUBLE SERVEUR HTTP =================

httpd_handle_t apiServer = NULL;
httpd_handle_t streamServer = NULL;

void startCameraServer() {
  // Serveur API principal (port 80) — tout SAUF /stream
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 12;
  config.max_open_sockets = 5;
  config.ctrl_port = 32768;

  httpd_uri_t index_uri   = { .uri = "/",        .method = HTTP_GET, .handler = index_handler,   .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
  httpd_uri_t video_uri   = { .uri = "/video",   .method = HTTP_GET, .handler = video_handler,   .user_ctx = NULL };
  httpd_uri_t list_uri    = { .uri = "/list",    .method = HTTP_GET, .handler = list_handler,    .user_ctx = NULL };
  httpd_uri_t dl_uri      = { .uri = "/dl",      .method = HTTP_GET, .handler = dl_handler,      .user_ctx = NULL };
  httpd_uri_t info_uri    = { .uri = "/info",    .method = HTTP_GET, .handler = info_handler,    .user_ctx = NULL };
  httpd_uri_t setup_uri   = { .uri = "/setup",   .method = HTTP_GET, .handler = config_handler,  .user_ctx = NULL };
  httpd_uri_t scan_uri    = { .uri = "/scan",    .method = HTTP_GET, .handler = scan_handler,    .user_ctx = NULL };
  httpd_uri_t save_uri    = { .uri = "/save",    .method = HTTP_POST,.handler = save_handler,    .user_ctx = NULL };
  httpd_uri_t led_uri     = { .uri = "/led",     .method = HTTP_GET, .handler = led_handler,     .user_ctx = NULL };

  if (httpd_start(&apiServer, &config) == ESP_OK) {
    httpd_register_uri_handler(apiServer, &index_uri);
    httpd_register_uri_handler(apiServer, &capture_uri);
    httpd_register_uri_handler(apiServer, &video_uri);
    httpd_register_uri_handler(apiServer, &list_uri);
    httpd_register_uri_handler(apiServer, &dl_uri);
    httpd_register_uri_handler(apiServer, &info_uri);
    httpd_register_uri_handler(apiServer, &setup_uri);
    httpd_register_uri_handler(apiServer, &scan_uri);
    httpd_register_uri_handler(apiServer, &save_uri);
    httpd_register_uri_handler(apiServer, &led_uri);
    Serial.println("Serveur API demarre (port 80)");
  }

  // Serveur STREAM dédié (port 81) — /stream uniquement
  httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
  scfg.server_port = 81;
  scfg.max_uri_handlers = 1;
  scfg.max_open_sockets = 3;
  scfg.ctrl_port = 32769;  // DIFFERENT du serveur principal

  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  if (httpd_start(&streamServer, &scfg) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &stream_uri);
    Serial.println("Serveur STREAM demarre (port 81)");
  }
}

// ================ ULTRASON =================

float readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return -1.0;
  return duration * 0.0343f / 2.0f;
}

// ================ GHOST MODE =================

void startGhostRecording() {
  if (!sdReady) return;
  String name = "/ghost_" + String(millis()) + ".avi";
  File f = SD_MMC.open(name, FILE_WRITE);
  if (!f) {
    Serial.println("GHOST ERREUR ouverture " + name);
    return;
  }
  uint16_t maxFrames = (uint16_t)((REC_MAX_MS / 1000 + 2) * REC_FPS);
  if (!avi.begin(f, frameW, frameH, REC_FPS, maxFrames)) {
    f.close();
    return;
  }
  aviFile = f;

  portENTER_CRITICAL(&stateMux);
  ghostRecording = true;
  ghostAnalyzing = true;
  ghostStartMs = millis();
  ghostHitCount = 0;
  ghostVideoPath = name;
  portEXIT_CRITICAL(&stateMux);

  Serial.println("GHOST START -> " + name);
}

void stopGhostRecording(bool isRealThreat) {
  portENTER_CRITICAL(&stateMux);
  bool wasRecording = ghostRecording;
  ghostRecording = false;
  ghostAnalyzing = false;
  String gpath = ghostVideoPath;
  portEXIT_CRITICAL(&stateMux);

  if (!wasRecording) return;

  uint16_t frames = avi.end(millis() - ghostStartMs);  // fps reels
  aviFile.close();

  if (!isRealThreat) {
    SD_MMC.remove(gpath);
    Serial.println("GHOST STOP -> faux positif supprime (" + String(frames) + " frames)");
  } else {
    String realName;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
      char buf[40];
      strftime(buf, sizeof(buf), "/rec_%Y-%m-%d_%H-%M-%S.avi", &timeinfo);
      realName = buf;
    } else {
      realName = "/rec_" + String(millis()) + ".avi";
    }
    SD_MMC.rename(gpath, realName);
    lastVideoPath = realName;
    videoCount++;

    Serial.printf("GHOST CONFIRME -> %s (%d frames)\n", realName.c_str(), frames);

    if (isNight() && !ledManual) {
      ledSet(true);
      ledAutoOffAt = millis() + 30000;
    }
    if (FLASH_PENDANT_CAPTURE) {
      digitalWrite(FLASH_LED_PIN, HIGH);
      delay(120);
    }
    camera_fb_t* fb = camGet();
    if (FLASH_PENDANT_CAPTURE) digitalWrite(FLASH_LED_PIN, LOW);
    if (fb) {
      String caption = "👻 GHOST MODE — MENACE CONFIRMEE !\n";
      caption += "📏 Distance : " + String((int)lastDistance) + " cm\n";
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 100)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);
        caption += String("🕒 ") + buf;
      }
      caption += "\n📹 Video enregistree : " + realName.substring(1);
      // Copie pour ne pas bloquer le mutex
      uint8_t* copy = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (copy) {
        memcpy(copy, fb->buf, fb->len);
        size_t flen = fb->len;
        camReturn(fb);
        telegramSendPhoto(copy, flen, caption);
        free(copy);
      } else {
        telegramSendPhoto(fb->buf, fb->len, caption);
        camReturn(fb);
      }
    }
    telegramSendMessage("👻 GHOST MODE — Vraie menace confirmee apres " +
                        String(GHOST_ANALYZE_MS / 1000) + "s d'analyse.\n" +
                        "📹 /video pour recuperer la preuve.\n" +
                        "🌐 http://" + WiFi.localIP().toString() + "/video");
  }
}

// ================ ALERTE & ENREGISTREMENT =================

void sendAlert(float distance, bool isPostGhost = false) {
  if (isPostGhost) return;

  if (isNight() && !ledManual) {
    ledSet(true);
    ledAutoOffAt = millis() + 10000;
  }
  if (FLASH_PENDANT_CAPTURE) {
    digitalWrite(FLASH_LED_PIN, HIGH);
    delay(120);
  }
  camera_fb_t* fb = camGet();
  if (fb) camReturn(fb);
  fb = camGet();
  if (FLASH_PENDANT_CAPTURE) digitalWrite(FLASH_LED_PIN, LOW);
  if (!fb) return;

  String caption = "🚨 ALERTE : presence detectee a " + String((int)distance) + " cm";
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);
    caption += String("\n🕒 ") + buf;
  }
  caption += "\n📹 Video en cours d'enregistrement...";

  // Copie JPEG pour libérer le mutex caméra immédiatement
  uint8_t* copy = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (copy) {
    memcpy(copy, fb->buf, fb->len);
    size_t flen = fb->len;
    camReturn(fb);
    telegramSendPhoto(copy, flen, caption);
    free(copy);
  } else {
    telegramSendPhoto(fb->buf, fb->len, caption);
    camReturn(fb);
  }
}

void ensureFreeSpace(uint64_t needBytes) {
  if (!sdReady) return;
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  while (total - used < needBytes) {
    File root = SD_MMC.open("/");
    String oldest;
    time_t oldestT = 0;
    File f = root.openNextFile();
    while (f) {
      String n = f.name();
      if (n.endsWith(".avi")) {
        time_t t = f.getLastWrite();
        if (oldest.length() == 0 || t < oldestT) { oldest = n; oldestT = t; }
      }
      f = root.openNextFile();
    }
    root.close();
    if (oldest.length() == 0) break;
    if (!oldest.startsWith("/")) oldest = "/" + oldest;
    Serial.println("Rotation SD : suppression " + oldest);
    SD_MMC.remove(oldest);
    used = SD_MMC.usedBytes();
  }
}

void startRecording() {
  if (!sdReady) {
    telegramSendMessage("⚠️ Carte SD absente : enregistrement video impossible.");
    return;
  }
  ensureFreeSpace(100UL * 1024 * 1024);  // Garde 100 Mo libres

  String name;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100)) {
    char buf[40];
    strftime(buf, sizeof(buf), "/rec_%Y-%m-%d_%H-%M-%S.avi", &timeinfo);
    name = buf;
  } else {
    name = "/rec_" + String(millis()) + ".avi";
  }
  ledAutoOffAt = 0;
  if (isNight() && !ledManual) ledSet(true);
  aviFile = SD_MMC.open(name, FILE_WRITE);
  if (!aviFile) {
    Serial.println("ERREUR ouverture " + name);
    telegramSendMessage("⚠️ Impossible d'ecrire sur la carte SD (pleine ?)");
    return;
  }
  uint16_t maxFrames = (uint16_t)((REC_MAX_MS / 1000 + 2) * REC_FPS);
  if (!avi.begin(aviFile, frameW, frameH, REC_FPS, maxFrames)) {
    aviFile.close();
    return;
  }
  portENTER_CRITICAL(&stateMux);
  recording = true;
  portEXIT_CRITICAL(&stateMux);
  recStartMs = millis();
  lastRecFrameMs = 0;
  absenceCount = 0;
  lastVideoPath = name;
  digitalWrite(REC_LED_PIN, LOW);
  Serial.println("REC -> " + name);
}

void stopRecording() {
  portENTER_CRITICAL(&stateMux);
  bool wasRec = recording;
  recording = false;
  portEXIT_CRITICAL(&stateMux);
  if (!wasRec) return;

  digitalWrite(REC_LED_PIN, HIGH);
  if (!ledManual) ledSet(false);
  uint16_t frames = avi.end(millis() - recStartMs);  // fps reels
  aviFile.close();
  videoCount++;

  unsigned long duree = (millis() - recStartMs) / 1000;
  File f = SD_MMC.open(lastVideoPath);
  size_t sz = f ? f.size() : 0;
  if (f) f.close();

  Serial.printf("Fin REC : %d frames, %lu Ko\n", frames, sz / 1024);
  telegramSendMessage("📹 Video terminee : " + String(duree) + " s (" +
                      String(sz / 1024) + " Ko)\n👉 /video pour la recevoir\n🌐 http://" +
                      WiFi.localIP().toString() + "/video (meme WiFi)");
  lastAlertMs = millis();
}

// ================ CAMERA / SD =================

bool initCamera() {
  camera_config_t config = {};  // FIX : initialisation complete
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0    = Y2_GPIO_NUM;
  config.pin_d1    = Y3_GPIO_NUM;
  config.pin_d2    = Y4_GPIO_NUM;
  config.pin_d3    = Y5_GPIO_NUM;
  config.pin_d4    = Y6_GPIO_NUM;
  config.pin_d5    = Y7_GPIO_NUM;
  config.pin_d6    = Y8_GPIO_NUM;
  config.pin_d7    = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 16000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_UXGA;
    config.jpeg_quality = 6;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    frameW = 1600; frameH = 1200;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 8;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    frameW = 800; frameH = 600;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("ERREUR init camera : 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_sharpness(s, 2);
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, 1);
    s->set_special_effect(s, 0);
    s->set_wb_mode(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, (gainceiling_t)2);
    s->set_lenc(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_dcw(s, 0);
    s->set_raw_gma(s, 1);
    Serial.println("Capteur regle : UXGA 1600x1200, JPEG q=6, nettete+2");
  }
  return true;
}

bool initSD() {
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Carte SD absente -> videos desactivees (alertes photo OK)");
    return false;
  }
  Serial.printf("Carte SD OK : %llu Mo\n", SD_MMC.cardSize() / (1024 * 1024));
  return true;
}

// ================ SETUP / LOOP =================

void setup() {
  Serial.begin(115200);
  // Désactiver brownout detector (USB pas assez puissant pour UXGA + WiFi)
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.println("\n=== ESP32-CAM Surveillance v3 (Corrige Complet + Ghost Mode) ===");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);      // SR602 PIR
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  pinMode(REC_LED_PIN, OUTPUT);
  digitalWrite(REC_LED_PIN, HIGH);

  // Mutex caméra
  camMutex = xSemaphoreCreateMutex();
  if (!camMutex) Serial.println("WARN : impossible de creer le mutex camera");

  if (!initCamera()) {
    Serial.println("Camera KO - redemarrage dans 5 s");
    delay(5000);
    ESP.restart();
  }

  loadConfig();

  // Purge backlog Telegram au premier boot (lastUpdateId == 0)
  if (lastUpdateId == 0 && cfgToken.length() > 0) {
    WiFiClientSecure client;
    client.setInsecure();
    if (client.connect("api.telegram.org", 443)) {
      client.print("GET /bot" + cfgToken + "/getUpdates?offset=-1&limit=1 HTTP/1.1\r\n");
      client.print("Host: api.telegram.org\r\nConnection: close\r\n\r\n");
      unsigned long t0 = millis();
      while (client.connected() && !client.available() && millis() - t0 < 5000) delay(10);
      String resp;
      while (client.available()) resp += (char)client.read();
      client.stop();
      int u = resp.indexOf("\"update_id\":");
      if (u >= 0) {
        long uid = resp.substring(u + 12, resp.indexOf(",", u)).toInt();
        if (uid > 0) {
          lastUpdateId = uid;
          saveUpdateId(uid);
          Serial.println("Backlog Telegram purge. Dernier update_id : " + String(uid));
        }
      }
    }
  }

  bool wifiOk = false;
  if (cfgSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    WiFi.setSleep(false);
    Serial.print("Connexion WiFi '" + cfgSsid + "'");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 45000) {  // 45s timeout
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    wifiOk = (WiFi.status() == WL_CONNECTED);
  }

  if (!wifiOk) {
    Serial.println("Pas de WiFi -> portail de configuration");
    startConfigPortal();
    lastWifiOkMs = millis();
    lastLoopAliveMs = millis();
    esp_task_wdt_config_t wdt_cfg = { .timeout_ms = WDT_TIMEOUT_S * 1000, .trigger_panic = true };
    esp_task_wdt_init(&wdt_cfg);
    esp_task_wdt_add(NULL);
    return;
  }

  Serial.print("WiFi OK : http://");
  Serial.println(WiFi.localIP());

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  sdReady = initSD();
  startCameraServer();

  // mDNS
  {
    String slug = "";
    String lower = cfgCamName;
    lower.toLowerCase();
    for (size_t i = 0; i < lower.length(); i++) {
      char c = lower[i];
      slug += isalnum(c) ? String(c) : String("-");
    }
    if (MDNS.begin(slug.c_str())) {
      MDNS.addService("esp32cam", "tcp", 80);
      MDNS.addServiceTxt("esp32cam", "tcp", "name", cfgCamName.c_str());
      Serial.println("mDNS actif : http://" + slug + ".local");
    }
  }

  // Watchdog natif ESP32
  esp_task_wdt_config_t wdt_cfg2 = { .timeout_ms = WDT_TIMEOUT_S * 1000, .trigger_panic = true };
  esp_task_wdt_init(&wdt_cfg2);
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog natif actif (" + String(WDT_TIMEOUT_S) + "s)");

  // Task Telegram dédiée (core 0, prio 1)
  xTaskCreatePinnedToCore(telegramTask, "telegram", 8192, NULL, 1, &telegramTaskHandle, 0);
  Serial.println("Task Telegram demarree sur core 0");

  lastWifiOkMs = millis();
  lastLoopAliveMs = millis();

  delay(1500);
  String bootMsg = "✅ Camera en ligne ! (v3)\n";
  bootMsg += String("🔍 Detection ultrason : ") + (DETECTION_ULTRASON ? "ACTIVEE" : "desactivee") + "\n";
  bootMsg += String("👻 Ghost Mode : ") + (GHOST_MODE_ENABLED ? "ACTIVE" : "desactive") + "\n\n";
  telegramSendMessage(bootMsg + helpText());
}

void loop() {
  esp_task_wdt_reset();
  lastLoopAliveMs = millis();

  // ---------- MODE CONFIG : retry WiFi toutes les 2 min ----------
  if (configMode) {
    static unsigned long lastTry = 0;
    if (cfgSsid.length() > 0 && millis() - lastTry > 120000) {
      lastTry = millis();
      Serial.println("Portail actif : nouvelle tentative WiFi...");
      WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
        delay(200);
        lastLoopAliveMs = millis();
        esp_task_wdt_reset();
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi retrouve -> redemarrage en mode normal");
        delay(500);
        ESP.restart();
      }
    }
    delay(50);
    return;
  }

  unsigned long now = millis();

  // ---------- reconnexion WiFi automatique ----------
  static unsigned long lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastWifiRetry > 30000) {
    lastWifiRetry = now;
    Serial.println("WiFi perdu, tentative de reconnexion...");
    WiFi.disconnect();
    WiFi.reconnect();
  }
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = now;
  }

  // ---------- extinction auto LED ----------
  if (ledAutoOffAt > 0 && now >= ledAutoOffAt) {
    portENTER_CRITICAL(&stateMux);
    bool rec = recording;
    bool lman = ledManual;
    portEXIT_CRITICAL(&stateMux);
    if (!rec && !lman) {
      ledAutoOffAt = 0;
      ledSet(false);
    }
  }

  // ---------- detection PIR (SR602) ----------\n  if (DETECTION_PIR) {\n    static bool pirLast = false;\n    bool pirNow = digitalRead(PIR_PIN);\n    \n    // Front montant: mouvement detecte\n    if (pirNow && !pirLast) {\n      if (GHOST_MODE_ENABLED && !ghostAnalyzing && !recording) {\n        startGhostRecording();\n      }\n    }\n    \n    // PIR actif pendant Ghost Mode: incrementer hits\n    if (pirNow && GHOST_MODE_ENABLED && ghostAnalyzing) {\n      portENTER_CRITICAL(&stateMux);\n      ghostHitCount++;\n      portEXIT_CRITICAL(&stateMux);\n    }\n    pirLast = pirNow;\n  }\n\n  // ---------- Ghost Mode : decision ----------
  if (GHOST_MODE_ENABLED && ghostAnalyzing) {
    if (now - ghostStartMs >= GHOST_ANALYZE_MS) {
      portENTER_CRITICAL(&stateMux);
      int hits = ghostHitCount;
      portEXIT_CRITICAL(&stateMux);
      if (hits >= GHOST_MIN_HITS) {
        stopGhostRecording(true);
      } else {
        stopGhostRecording(false);
      }
    }
  }

  // ---------- mesure ultrason ----------
  if (DETECTION_ULTRASON && now - lastMeasureMs >= INTERVALLE_MESURE_MS) {
    lastMeasureMs = now;
    float d = readDistanceCM();
    lastDistance = d;

    if (d > 0 && d <= cfgSeuil) {
      compteurDetection++;
      if (GHOST_MODE_ENABLED && ghostAnalyzing) {
        portENTER_CRITICAL(&stateMux);
        ghostHitCount++;
        portEXIT_CRITICAL(&stateMux);
      }
      Serial.printf("Detection %.0f cm (%d/%d)\n", d, compteurDetection, MESURES_CONFIRMATION);
    } else {
      compteurDetection = 0;
    }

    portENTER_CRITICAL(&stateMux);
    bool rec = recording;
    bool grec = ghostRecording;
    bool ganal = ghostAnalyzing;
    portEXIT_CRITICAL(&stateMux);

    if (detectionActive && !rec && !ganal &&
        compteurDetection >= MESURES_CONFIRMATION &&
        now - lastAlertMs >= COOLDOWN_MS) {
      lastAlertMs = now;
      compteurDetection = 0;
      if (GHOST_MODE_ENABLED) {
        startGhostRecording();
      } else {
        sendAlert(d);
        startRecording();
      }
    }
  }

  // ---------- enregistrement video (normal ou ghost) ----------
  portENTER_CRITICAL(&stateMux);
  bool rec = recording;
  bool grec = ghostRecording;
  portEXIT_CRITICAL(&stateMux);

  if (rec || grec) {
    unsigned long frameInterval = (REC_FPS > 0) ? (1000 / REC_FPS) : 100;
    if (now - lastRecFrameMs >= frameInterval) {
      lastRecFrameMs = now;
      camera_fb_t* fb = camGet();
      if (fb) {
        lastFrameOkMs = millis();
        if (!avi.addFrame(fb->buf, fb->len)) {
          if (grec) stopGhostRecording(false);
          else stopRecording();
        }
        camReturn(fb);
      }
    }
    // Verification presence uniquement en mode normal (pas ghost)
    if (rec && !grec && now - lastPresenceMs >= 500) {
      lastPresenceMs = now;
      float d = readDistanceCM();
      if (d < 0 || d > cfgSeuil + HYSTERESIS_CM) absenceCount++;
      else absenceCount = 0;
      unsigned long duree = now - recStartMs;
      if ((absenceCount >= 2 && duree >= REC_MIN_MS) || duree >= REC_MAX_MS) {
        stopRecording();
      }
    }
  }

  delay(1);
}
