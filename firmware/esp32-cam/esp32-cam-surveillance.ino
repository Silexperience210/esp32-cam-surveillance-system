/*
 * ESP32-CAM Surveillance intelligente v2
 * --------------------------------------
 * - Streaming video MJPEG en direct (navigateur)
 * - Declenchement par capteur ultrason HC-SR04
 * - Alerte Telegram : photo + notification
 * - ENREGISTREMENT VIDEO AVI sur microSD pendant la presence
 * - COMMANDES TELEGRAM : /photo /video /status /on /off /help
 *
 * Carte : AI Thinker ESP32-CAM (clone Binghe) + camera OV3660
 * Carte microSD : FAT32 (mode 1-bit : GPIO 12/13 restent libres pour le HC-SR04)
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_http_server.h"
#include "FS.h"
#include "SD_MMC.h"
#include "time.h"
#include <Preferences.h>
#include "avi_writer.h"

// ================ CONFIGURATION : VIA LE PORTAIL WEB ================
// Plus rien a modifier dans le code !
// Au 1er demarrage (ou si le WiFi est injoignable), la camera cree
// le reseau WiFi "ESP32-CAM-Setup-xxxx" (mot de passe : 12345678).
// Connecte-toi dessus puis ouvre http://192.168.4.1 pour tout regler :
// WiFi, bot Telegram, chat ID, nom de la camera, distance de detection.
// En fonctionnement normal : config modifiable sur http://<IP>/setup

// Detection ultrason (hardware)
const bool  DETECTION_ULTRASON      = false;  // false = HC-SR04 non requis (stream + Telegram seuls)
const float HYSTERESIS_CM           = 20.0;   // marge pour detecter la sortie
const int   MESURES_CONFIRMATION    = 3;      // nb de mesures consecutives
const unsigned long INTERVALLE_MESURE_MS = 250;
const unsigned long COOLDOWN_MS          = 30000; // delai min entre 2 alertes

// Enregistrement video
const uint8_t  REC_FPS     = 10;      // images/seconde de la video AVI
const unsigned long REC_MIN_MS = 5000;   // duree minimum d'une video
const unsigned long REC_MAX_MS = 30000;  // duree maximum d'une video
const size_t MAX_UPLOAD_TELEGRAM = 18UL * 1024 * 1024;  // 18 Mo max envoi TG

// Telegram
const unsigned long TELEGRAM_POLL_MS = 3000;  // verification commandes /3 s

// Watchdog anti-plantage (fonctionnement 24/7)
const unsigned long WDT_LOOP_STALL_MS  = 60000;   // boucle figee > 60 s -> restart
const unsigned long WDT_STREAM_STALL_MS = 30000;  // stream actif sans frame > 30 s -> restart
const unsigned long WDT_WIFI_LOST_MS   = 300000;  // WiFi perdu > 5 min -> restart

// Pins HC-SR04 (carte SD en mode 1-bit, pas de conflit)
#define TRIG_PIN 13
#define ECHO_PIN 12

// LEDs
#define FLASH_LED_PIN 4    // LED flash blanche
#define REC_LED_PIN   33   // LED rouge arriere (active LOW) = temoin REC

// LED externe d'eclairage (GPIO 4 = meme pin que la flash integree : les deux
// s'allument ensemble). Allumage auto lors des alertes entre LED_NUIT_DEBUT et LED_NUIT_FIN.
#define EXT_LED_PIN 4
const int LED_NUIT_DEBUT = 18;  // 18h
const int LED_NUIT_FIN   = 7;   // 7h du matin
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

// ============================ ETAT GLOBAL ===========================

// Configuration chargee depuis la flash (modifiable via portail web)
bool configMode = false;
String cfgSsid, cfgPass, cfgToken, cfgChatId, cfgCamName = "ESP32-CAM";
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

// LED externe
bool ledState = false;
bool ledManual = false;           // true = controle manuel (prioritaire sur l'auto)
unsigned long ledAutoOffAt = 0;   // extinction auto programmee

void ledSet(bool on) {
  ledState = on;
  digitalWrite(EXT_LED_PIN, on ? HIGH : LOW);
}

bool isNight() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return false;
  int h = timeinfo.tm_hour;
  return (h >= LED_NUIT_DEBUT || h < LED_NUIT_FIN);
}

// Watchdog
volatile unsigned long lastLoopAliveMs = 0;
volatile unsigned long lastFrameOkMs = 0;
volatile int streamClients = 0;
unsigned long lastWifiOkMs = 0;

void watchdogTask(void* param) {
  while (true) {
    unsigned long now = millis();
    if (lastLoopAliveMs > 0 && now - lastLoopAliveMs > WDT_LOOP_STALL_MS) {
      Serial.println("WATCHDOG: boucle principale figee -> redemarrage");
      delay(200);
      ESP.restart();
    }
    if ((streamClients > 0 || recording) && lastFrameOkMs > 0 &&
        now - lastFrameOkMs > WDT_STREAM_STALL_MS) {
      Serial.println("WATCHDOG: camera ne produit plus de frames -> redemarrage");
      delay(200);
      ESP.restart();
    }
    if (configMode || WiFi.status() == WL_CONNECTED) {
      lastWifiOkMs = now;
    } else if (lastWifiOkMs > 0 && now - lastWifiOkMs > WDT_WIFI_LOST_MS) {
      Serial.println("WATCHDOG: WiFi perdu depuis 5 min -> redemarrage");
      delay(200);
      ESP.restart();
    }
    delay(1000);
  }
}

// ================== CONFIGURATION (flash + portail) =================

void loadConfig() {
  Preferences prefs;
  prefs.begin("camcfg", true);   // lecture seule
  cfgSsid    = prefs.getString("ssid", "");
  cfgPass    = prefs.getString("pass", "");
  cfgToken   = prefs.getString("token", "");
  cfgChatId  = prefs.getString("chatid", "");
  cfgCamName = prefs.getString("name", "ESP32-CAM");
  cfgSeuil   = prefs.getFloat("seuil", 100.0);
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
  prefs.end();
}

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
    "<title>ESP32-CAM Config</title><style>"
    "body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:20px auto;padding:0 15px}"
    "input{width:100%;padding:10px;margin:5px 0 15px;border-radius:6px;border:1px solid #444;background:#222;color:#eee;box-sizing:border-box}"
    "button{width:100%;padding:12px;background:#1976d2;color:#fff;border:0;border-radius:6px;font-size:16px;cursor:pointer}"
    "label{color:#aaa;font-size:14px}h2{text-align:center}"
    ".info{background:#1b3a1b;padding:10px;border-radius:6px;font-size:13px;margin-bottom:15px}"
    "</style></head><body>");
  h += "<h2>&#x1F4F7; Configuration ESP32-CAM</h2>";
  if (configMode) h += "<div class='info'>&#x1F4E1; La camera n'a pas pu se connecter au WiFi. Remplis ce formulaire : elle redemarrera sur ton reseau.</div>";
  h += F("<form method='POST' action='/save'>"
    "<label>Nom du reseau WiFi (SSID)</label>"
    "<input name='ssid' list='nets' value='");
  h += cfgSsid;
  h += F("' required><datalist id='nets'></datalist>"
    "<label>Mot de passe WiFi</label>"
    "<input name='pass' type='password' placeholder='(laisser vide = inchange)'>"
    "<label>Token bot Telegram (@BotFather)</label>"
    "<input name='token' value='");
  h += cfgToken;
  h += F("'>"
    "<label>Chat ID Telegram</label>"
    "<input name='chatid' value='");
  h += cfgChatId;
  h += F("'>"
    "<label>Nom de la camera</label>"
    "<input name='name' value='");
  h += cfgCamName;
  h += F("'>"
    "<label>Distance de detection (cm)</label>"
    "<input name='seuil' type='number' value='");
  h += String((int)cfgSeuil);
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
  char buf[1024];
  int len = min((int)sizeof(buf) - 1, (int)req->content_len);
  int ret = httpd_req_recv(req, buf, len);
  if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
  buf[ret] = 0;
  String body = buf;

  String s = postParam(body, "ssid");
  if (s.length()) cfgSsid = s;
  s = postParam(body, "pass");
  if (s.length()) cfgPass = s;    // vide = inchange
  s = postParam(body, "token");
  if (s.length()) cfgToken = s;
  s = postParam(body, "chatid");
  if (s.length()) cfgChatId = s;
  s = postParam(body, "name");
  if (s.length()) cfgCamName = s;
  s = postParam(body, "seuil");
  if (s.length()) cfgSeuil = s.toFloat();
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

// Portail de configuration (mode point d'acces)
void startConfigPortal() {
  configMode = true;
  String mac = WiFi.macAddress();
  String apName = "ESP32-CAM-Setup-" + mac.substring(12, 14) + mac.substring(15, 17);
  WiFi.mode(WIFI_AP_STA);   // AP + STA (necessaire pour le scan des reseaux)
  WiFi.softAP(apName.c_str(), "12345678");
  Serial.println("\n=== MODE CONFIGURATION ===");
  Serial.println("Reseau WiFi : " + apName + "  (mot de passe : 12345678)");
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
    Serial.println("Portail HTTP demarre");
  }
}

// ============================ TELEGRAM ==============================

String urlencode(const String& s) {
  String out;
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
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) return false;
  String url = "/bot" + cfgToken + "/sendMessage?chat_id=" + cfgChatId +
               "&text=" + urlencode(text);
  client.print("GET " + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");
  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 8000) delay(10);
  String resp;
  while (client.available()) resp += (char)client.read();
  client.stop();
  return resp.indexOf("\"ok\":true") >= 0;
}

bool telegramSendPhoto(uint8_t* jpg, size_t jpgLen, const String& caption) {
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
  }
  client.print(tail);

  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 10000) delay(10);
  String resp;
  while (client.available()) resp += (char)client.read();
  client.stop();
  bool ok = resp.indexOf("\"ok\":true") >= 0;
  Serial.println(ok ? "Photo envoyee !" : "Erreur TG: " + resp.substring(0, 200));
  return ok;
}

bool telegramSendDocument(const String& path, const String& caption) {
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
    }
  }
  client.print(tail);
  f.close();

  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 15000) delay(10);
  String resp;
  while (client.available()) resp += (char)client.read();
  client.stop();
  bool ok = resp.indexOf("\"ok\":true") >= 0;
  Serial.println(ok ? "Video envoyee !" : "Erreur TG doc: " + resp.substring(0, 200));
  return ok;
}

// ================== COMMANDES TELEGRAM ENTRANTES ====================

String helpText() {
  return "📷 ESP32-CAM Surveillance\n"
         "📸 /photo — photo instantanee\n"
         "📹 /video — recevoir la derniere video\n"
         "💡 /led — LED on/off (/ledon /ledoff /ledauto)\n"
         "📊 /status — etat du systeme\n"
         "⏸ /off — desactiver les alertes\n"
         "▶️ /on — reactiver les alertes\n"
         "🌐 Direct : http://" + WiFi.localIP().toString() + "/ (meme WiFi)";
}

String statusText() {
  String s = "📊 STATUT\n";
  s += "🌐 IP : http://" + WiFi.localIP().toString() + "/\n";
  s += "⏱ Uptime : " + String(millis() / 60000) + " min\n";
  s += "📏 Distance : " + (lastDistance > 0 ? String((int)lastDistance) + " cm" : String("rien")) + "\n";
  s += "🔍 Detection ultrason : " + String(DETECTION_ULTRASON ? "oui" : "NON (mode stream seul)") + "\n";
  s += "🔔 Alertes : " + String(detectionActive ? "ACTIVEES" : "desactivees") + "\n";
  s += "🔴 Enregistrement : " + String(recording ? "EN COURS" : "non") + "\n";
  s += "💾 Carte SD : " + String(sdReady ? "OK" : "ABSENTE") + "\n";
  s += "📹 Videos enregistrees : " + String(videoCount) + "\n";
  s += "💡 LED : " + String(ledState ? "ALLUMEE" : "eteinte") + (ledManual ? " (manuel)" : " (auto)") + "\n";
  s += "🌙 Mode nuit (18h-7h) : " + String(isNight() ? "oui" : "non");
  return s;
}

void handleCommand(String cmd) {
  cmd.trim();
  int at = cmd.indexOf('@');
  if (at > 0) cmd = cmd.substring(0, at);
  Serial.println("Commande Telegram : " + cmd);

  if (cmd.startsWith("/photo")) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      telegramSendPhoto(fb->buf, fb->len, "📸 Photo a la demande");
      esp_camera_fb_return(fb);
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
    if (cmd.startsWith("/ledauto")) ledManual = false;
    else if (cmd.startsWith("/ledon")) { ledManual = true; ledSet(true); }
    else if (cmd.startsWith("/ledoff")) { ledManual = true; ledSet(false); }
    else { ledManual = true; ledSet(!ledState); }   // /led = toggle
    telegramSendMessage(String("\xF0\x9F\x92\xA1 LED : ") + (ledState ? "ALLUMEE" : "eteinte") +
                        (ledManual ? " (manuel)" : " (auto)"));
  } else if (cmd.startsWith("/status")) {
    telegramSendMessage(statusText());
  } else if (cmd.startsWith("/off")) {
    detectionActive = false;
    telegramSendMessage("⏸ Alertes automatiques DESACTIVEES.\n/on pour reactiver.");
  } else if (cmd.startsWith("/on")) {
    detectionActive = true;
    telegramSendMessage("▶️ Alertes automatiques ACTIVEES.");
  } else if (cmd.startsWith("/help") || cmd.startsWith("/start")) {
    telegramSendMessage(helpText());
  } else {
    telegramSendMessage("❓ Commande inconnue : " + cmd + "\n\n" + helpText());
  }
}

void telegramPoll() {
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) return;
  String url = "/bot" + cfgToken + "/getUpdates?offset=" + String(lastUpdateId + 1) +
               "&limit=5&timeout=0";
  client.print("GET " + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");
  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 8000) delay(10);
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
      if (chatId == cfgChatId && text.length() > 0) handleCommand(text);
    }
    pos = u + 12;
  }
}

// ============================ STREAMING HTTP ========================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t* req) {
  camera_fb_t* fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  streamClients++;

  // Qualite JPEG reduite PENDANT le stream : frames ~40% plus legeres
  // = flux plus fluide. Les photos /capture restent en qualite maximale.
  sensor_t* s = esp_camera_sensor_get();
  int oldQuality = -1;
  if (s) { oldQuality = s->status.quality; s->set_quality(s, 16); }

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }
    lastFrameOkMs = millis();
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;
  }
  if (s && oldQuality >= 0) s->set_quality(s, oldQuality);
  streamClients--;
  return res;
}

// /led?state=on|off|toggle|auto  (+ redir=1 pour revenir a la page)
static esp_err_t led_handler(httpd_req_t* req) {
  char query[64];
  bool redir = false;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[12];
    if (httpd_query_key_value(query, "state", val, sizeof(val)) == ESP_OK) {
      if (strcmp(val, "auto") == 0) ledManual = false;
      else {
        ledManual = true;
        if (strcmp(val, "on") == 0) ledSet(true);
        else if (strcmp(val, "off") == 0) ledSet(false);
        else if (strcmp(val, "toggle") == 0) ledSet(!ledState);
      }
    }
    char r[8];
    if (httpd_query_key_value(query, "redir", r, sizeof(r)) == ESP_OK) redir = true;
  }
  if (redir) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
  }
  String json = "{\"led\":" + String(ledState ? "true" : "false") +
                ",\"manual\":" + String(ledManual ? "true" : "false") +
                ",\"night\":" + String(isNight() ? "true" : "false") + "}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t capture_handler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t serveSDFile(httpd_req_t* req, const String& path) {
  File f = SD_MMC.open(path);
  if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "video/x-msvideo");
  String fname = path.substring(path.lastIndexOf('/') + 1);
  httpd_resp_set_hdr(req, "Content-Disposition", ("attachment; filename=\"" + fname + "\"").c_str());
  uint8_t* buf = (uint8_t*)malloc(4096);
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
  if (!sdReady || lastVideoPath.length() == 0) { httpd_resp_send_404(req); return ESP_FAIL; }
  return serveSDFile(req, lastVideoPath);
}

static esp_err_t dl_handler(httpd_req_t* req) {
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
  if (n.indexOf("..") >= 0 || n.indexOf('/') >= 0 || !n.endsWith(".avi")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nom invalide");
    return ESP_FAIL;
  }
  return serveSDFile(req, "/" + n);
}

static esp_err_t list_handler(httpd_req_t* req) {
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
        html += "<li><a href='/dl?f=" + n.substring(1) + "'>" + n + "</a> (" +
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

// /info : identite de la camera (utilise par le moniteur multi-cams)
static esp_err_t info_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  String json = "{\"name\":\"" + cfgCamName +
                "\",\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"model\":\"ESP32-CAM OV3660\"" +
                ",\"sd\":" + String(sdReady ? "true" : "false") +
                ",\"detection\":" + String(detectionActive ? "true" : "false") +
                ",\"led\":" + String(ledState ? "true" : "false") + "}";
  return httpd_resp_send(req, json.c_str(), json.length());
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 12;

  httpd_handle_t server = NULL;
  httpd_uri_t index_uri   = { .uri = "/",        .method = HTTP_GET, .handler = index_handler,   .user_ctx = NULL };
  httpd_uri_t stream_uri  = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,  .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
  httpd_uri_t video_uri   = { .uri = "/video",   .method = HTTP_GET, .handler = video_handler,   .user_ctx = NULL };
  httpd_uri_t list_uri    = { .uri = "/list",    .method = HTTP_GET, .handler = list_handler,    .user_ctx = NULL };
  httpd_uri_t dl_uri      = { .uri = "/dl",      .method = HTTP_GET, .handler = dl_handler,      .user_ctx = NULL };
  httpd_uri_t info_uri    = { .uri = "/info",    .method = HTTP_GET, .handler = info_handler,    .user_ctx = NULL };
  httpd_uri_t setup_uri   = { .uri = "/setup",   .method = HTTP_GET, .handler = config_handler,  .user_ctx = NULL };
  httpd_uri_t scan_uri    = { .uri = "/scan",    .method = HTTP_GET, .handler = scan_handler,    .user_ctx = NULL };
  httpd_uri_t save_uri    = { .uri = "/save",    .method = HTTP_POST,.handler = save_handler,    .user_ctx = NULL };
  httpd_uri_t led_uri     = { .uri = "/led",     .method = HTTP_GET, .handler = led_handler,     .user_ctx = NULL };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &stream_uri);
    httpd_register_uri_handler(server, &capture_uri);
    httpd_register_uri_handler(server, &video_uri);
    httpd_register_uri_handler(server, &list_uri);
    httpd_register_uri_handler(server, &dl_uri);
    httpd_register_uri_handler(server, &info_uri);
    httpd_register_uri_handler(server, &setup_uri);
    httpd_register_uri_handler(server, &scan_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &led_uri);
    Serial.println("Serveur HTTP demarre");
  }
}

// ============================ ULTRASON ==============================

float readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);  // timeout 30 ms
  if (duration == 0) return -1.0;
  return duration * 0.0343f / 2.0f;
}

// ======================= ALERTE & ENREGISTREMENT ====================

void sendAlert(float distance) {
  // Eclairage nocturne : LED allumee pour la photo (si entre 18h et 7h)
  if (isNight() && !ledManual) {
    ledSet(true);
    ledAutoOffAt = millis() + 10000;  // extinction auto dans 10 s si pas d'enregistrement
  }
  if (FLASH_PENDANT_CAPTURE) {
    digitalWrite(FLASH_LED_PIN, HIGH);
    delay(120);
  }
  camera_fb_t* fb = esp_camera_fb_get();   // purge frame ancienne
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
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
  telegramSendPhoto(fb->buf, fb->len, caption);
  esp_camera_fb_return(fb);
}

void startRecording() {
  if (!sdReady) {
    telegramSendMessage("⚠️ Carte SD absente : enregistrement video impossible.");
    return;
  }
  String name;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100)) {
    char buf[40];
    strftime(buf, sizeof(buf), "/rec_%Y-%m-%d_%H-%M-%S.avi", &timeinfo);
    name = buf;
  } else {
    name = "/rec_" + String(millis()) + ".avi";
  }
  ledAutoOffAt = 0;  // un enregistrement demarre : extinction geree par stopRecording
  if (isNight() && !ledManual) ledSet(true);  // eclairage pendant toute la video
  aviFile = SD_MMC.open(name, FILE_WRITE);
  if (!aviFile) {
    Serial.println("ERREUR ouverture " + name);
    return;
  }
  uint16_t maxFrames = (uint16_t)((REC_MAX_MS / 1000 + 2) * REC_FPS);
  if (!avi.begin(aviFile, frameW, frameH, REC_FPS, maxFrames)) {
    aviFile.close();
    return;
  }
  recording = true;
  recStartMs = millis();
  lastRecFrameMs = 0;
  absenceCount = 0;
  lastVideoPath = name;
  digitalWrite(REC_LED_PIN, LOW);   // LED rouge ON
  Serial.println("REC -> " + name);
}

void stopRecording() {
  if (!recording) return;
  recording = false;
  digitalWrite(REC_LED_PIN, HIGH);  // LED rouge OFF
  if (!ledManual) ledSet(false);    // extinction LED eclairage (sauf controle manuel)
  uint16_t frames = avi.end();
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
  lastAlertMs = millis();  // cooldown avant prochaine alerte
}

// ============================ CAMERA / SD ===========================

bool initCamera() {
  camera_config_t config;
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
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_SVGA;   // 800x600
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    frameW = 800; frameH = 600;
  } else {
    config.frame_size   = FRAMESIZE_VGA;    // 640x480
    config.jpeg_quality = 15;
    config.fb_count     = 1;
    frameW = 640; frameH = 480;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("ERREUR init camera : 0x%x\n", err);
    return false;
  }
  return true;
}

bool initSD() {
  // mode 1-bit (true) : utilise GPIO 2/14/15, libere 4/12/13 pour le HC-SR04
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Carte SD absente -> videos desactivees (alertes photo OK)");
    return false;
  }
  Serial.printf("Carte SD OK : %llu Mo\n", SD_MMC.cardSize() / (1024 * 1024));
  return true;
}

// ============================ SETUP / LOOP ==========================

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32-CAM Surveillance v2 ===");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  pinMode(REC_LED_PIN, OUTPUT);
  digitalWrite(REC_LED_PIN, HIGH);   // LED rouge eteinte (active LOW)

  if (!initCamera()) {
    Serial.println("Camera KO - redemarrage dans 5 s");
    delay(5000);
    ESP.restart();
  }

  loadConfig();

  bool wifiOk = false;
  if (cfgSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    WiFi.setSleep(false);
    Serial.print("Connexion WiFi '" + cfgSsid + "'");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
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
    xTaskCreatePinnedToCore(watchdogTask, "watchdog", 2048, NULL, 1, NULL, 0);
    return;
  }

  Serial.print("WiFi OK : http://");
  Serial.println(WiFi.localIP());

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  sdReady = initSD();
  startCameraServer();

  // Watchdog anti-plantage sur l'autre coeur
  lastWifiOkMs = millis();
  lastLoopAliveMs = millis();
  xTaskCreatePinnedToCore(watchdogTask, "watchdog", 2048, NULL, 1, NULL, 0);
  Serial.println("Watchdog actif");

  delay(1500);
  String bootMsg = "✅ Camera en ligne !\n";
  bootMsg += String("🔍 Detection ultrason : ") + (DETECTION_ULTRASON ? "ACTIVEE" : "desactivee") + "\n\n";
  telegramSendMessage(bootMsg + helpText());
}

void loop() {
  lastLoopAliveMs = millis();
  if (configMode) { delay(50); return; }  // portail de config : le serveur HTTP gere tout

  unsigned long now = millis();

  // ---------- reconnexion WiFi automatique ----------
  static unsigned long lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastWifiRetry > 30000) {
    lastWifiRetry = now;
    Serial.println("WiFi perdu, tentative de reconnexion...");
    WiFi.disconnect();
    WiFi.reconnect();
  }

  // ---------- extinction auto LED (alerte sans enregistrement) ----------
  if (ledAutoOffAt > 0 && now >= ledAutoOffAt && !recording && !ledManual) {
    ledAutoOffAt = 0;
    ledSet(false);
  }

  // ---------- mesure ultrason ----------
  if (DETECTION_ULTRASON && now - lastMeasureMs >= INTERVALLE_MESURE_MS) {
    lastMeasureMs = now;
    float d = readDistanceCM();
    lastDistance = d;

    if (d > 0 && d <= cfgSeuil) {
      compteurDetection++;
      Serial.printf("Detection %.0f cm (%d/%d)\n", d, compteurDetection, MESURES_CONFIRMATION);
    } else {
      compteurDetection = 0;
    }

    if (detectionActive && !recording &&
        compteurDetection >= MESURES_CONFIRMATION &&
        now - lastAlertMs >= COOLDOWN_MS) {
      lastAlertMs = now;
      compteurDetection = 0;
      sendAlert(d);        // 1) photo Telegram immediate
      startRecording();    // 2) video pendant la presence
    }
  }

  // ---------- enregistrement video ----------
  if (recording) {
    if (now - lastRecFrameMs >= 1000 / REC_FPS) {
      lastRecFrameMs = now;
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        lastFrameOkMs = millis();
        if (!avi.addFrame(fb->buf, fb->len)) stopRecording();  // buffer plein
        esp_camera_fb_return(fb);
      }
    }
    if (now - lastPresenceMs >= 500) {   // presence toujours la ?
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

  // ---------- commandes Telegram ----------
  if (now - lastPollMs >= TELEGRAM_POLL_MS) {
    lastPollMs = now;
    telegramPoll();
  }

  delay(1);  // watchdog
}
