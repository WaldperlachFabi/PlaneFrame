#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>      // https://github.com/tzapu/WiFiManager (ESP32-kompatibler Fork)
#include <Preferences.h>      // Für persistenten Flag & Speicherung client_id/secret
#include <time.h>             // NTP / Zeitfunktionen
#include <stdlib.h>

// ---------- Globals ----------
Preferences prefs;
bool portalActive = false;

// ---------- WiFiManager global ----------
WiFiManager wm;
bool wmParamsInitialized = false;

// persistent buffers & parameter pointers (müssen im RAM bleiben)
char buf_clientid[65];
char buf_clientsecret[129];
char buf_poll[16];
char buf_landingDelay[16];
char buf_landingHold[16];
char buf_startHold[16];
char buf_animStep[16];
char buf_fixedOffset[32];
char buf_ntp[128];
char buf_brightness[8];
char buf_quietStart[8]; // NEU für Quiet Hours
char buf_quietEnd[8];   // NEU für Quiet Hours

WiFiManagerParameter* p_client_id = nullptr;
WiFiManagerParameter* p_client_secret = nullptr;
WiFiManagerParameter* p_poll = nullptr;
WiFiManagerParameter* p_landingDelay = nullptr;
WiFiManagerParameter* p_landingHold = nullptr;
WiFiManagerParameter* p_startHold = nullptr;
WiFiManagerParameter* p_animStep = nullptr;
WiFiManagerParameter* p_fixedOffset = nullptr;
WiFiManagerParameter* p_ntp = nullptr;
WiFiManagerParameter* p_brightness = nullptr;
WiFiManagerParameter* p_quietStart = nullptr; // NEU für Quiet Hours
WiFiManagerParameter* p_quietEnd = nullptr;   // NEU für Quiet Hours


// ---------- OpenSky ----------
String client_id = "";      // leer -> User muss setzen
String client_secret = "";  // leer -> User muss setzen
String accessToken = "";
unsigned long tokenExpiresAt = 0;
unsigned long lastTokenAttempt = 0;
const unsigned long TOKEN_ATTEMPT_INTERVAL_MS = 5000;

// ---------- LED ----------
#define LED_PIN 4
#define NUM_LEDS 60
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

const int NORTH_START_LED = 0;
const int NORTH_END_LED   = 29;
const int SOUTH_START_LED = 30;
const int SOUTH_END_LED   = 59;

// ---------- Configurable defaults (werden beim ersten Start verwendet, können im Portal geändert werden) ----------
unsigned long planePollInterval      = 15000UL;
unsigned long landingDelayMs         = 22000UL;
unsigned long landingHoldMs          = 3000UL;
unsigned long startHoldMs            = 1500UL;
unsigned long animStepMs             = 30UL;
int           defaultBrightness      = 64;
long          fixedTimeOffsetSeconds = 2 * 3600L; // +2h
String        ntpServer              = "de.pool.ntp.org";
int           quietHourStart         = 23; // NEU: Konfigurierbar
int           quietHourEnd           = 7;  // NEU: Konfigurierbar

// ---------- Timing ----------
unsigned long lastPlaneCheck = 0;
const unsigned long ANIM_MIN_STEP_MS = 1;

// ---------- Runway Boxen ----------
struct Box {
  float nwLat, nwLon;
  float swLat, swLon;
  float seLat, seLon;
  float neLat, neLon;
};

Box southRunwayEast = {48.350591,11.823452, 48.341976,11.825173, 48.345327,11.853362, 48.352096,11.851771};
Box northRunwayEast = {48.370236,11.820893, 48.362405,11.822028, 48.365147,11.852725, 48.372215,11.851543};

Box southRunwayWest = {48.340595,11.700290, 48.332208,11.701961, 48.337140,11.754650, 48.343331,11.753105};
Box northRunwayWest = {48.363273,11.714366, 48.354035,11.716009, 48.358893,11.771707, 48.365683,11.770730};

// ---------- Tracking ----------
struct PlaneState {
  bool landingTriggered = false;
  bool startTriggered   = false;
  bool reversedDirection = false;
  unsigned long detectedAt = 0;
  String lastPlane = "";
  String pendingPlane = "";
};

PlaneState southEast, southWest, northEast, northWest;

bool animationRunning = false;
bool wifiAnimDone = false;

// ---------- Letzte 5 Flugzeuge ----------
String recentPlanes[5] = {"", "", "", "", ""};
int recentIndex = 0;

bool wasRecentlyTriggered(String callsign) {
  for (int i = 0; i < 5; i++) {
    if (recentPlanes[i] == callsign) return true;
  }
  return false;
}

void addRecentPlane(String callsign) {
  recentPlanes[recentIndex] = callsign;
  recentIndex = (recentIndex + 1) % 5;
}

// ---------- Quiet Hours (konfigurierbar) ----------
bool isQuietHours(struct tm &timeinfo) {
  int h = timeinfo.tm_hour;
  if (quietHourStart > quietHourEnd) { // Prüft auf Mitternacht-Wechsel (z.B. 23-7)
      return (h >= quietHourStart || h < quietHourEnd);
  } else { // Prüft am selben Tag (z.B. 9-17)
      return (h >= quietHourStart && h < quietHourEnd);
  }
}

// ---------- Hilfsfunktionen ----------
bool isInside(Box box, float lat, float lon){
  bool inside = lat >= min(box.swLat, box.nwLat) && lat <= max(box.neLat, box.seLat) &&
                lon >= min(box.nwLon, box.neLon) && lon <= max(box.swLon, box.neLon);
  return inside;
}

void setStripColorAll(uint8_t r, uint8_t g, uint8_t b) {
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}

void showErrorLED(uint8_t r, uint8_t g, uint8_t b, int durationMs) {
  setStripColorAll(r, g, b);
  delay(durationMs);
  strip.clear();
  strip.show();
}

void purpleFadeOnce(int steps=40, int stepDelay=10) {
  uint8_t pr = 128, pg = 0, pb = 128;
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(pr, pg, pb));
  int maxB = constrain(defaultBrightness, 0, 255);
  for (int b = 0; b <= maxB; b += max(1, maxB/steps)) {
    strip.setBrightness(b); strip.show(); delay(stepDelay);
  }
  for (int b = maxB; b >= 0; b -= max(1, maxB/steps)) {
    strip.setBrightness(b); strip.show(); delay(stepDelay);
  }
  strip.clear(); strip.show();
}

// ---------- Animationen ----------
void animateLandingRange(int startLed, int endLed) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100) && isQuietHours(timeinfo)) return;

  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = endLed; i >= startLed; --i) {
    strip.setPixelColor(i, strip.Color(255,0,0));
    strip.show();
    delay(max((unsigned long)ANIM_MIN_STEP_MS, animStepMs));
  }
  delay(landingHoldMs);
  strip.clear(); strip.show();
  animationRunning = false;
}

void animateStartRange(int startLed, int endLed) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100) && isQuietHours(timeinfo)) return;

  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = endLed; i >= startLed; --i) {
    strip.setPixelColor(i, strip.Color(0,0,255));
    strip.show();
    delay(max((unsigned long)ANIM_MIN_STEP_MS, animStepMs));
  }
  delay(startHoldMs);
  strip.clear(); strip.show();
  animationRunning = false;
}

void animateLandingRangeReversed(int startLed, int endLed) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100) && isQuietHours(timeinfo)) return;
    
  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = startLed; i <= endLed; ++i) {
    strip.setPixelColor(i, strip.Color(255, 0, 0));
    strip.show();
    delay(max((unsigned long)ANIM_MIN_STEP_MS, animStepMs));
  }
  delay(landingHoldMs);
  strip.clear(); strip.show();
  animationRunning = false;
}

void animateStartRangeReversed(int startLed, int endLed) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100) && isQuietHours(timeinfo)) return;

  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = startLed; i <= endLed; ++i) {
    strip.setPixelColor(i, strip.Color(0, 0, 255));
    strip.show();
    delay(max((unsigned long)ANIM_MIN_STEP_MS, animStepMs));
  }
  delay(startHoldMs);
  strip.clear(); strip.show();
  animationRunning = false;
}

void animateWiFiConnected() {
  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(0,255,0));
    strip.show();
    delay(20);
  }
  delay(500);
  strip.clear(); strip.show();
  animationRunning = false;
}

// ---------- Simple parsing helpers ----------
unsigned long parseUL(const String &s, unsigned long def) {
  if (s.length() == 0) return def;
  char *endptr;
  unsigned long v = strtoul(s.c_str(), &endptr, 10);
  if (endptr == s.c_str()) return def;
  return v;
}
long parseLongVal(const String &s, long def) {
  if (s.length() == 0) return def;
  char *endptr;
  long v = strtol(s.c_str(), &endptr, 10);
  if (endptr == s.c_str()) return def;
  return v;
}
int parseIntVal(const String &s, int def) {
  if (s.length() == 0) return def;
  char *endptr;
  long v = strtol(s.c_str(), &endptr, 10);
  if (endptr == s.c_str()) return def;
  if (v < INT_MIN || v > INT_MAX) return def;
  return (int)v;
}

// ---------- Config load/save/print ----------
void printConfig() {
  Serial.println("==== CONFIG ====");
  Serial.printf("client_id: %s\n", client_id.c_str());
  Serial.printf("client_secret: %s\n", client_secret.length() ? "<set>" : "<not set>");
  Serial.printf("planePollInterval: %lu ms\n", planePollInterval);
  Serial.printf("landingDelayMs: %lu ms\n", landingDelayMs);
  Serial.printf("landingHoldMs: %lu ms\n", landingHoldMs);
  Serial.printf("startHoldMs: %lu ms\n", startHoldMs);
  Serial.printf("animStepMs: %lu ms\n", animStepMs);
  Serial.printf("fixedTimeOffsetSeconds: %ld s\n", fixedTimeOffsetSeconds);
  Serial.printf("ntpServer: %s\n", ntpServer.c_str());
  Serial.printf("defaultBrightness: %d\n", defaultBrightness);
  Serial.printf("quietHourStart: %d\n", quietHourStart);
  Serial.printf("quietHourEnd: %d\n", quietHourEnd);
  Serial.println("==== /CONFIG ====");
}

void loadConfigFromPrefs() {
  prefs.begin("config", true); // read-only
  client_id = prefs.getString("client_id", client_id);
  client_secret = prefs.getString("client_secret", client_secret);
  String s;
  s = prefs.getString("planePollInterval", String(planePollInterval)); planePollInterval = parseUL(s, planePollInterval);
  s = prefs.getString("landingDelayMs", String(landingDelayMs)); landingDelayMs = parseUL(s, landingDelayMs);
  s = prefs.getString("landingHoldMs", String(landingHoldMs)); landingHoldMs = parseUL(s, landingHoldMs);
  s = prefs.getString("startHoldMs", String(startHoldMs)); startHoldMs = parseUL(s, startHoldMs);
  s = prefs.getString("animStepMs", String(animStepMs)); animStepMs = parseUL(s, animStepMs); if (animStepMs < ANIM_MIN_STEP_MS) animStepMs = ANIM_MIN_STEP_MS;
  s = prefs.getString("fixedTimeOffsetSeconds", String(fixedTimeOffsetSeconds)); fixedTimeOffsetSeconds = parseLongVal(s, fixedTimeOffsetSeconds);
  s = prefs.getString("ntpServer", ntpServer); if (s.length() > 0) ntpServer = s;
  s = prefs.getString("defaultBrightness", String(defaultBrightness)); defaultBrightness = parseIntVal(s, defaultBrightness); defaultBrightness = constrain(defaultBrightness, 0, 255);
  s = prefs.getString("quietHourStart", String(quietHourStart)); quietHourStart = parseIntVal(s, quietHourStart); quietHourStart = constrain(quietHourStart, 0, 23);
  s = prefs.getString("quietHourEnd", String(quietHourEnd)); quietHourEnd = parseIntVal(s, quietHourEnd); quietHourEnd = constrain(quietHourEnd, 0, 23);
  prefs.end();
  printConfig();
}

// ---------- WiFiManager parameter handling ----------
void saveConfigCallback() {
  Serial.println("WiFiManager: saveConfigCallback aufgerufen -> speichere Parameter...");
  prefs.begin("config", false);

  if (p_client_id && String(p_client_id->getValue()).length() > 0) { prefs.putString("client_id", p_client_id->getValue()); client_id = p_client_id->getValue(); }
  if (p_client_secret && String(p_client_secret->getValue()).length() > 0) { prefs.putString("client_secret", p_client_secret->getValue()); client_secret = p_client_secret->getValue(); }
  if (p_poll && String(p_poll->getValue()).length() > 0) { prefs.putString("planePollInterval", p_poll->getValue()); planePollInterval = parseUL(p_poll->getValue(), planePollInterval); }
  if (p_landingDelay && String(p_landingDelay->getValue()).length() > 0) { prefs.putString("landingDelayMs", p_landingDelay->getValue()); landingDelayMs = parseUL(p_landingDelay->getValue(), landingDelayMs); }
  if (p_landingHold && String(p_landingHold->getValue()).length() > 0) { prefs.putString("landingHoldMs", p_landingHold->getValue()); landingHoldMs = parseUL(p_landingHold->getValue(), landingHoldMs); }
  if (p_startHold && String(p_startHold->getValue()).length() > 0) { prefs.putString("startHoldMs", p_startHold->getValue()); startHoldMs = parseUL(p_startHold->getValue(), startHoldMs); }
  if (p_animStep && String(p_animStep->getValue()).length() > 0) { prefs.putString("animStepMs", p_animStep->getValue()); animStepMs = parseUL(p_animStep->getValue(), animStepMs); if (animStepMs < ANIM_MIN_STEP_MS) animStepMs = ANIM_MIN_STEP_MS;}
  if (p_fixedOffset && String(p_fixedOffset->getValue()).length() > 0) { prefs.putString("fixedTimeOffsetSeconds", p_fixedOffset->getValue()); fixedTimeOffsetSeconds = parseLongVal(p_fixedOffset->getValue(), fixedTimeOffsetSeconds); }
  if (p_ntp && String(p_ntp->getValue()).length() > 0) { prefs.putString("ntpServer", p_ntp->getValue()); ntpServer = p_ntp->getValue(); }
  if (p_brightness && String(p_brightness->getValue()).length() > 0) { prefs.putString("defaultBrightness", p_brightness->getValue()); defaultBrightness = parseIntVal(p_brightness->getValue(), defaultBrightness); defaultBrightness = constrain(defaultBrightness, 0, 255); }
  if (p_quietStart && String(p_quietStart->getValue()).length() > 0) { prefs.putString("quietHourStart", p_quietStart->getValue()); quietHourStart = parseIntVal(p_quietStart->getValue(), quietHourStart); quietHourStart = constrain(quietHourStart, 0, 23); }
  if (p_quietEnd && String(p_quietEnd->getValue()).length() > 0) { prefs.putString("quietHourEnd", p_quietEnd->getValue()); quietHourEnd = parseIntVal(p_quietEnd->getValue(), quietHourEnd); quietHourEnd = constrain(quietHourEnd, 0, 23); }
  
  if (WiFi.status() == WL_CONNECTED && client_id.length() > 0 && client_secret.length() > 0) {
    prefs.putBool("wifiConfigured", true);
  } else {
    prefs.putBool("wifiConfigured", false);
  }
  prefs.end();
  printConfig();
}

void ensureWiFiManagerParams() {
  if (wmParamsInitialized) return;

  strncpy(buf_clientid, client_id.c_str(), sizeof(buf_clientid)-1); buf_clientid[sizeof(buf_clientid)-1] = '\0';
  strncpy(buf_clientsecret, client_secret.c_str(), sizeof(buf_clientsecret)-1); buf_clientsecret[sizeof(buf_clientsecret)-1] = '\0';
  snprintf(buf_poll, sizeof(buf_poll), "%lu", planePollInterval);
  snprintf(buf_landingDelay, sizeof(buf_landingDelay), "%lu", landingDelayMs);
  snprintf(buf_landingHold, sizeof(buf_landingHold), "%lu", landingHoldMs);
  snprintf(buf_startHold, sizeof(buf_startHold), "%lu", startHoldMs);
  snprintf(buf_animStep, sizeof(buf_animStep), "%lu", animStepMs);
  snprintf(buf_fixedOffset, sizeof(buf_fixedOffset), "%ld", fixedTimeOffsetSeconds);
  strncpy(buf_ntp, ntpServer.c_str(), sizeof(buf_ntp)-1); buf_ntp[sizeof(buf_ntp)-1] = '\0';
  snprintf(buf_brightness, sizeof(buf_brightness), "%d", defaultBrightness);
  snprintf(buf_quietStart, sizeof(buf_quietStart), "%d", quietHourStart);
  snprintf(buf_quietEnd, sizeof(buf_quietEnd), "%d", quietHourEnd);

  p_client_id = new WiFiManagerParameter("clientid", "OpenSky Client ID", buf_clientid, sizeof(buf_clientid));
  p_client_secret = new WiFiManagerParameter("clientsecret", "OpenSky Client Secret", buf_clientsecret, sizeof(buf_clientsecret));
  p_poll = new WiFiManagerParameter("poll", "Poll Interval (ms)", buf_poll, sizeof(buf_poll));
  p_landingDelay = new WiFiManagerParameter("landingDelay", "Landing delay (ms)", buf_landingDelay, sizeof(buf_landingDelay));
  p_landingHold = new WiFiManagerParameter("landingHold", "Landing hold (ms)", buf_landingHold, sizeof(buf_landingHold));
  p_startHold = new WiFiManagerParameter("startHold", "Start hold (ms)", buf_startHold, sizeof(buf_startHold));
  p_animStep = new WiFiManagerParameter("animStep", "Animation step (ms)", buf_animStep, sizeof(buf_animStep));
  p_fixedOffset = new WiFiManagerParameter("fixedOffset", "Time offset (sec)", buf_fixedOffset, sizeof(buf_fixedOffset));
  p_ntp = new WiFiManagerParameter("ntpServer", "NTP server", buf_ntp, sizeof(buf_ntp));
  p_brightness = new WiFiManagerParameter("brightness", "Brightness (0-255)", buf_brightness, sizeof(buf_brightness));
  p_quietStart = new WiFiManagerParameter("quietStart", "Ruhezeit Start (Stunde 0-23)", buf_quietStart, sizeof(buf_quietStart));
  p_quietEnd = new WiFiManagerParameter("quietEnd", "Ruhezeit Ende (Stunde 0-23)", buf_quietEnd, sizeof(buf_quietEnd));

  wm.addParameter(p_client_id);
  wm.addParameter(p_client_secret);
  wm.addParameter(p_poll);
  wm.addParameter(p_landingDelay);
  wm.addParameter(p_landingHold);
  wm.addParameter(p_startHold);
  wm.addParameter(p_animStep);
  wm.addParameter(p_fixedOffset);
  wm.addParameter(p_ntp);
  wm.addParameter(p_brightness);
  wm.addParameter(p_quietStart);
  wm.addParameter(p_quietEnd);

  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setDebugOutput(false);
  wmParamsInitialized = true;
}

// ---------- Captive-Portal / Config ----------
void startConfigPortalLoop() {
  while (true) {
    ensureWiFiManagerParams();
    portalActive = true;
    setStripColorAll(0,0,255);
    Serial.println("=== Captive Portal (RunwayFrame) geöffnet ===");
    bool connected = wm.startConfigPortal("RunwayFrame");
    
    // Callback speichert bereits, aber zur Sicherheit hier nochmal explizit die wichtigsten Werte setzen
    if (p_client_id) client_id = p_client_id->getValue();
    if (p_client_secret) client_secret = p_client_secret->getValue();

    if (WiFi.status() == WL_CONNECTED && client_id.length() > 0 && client_secret.length() > 0) {
      portalActive = false;
      Serial.println("WLAN verbunden und OpenSky-Creds gesetzt -> Portal beendet.");
      break;
    } else {
      portalActive = false;
      Serial.println("Portal beendet, aber noch keine vollständige Konfiguration. Starte Portal erneut...");
      delay(500);
    }
  }
}

// ---------- OpenSky (Token holen) ----------
bool fetchAccessToken() {
  if (WiFi.status() != WL_CONNECTED || client_id.length() == 0 || client_secret.length() == 0) return false;

  HTTPClient http;
  http.begin("https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData = "grant_type=client_credentials&client_id=" + client_id + "&client_secret=" + client_secret;
  int httpCode = http.POST(postData);
  String payload = http.getString();
  http.end();
  
  Serial.printf("Token-Request -> HTTP Code: %d\n", httpCode);
  
  if (httpCode == 400) {
    Serial.println("❌ Fehler HTTP Token: 400 (ungültige Credentials). Lösche Config und starte neu.");
    showErrorLED(255, 0, 0, 3000);
    prefs.begin("config", false);
    prefs.putBool("wifiConfigured", false);
    prefs.remove("client_id");
    prefs.remove("client_secret");
    prefs.end();
    WiFi.disconnect(true, true);
    delay(1000);
    ESP.restart();
    return false;
  }
  if (httpCode != 200) return false;

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, payload)) return false;

  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"].as<int>();
  tokenExpiresAt = millis() + (unsigned long)(max(30, expiresIn - 60)) * 1000UL;
  Serial.println("✅ Access Token erhalten!");
  return true;
}

// ---------- Plane fetch logic ----------
void handleBox(Box box, PlaneState &state, String side, bool isNorth, float lat, float lon, float track, float altitude, String callsign, bool onGround) {
  if (callsign == "" || onGround || altitude < 30 || wasRecentlyTriggered(callsign) || !isInside(box, lat, lon)) {
    return;
  }

  Serial.printf("✈️ Check %s @ %sBahn %s | Alt: %.1fm, Track: %.1f\n", callsign.c_str(), isNorth ? "Nord" : "Süd", side.c_str(), altitude, track);

  if (state.pendingPlane == "") {
    bool isLanding = false, isStart = false, isReversed = false;

    if (track >= 260 && track <= 280) { // Richtung Ost → West (Normal)
      if (side == "OST") { isLanding = true; } 
      else if (side == "WEST") { isStart = true; }
    } else if (track >= 80 && track <= 100) { // Richtung West → Ost (Reversed)
      isReversed = true;
      if (side == "WEST") { isLanding = true; } 
      else if (side == "OST") { isStart = true; }
    }

    if (isLanding || isStart) {
      state.pendingPlane = callsign;
      state.detectedAt = millis();
      state.landingTriggered = isLanding;
      state.startTriggered = isStart;
      state.reversedDirection = isReversed;
      addRecentPlane(callsign);
      Serial.printf("  ✅ %s erkannt: %s auf %sBahn (%s)\n", 
          isLanding ? "🛬 Landung" : "🛫 Start", 
          isReversed ? "West → Ost (reversed)" : "Ost → West (normal)",
          isNorth ? "Nord" : "Süd", 
          callsign.c_str());
    }
  }
}

void fetchPlanes() {
  if (WiFi.status() != WL_CONNECTED || accessToken == "") return;

  HTTPClient http;
  String url = "https://opensky-network.org/api/states/all?lamin=48.32&lomin=11.69&lamax=48.38&lomax=11.86";
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + accessToken);
  int httpCode = http.GET();
  
  if (httpCode <= 0) { http.end(); return; }
  String payload = http.getString();
  http.end();
  
  Serial.printf("Plane-Request -> HTTP Code: %d, Payload: %u bytes\n", httpCode, (unsigned int)payload.length());
  if (httpCode != 200) return;

  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, payload) || !doc.containsKey("states")) return;
  
  JsonArray states = doc["states"].as<JsonArray>();
  for (JsonArray f : states) {
    float lat = f[6].isNull() ? 0.0f : f[6].as<float>();
    float lon = f[5].isNull() ? 0.0f : f[5].as<float>();
    float track = f[10].isNull() ? -1.0f : f[10].as<float>();
    bool onGround = f[8].as<bool>();
    float altitude = f[13].isNull() ? (f[7].isNull() ? -1.0f : f[7].as<float>()) : f[13].as<float>();
    String callsign = f[1].as<String>();
    callsign.trim();

    handleBox(southRunwayEast, southEast, "OST", false, lat, lon, track, altitude, callsign, onGround);
    handleBox(southRunwayWest, southWest, "WEST", false, lat, lon, track, altitude, callsign, onGround);
    handleBox(northRunwayEast, northEast, "OST", true, lat, lon, track, altitude, callsign, onGround);
    handleBox(northRunwayWest, northWest, "WEST", true, lat, lon, track, altitude, callsign, onGround);
  }
}

// ---------- NTP / TZ Globals ----------
bool ntpConfigured = false;
int lastPrintedMinute = -1;

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.clear(); strip.show();

  for (int i = 0; i < 3; ++i) purpleFadeOnce(40, 8);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("RunwayFrame");

  loadConfigFromPrefs();

  prefs.begin("config", true);
  bool wifiConfigured = prefs.getBool("wifiConfigured", false);
  prefs.end();

  if (wifiConfigured) {
    Serial.println("Versuche Verbindung mit gespeichertem WLAN...");
    WiFi.begin();
    unsigned long start = millis();
    while (millis() - start < 30000 && WiFi.status() != WL_CONNECTED) { delay(200); }
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WLAN Connect Timeout -> öffne Captive Portal.");
      startConfigPortalLoop();
    }
  } else {
    Serial.println("Erstkonfiguration -> Captive Portal starten.");
    startConfigPortalLoop();
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("✅ WLAN verbunden: "); Serial.println(WiFi.SSID());
    animateWiFiConnected();
    wifiAnimDone = true;
    ensureWiFiManagerParams();
    wm.startWebPortal();
    Serial.print("Web-Portal gestartet auf IP: "); Serial.println(WiFi.localIP());
  }
}

// ---------- Loop ----------
void loop() {
  unsigned long now = millis();
  if (wmParamsInitialized) wm.process();

  if (Serial.available() && Serial.readStringUntil('\n').equalsIgnoreCase("wifi")) {
    Serial.println("Resetting WiFi config flag. Reboot to open portal.");
    prefs.begin("config", false);
    prefs.putBool("wifiConfigured", false);
    prefs.end();
    delay(100);
    ESP.restart();
  }
  
  struct tm timeinfo;
  bool timeAvailable = getLocalTime(&timeinfo, 500);
  bool inQuietHours = timeAvailable && isQuietHours(timeinfo);
  
  if (timeAvailable && timeinfo.tm_min != lastPrintedMinute) {
    Serial.printf("Zeit: %02d:%02d:%02d%s\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, inQuietHours ? " (Ruhezeit)" : "");
    lastPrintedMinute = timeinfo.tm_min;
    strip.setBrightness(inQuietHours ? 0 : constrain(defaultBrightness, 0, 255));
    if (inQuietHours) strip.show(); // Lichter explizit ausschalten
  }

  // Animation triggers - nur wenn keine Animation läuft UND keine Ruhezeit ist
  if (!animationRunning && !inQuietHours) {
    PlaneState* s = nullptr;
    int startLed = 0, endLed = 0;
    
    if (southEast.startTriggered || southWest.startTriggered) { s = southEast.startTriggered ? &southEast : &southWest; startLed = SOUTH_START_LED; endLed = SOUTH_END_LED; }
    else if (southEast.landingTriggered && now - southEast.detectedAt > landingDelayMs) { s = &southEast; startLed = SOUTH_START_LED; endLed = SOUTH_END_LED; }
    else if (southWest.landingTriggered && now - southWest.detectedAt > landingDelayMs) { s = &southWest; startLed = SOUTH_START_LED; endLed = SOUTH_END_LED; }
    else if (northEast.startTriggered || northWest.startTriggered) { s = northEast.startTriggered ? &northEast : &northWest; startLed = NORTH_START_LED; endLed = NORTH_END_LED; }
    else if (northEast.landingTriggered && now - northEast.detectedAt > landingDelayMs) { s = &northEast; startLed = NORTH_START_LED; endLed = NORTH_END_LED; }
    else if (northWest.landingTriggered && now - northWest.detectedAt > landingDelayMs) { s = &northWest; startLed = NORTH_START_LED; endLed = NORTH_END_LED; }

    if (s) {
      if (s->startTriggered) {
        if (s->reversedDirection) animateStartRangeReversed(startLed, endLed); else animateStartRange(startLed, endLed);
      } else if (s->landingTriggered) {
        if (s->reversedDirection) animateLandingRangeReversed(startLed, endLed); else animateLandingRange(startLed, endLed);
      }
      s->lastPlane = s->pendingPlane; s->pendingPlane = ""; s->landingTriggered = false; s->startTriggered = false;
    }
  }

  unsigned long timeout = 90000UL;
  if (southEast.pendingPlane != "" && now - southEast.detectedAt > timeout) { southEast.pendingPlane = ""; southEast.landingTriggered = false; southEast.startTriggered = false;}
  if (southWest.pendingPlane != "" && now - southWest.detectedAt > timeout) { southWest.pendingPlane = ""; southWest.landingTriggered = false; southWest.startTriggered = false;}
  if (northEast.pendingPlane != "" && now - northEast.detectedAt > timeout) { northEast.pendingPlane = ""; northEast.landingTriggered = false; northEast.startTriggered = false;}
  if (northWest.pendingPlane != "" && now - northWest.detectedAt > timeout) { northWest.pendingPlane = ""; northWest.landingTriggered = false; northWest.startTriggered = false;}

  if (accessToken == "" || now > tokenExpiresAt) {
    if (now - lastTokenAttempt > TOKEN_ATTEMPT_INTERVAL_MS) {
      lastTokenAttempt = now;
      fetchAccessToken();
    }
  }

  // Fetch plane data - NUR WENN KEINE RUHEZEIT IST
  if (accessToken != "" && now - lastPlaneCheck > planePollInterval && !inQuietHours) {
    lastPlaneCheck = now;
    fetchPlanes();
  }

  if (WiFi.status() == WL_CONNECTED && !ntpConfigured) {
    Serial.printf("Konfiguriere NTP mit Server: %s / Offset: %ld s\n", ntpServer.c_str(), fixedTimeOffsetSeconds);
    configTime(fixedTimeOffsetSeconds, 3600, ntpServer.c_str());
    ntpConfigured = true;
  }
}
