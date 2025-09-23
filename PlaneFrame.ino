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
unsigned long planePollInterval   = 15000UL;
unsigned long landingDelayMs      = 22000UL;
unsigned long landingHoldMs       = 3000UL;
unsigned long startHoldMs         = 1500UL;
unsigned long animStepMs          = 30UL;
int           defaultBrightness   = 64;
long          fixedTimeOffsetSeconds = 2 * 3600L; // +2h
String        ntpServer           = "de.pool.ntp.org";

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
  bool reversedDirection = false; // NEU: Flag für umgekehrte Animation
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

// Purple fade in/out (smooth)
void purpleFadeOnce(int steps=40, int stepDelay=10) {
  // set purple color on all pixels, then change brightness
  uint8_t pr = 128, pg = 0, pb = 128;
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(pr, pg, pb));
  // fade in
  int maxB = constrain(defaultBrightness, 0, 255);
  for (int b = 0; b <= maxB; b += max(1, maxB/steps)) {
    strip.setBrightness(b);
    strip.show();
    delay(stepDelay);
  }
  // fade out
  for (int b = maxB; b >= 0; b -= max(1, maxB/steps)) {
    strip.setBrightness(b);
    strip.show();
    delay(stepDelay);
  }
  strip.clear();
  strip.show();
}

// ---------- Animationen ----------
void animateLandingRange(int startLed, int endLed) {
  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = endLed; i >= startLed; --i) {
    strip.setPixelColor(i, strip.Color(255,0,0)); // rot
    strip.show();
    delay(max((unsigned long)ANIM_MIN_STEP_MS, animStepMs));
  }
  delay(landingHoldMs);
  strip.clear(); strip.show();
  animationRunning = false;
}

void animateStartRange(int startLed, int endLed) {
  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = endLed; i >= startLed; --i) {
    strip.setPixelColor(i, strip.Color(0,0,255)); // blau
    strip.show();
    delay(max((unsigned long)ANIM_MIN_STEP_MS, animStepMs));
  }
  delay(startHoldMs);
  strip.clear(); strip.show();
  animationRunning = false;
}

// NEUE ANIMATIONSFUNKTIONEN (REVERSED)
void animateLandingRangeReversed(int startLed, int endLed) {
  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = startLed; i <= endLed; ++i) { // Richtung geändert
    strip.setPixelColor(i, strip.Color(255, 0, 0)); // rot
    strip.show();
    delay(max((unsigned long)ANIM_MIN_STEP_MS, animStepMs));
  }
  delay(landingHoldMs);
  strip.clear(); strip.show();
  animationRunning = false;
}

void animateStartRangeReversed(int startLed, int endLed) {
  animationRunning = true;
  strip.setBrightness(constrain(defaultBrightness, 0, 255));
  for (int i = startLed; i <= endLed; ++i) { // Richtung geändert
    strip.setPixelColor(i, strip.Color(0, 0, 255)); // blau
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
    strip.setPixelColor(i, strip.Color(0,255,0)); // grün
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
  if (v < INT_MIN) return def;
  if (v > INT_MAX) return def;
  return (int)v;
}

// ---------- Config load/save/print ----------
void printConfig() {
  Serial.println("==== CONFIG ====");
  Serial.printf("WiFiConfigured flag (prefs): %s\n", prefs.getBool("wifiConfigured", false) ? "true" : "false");
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
  Serial.println("==== /CONFIG ====");
}

void loadConfigFromPrefs() {
  prefs.begin("config", true); // read-only
  client_id = prefs.getString("client_id", client_id);
  client_secret = prefs.getString("client_secret", client_secret);

  // read numeric/string values (stored as strings to be robust)
  String s;

  s = prefs.getString("planePollInterval", String(planePollInterval));
  planePollInterval = parseUL(s, planePollInterval);

  s = prefs.getString("landingDelayMs", String(landingDelayMs));
  landingDelayMs = parseUL(s, landingDelayMs);

  s = prefs.getString("landingHoldMs", String(landingHoldMs));
  landingHoldMs = parseUL(s, landingHoldMs);

  s = prefs.getString("startHoldMs", String(startHoldMs));
  startHoldMs = parseUL(s, startHoldMs);

  s = prefs.getString("animStepMs", String(animStepMs));
  animStepMs = parseUL(s, animStepMs);
  if (animStepMs < ANIM_MIN_STEP_MS) animStepMs = ANIM_MIN_STEP_MS;

  s = prefs.getString("fixedTimeOffsetSeconds", String(fixedTimeOffsetSeconds));
  fixedTimeOffsetSeconds = parseLongVal(s, fixedTimeOffsetSeconds);

  s = prefs.getString("ntpServer", ntpServer);
  if (s.length() > 0) ntpServer = s;

  s = prefs.getString("defaultBrightness", String(defaultBrightness));
  defaultBrightness = parseIntVal(s, defaultBrightness);
  defaultBrightness = constrain(defaultBrightness, 0, 255);

  prefs.end();

  // print loaded values
  printConfig();
}

void saveConfigToPrefsIfNotEmpty(const char *key, const String &val) {
  if (val.length() == 0) return;
  prefs.begin("config", false);
  prefs.putString(key, val);
  prefs.end();
}

// ---------- WiFiManager parameter handling ----------
void saveConfigCallback() {
  Serial.println("WiFiManager: saveConfigCallback aufgerufen -> speichere Parameter...");
  prefs.begin("config", false);

  if (p_client_id) {
    String newClient = String(p_client_id->getValue());
    if (newClient.length() > 0) {
      prefs.putString("client_id", newClient);
      client_id = newClient;
      Serial.println("Client ID aus Portal übernommen (Callback).");
    }
  }
  if (p_client_secret) {
    String newSecret = String(p_client_secret->getValue());
    if (newSecret.length() > 0) {
      prefs.putString("client_secret", newSecret);
      client_secret = newSecret;
      Serial.println("Client Secret aus Portal übernommen (Callback).");
    }
  }
  if (p_poll) {
    String v = String(p_poll->getValue());
    if (v.length() > 0) {
      prefs.putString("planePollInterval", v);
      planePollInterval = parseUL(v, planePollInterval);
      Serial.printf("PollInterval gesetzt (Callback): %lu\n", planePollInterval);
    }
  }
  if (p_landingDelay) {
    String v = String(p_landingDelay->getValue());
    if (v.length() > 0) {
      prefs.putString("landingDelayMs", v);
      landingDelayMs = parseUL(v, landingDelayMs);
      Serial.printf("landingDelayMs gesetzt (Callback): %lu\n", landingDelayMs);
    }
  }
  if (p_landingHold) {
    String v = String(p_landingHold->getValue());
    if (v.length() > 0) {
      prefs.putString("landingHoldMs", v);
      landingHoldMs = parseUL(v, landingHoldMs);
      Serial.printf("landingHoldMs gesetzt (Callback): %lu\n", landingHoldMs);
    }
  }
  if (p_startHold) {
    String v = String(p_startHold->getValue());
    if (v.length() > 0) {
      prefs.putString("startHoldMs", v);
      startHoldMs = parseUL(v, startHoldMs);
      Serial.printf("startHoldMs gesetzt (Callback): %lu\n", startHoldMs);
    }
  }
  if (p_animStep) {
    String v = String(p_animStep->getValue());
    if (v.length() > 0) {
      prefs.putString("animStepMs", v);
      animStepMs = parseUL(v, animStepMs);
      if (animStepMs < ANIM_MIN_STEP_MS) animStepMs = ANIM_MIN_STEP_MS;
      Serial.printf("animStepMs gesetzt (Callback): %lu\n", animStepMs);
    }
  }
  if (p_fixedOffset) {
    String v = String(p_fixedOffset->getValue());
    if (v.length() > 0) {
      prefs.putString("fixedTimeOffsetSeconds", v);
      fixedTimeOffsetSeconds = parseLongVal(v, fixedTimeOffsetSeconds);
      Serial.printf("fixedTimeOffsetSeconds gesetzt (Callback): %ld\n", fixedTimeOffsetSeconds);
    }
  }
  if (p_ntp) {
    String v = String(p_ntp->getValue());
    if (v.length() > 0) {
      prefs.putString("ntpServer", v);
      ntpServer = v;
      Serial.printf("ntpServer gesetzt (Callback): %s\n", ntpServer.c_str());
    }
  }
  if (p_brightness) {
    String v = String(p_brightness->getValue());
    if (v.length() > 0) {
      prefs.putString("defaultBrightness", v);
      defaultBrightness = parseIntVal(v, defaultBrightness);
      defaultBrightness = constrain(defaultBrightness, 0, 255);
      Serial.printf("defaultBrightness gesetzt (Callback): %d\n", defaultBrightness);
    }
  }

  // Wenn WLAN verbunden ist und OpenSky-Creds gesetzt -> flag setzen
  if (WiFi.status() == WL_CONNECTED && client_id.length() > 0 && client_secret.length() > 0) {
    prefs.putBool("wifiConfigured", true);
    Serial.println("wifiConfigured flag gesetzt (Callback).");
  } else {
    prefs.putBool("wifiConfigured", false);
  }

  prefs.end();

  // Optional: print config kurz
  printConfig();
}

void ensureWiFiManagerParams() {
  if (wmParamsInitialized) return;

  // Fülle Buffers mit aktuellen Werten
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

  // allocate persistent parameters (mit new -> bleiben im RAM)
  p_client_id = new WiFiManagerParameter("clientid", "OpenSky Client ID", buf_clientid, sizeof(buf_clientid));
  p_client_secret = new WiFiManagerParameter("clientsecret", "OpenSky Client Secret", buf_clientsecret, sizeof(buf_clientsecret));
  p_poll = new WiFiManagerParameter("poll", "Poll Interval (ms)", buf_poll, sizeof(buf_poll));
  p_landingDelay = new WiFiManagerParameter("landingDelay", "Landing delay before animation (ms)", buf_landingDelay, sizeof(buf_landingDelay));
  p_landingHold = new WiFiManagerParameter("landingHold", "Landing hold (ms)", buf_landingHold, sizeof(buf_landingHold));
  p_startHold = new WiFiManagerParameter("startHold", "Start hold (ms)", buf_startHold, sizeof(buf_startHold));
  p_animStep = new WiFiManagerParameter("animStep", "Animation step delay (ms)", buf_animStep, sizeof(buf_animStep));
  p_fixedOffset = new WiFiManagerParameter("fixedOffset", "Fixed time offset (sec)", buf_fixedOffset, sizeof(buf_fixedOffset));
  p_ntp = new WiFiManagerParameter("ntpServer", "NTP server", buf_ntp, sizeof(buf_ntp));
  p_brightness = new WiFiManagerParameter("brightness", "LED Brightness (0-255)", buf_brightness, sizeof(buf_brightness));

  // add to WiFiManager
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

  // save-callback einmalig setzen
  wm.setSaveConfigCallback(saveConfigCallback);

  // kein Debug-Output
  wm.setDebugOutput(false);

  wmParamsInitialized = true;
}

// ---------- Captive-Portal / Config (angepasst, nutzt global wm) ----------
void startConfigPortalLoop() {
  // Diese Funktion blockiert solange, bis WiFi verbunden ist UND client_id + client_secret gesetzt sind.
  // Sie startet das Config-Portal wiederholt (gleiche Parameter auf globaler wm) und zeigt währenddessen blaue LEDs.
  while (true) {
    ensureWiFiManagerParams();

    // Zeige blau während Portal aktiv ist (blockierender Modus)
    portalActive = true;
    setStripColorAll(0,0,255);
    Serial.println("=== Captive Portal (RunwayFrame) geöffnet - bitte SSID/Passwort und OpenSky-Daten eintragen ===");

    // Startet das Config-Portal (blockierend). SSID ist "RunwayFrame".
    bool connected = wm.startConfigPortal("RunwayFrame");
    Serial.printf("wm: startConfigPortal returned: %d\n", connected);

    // Falls der Benutzer Werte eingegeben hat: übernehmen und speichern (falls nicht bereits durch callback)
    prefs.begin("config", false);
    if (p_client_id) {
      String newClient = String(p_client_id->getValue());
      if (newClient.length() > 0) {
        prefs.putString("client_id", newClient);
        client_id = newClient;
        Serial.println("Client ID aus Portal übernommen.");
      }
    }
    if (p_client_secret) {
      String newSecret = String(p_client_secret->getValue());
      if (newSecret.length() > 0) {
        prefs.putString("client_secret", newSecret);
        client_secret = newSecret;
        Serial.println("Client Secret aus Portal übernommen.");
      }
    }

    if (p_poll) {
      String v = String(p_poll->getValue());
      if (v.length() > 0) {
        prefs.putString("planePollInterval", v);
        planePollInterval = parseUL(v, planePollInterval);
        Serial.printf("PollInterval gesetzt: %lu\n", planePollInterval);
      }
    }
    if (p_landingDelay) {
      String v = String(p_landingDelay->getValue());
      if (v.length() > 0) {
        prefs.putString("landingDelayMs", v);
        landingDelayMs = parseUL(v, landingDelayMs);
        Serial.printf("landingDelayMs gesetzt: %lu\n", landingDelayMs);
      }
    }
    if (p_landingHold) {
      String v = String(p_landingHold->getValue());
      if (v.length() > 0) {
        prefs.putString("landingHoldMs", v);
        landingHoldMs = parseUL(v, landingHoldMs);
        Serial.printf("landingHoldMs gesetzt: %lu\n", landingHoldMs);
      }
    }
    if (p_startHold) {
      String v = String(p_startHold->getValue());
      if (v.length() > 0) {
        prefs.putString("startHoldMs", v);
        startHoldMs = parseUL(v, startHoldMs);
        Serial.printf("startHoldMs gesetzt: %lu\n", startHoldMs);
      }
    }
    if (p_animStep) {
      String v = String(p_animStep->getValue());
      if (v.length() > 0) {
        prefs.putString("animStepMs", v);
        animStepMs = parseUL(v, animStepMs);
        if (animStepMs < ANIM_MIN_STEP_MS) animStepMs = ANIM_MIN_STEP_MS;
        Serial.printf("animStepMs gesetzt: %lu\n", animStepMs);
      }
    }
    if (p_fixedOffset) {
      String v = String(p_fixedOffset->getValue());
      if (v.length() > 0) {
        prefs.putString("fixedTimeOffsetSeconds", v);
        fixedTimeOffsetSeconds = parseLongVal(v, fixedTimeOffsetSeconds);
        Serial.printf("fixedTimeOffsetSeconds gesetzt: %ld\n", fixedTimeOffsetSeconds);
      }
    }
    if (p_ntp) {
      String v = String(p_ntp->getValue());
      if (v.length() > 0) {
        prefs.putString("ntpServer", v);
        ntpServer = v;
        Serial.printf("ntpServer gesetzt: %s\n", ntpServer.c_str());
      }
    }
    if (p_brightness) {
      String v = String(p_brightness->getValue());
      if (v.length() > 0) {
        prefs.putString("defaultBrightness", v);
        defaultBrightness = parseIntVal(v, defaultBrightness);
        defaultBrightness = constrain(defaultBrightness, 0, 255);
        Serial.printf("defaultBrightness gesetzt: %d\n", defaultBrightness);
      }
    }

    // Wenn nach Portal-Aufruf WiFi verbunden ist UND creds vorhanden, setzen wir das Flag
    if (WiFi.status() == WL_CONNECTED && client_id.length() > 0 && client_secret.length() > 0) {
      prefs.putBool("wifiConfigured", true);
      prefs.end();
      portalActive = false;
      Serial.println("WLAN verbunden und OpenSky-Creds gesetzt -> Portal beendet.");
      break;
    } else {
      // nicht alles vorhanden / verbunden -> Portal erneut anzeigen
      prefs.putBool("wifiConfigured", false);
      prefs.end();
      portalActive = false;
      Serial.println("Portal beendet, aber noch keine vollständige Konfiguration. Starte Portal erneut...");
      delay(500);
      // loop erneut, Portal wird wieder geöffnet
    }
  }
}

// ---------- OpenSky (Token holen) ----------
bool fetchAccessToken() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (client_id.length() == 0 || client_secret.length() == 0) return false;

  HTTPClient http;
  http.begin("https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData = "grant_type=client_credentials&client_id=" + client_id + "&client_secret=" + client_secret;
  int httpCode = http.POST(postData);
  String payload = http.getString();

  Serial.printf("Token-Request -> HTTP Code: %d, Payload length: %u\n", httpCode, (unsigned int)payload.length());
  
  if (httpCode == 400) {
    Serial.println("❌ Fehler HTTP Token: 400 (ungültige Credentials)");
    http.end();

    // LEDs 3 Sekunden rot
    showErrorLED(255, 0, 0, 3000);

    // WLAN Config + client creds löschen und neustarten -> Captive Portal erscheint beim Boot
    prefs.begin("config", false);
    prefs.putBool("wifiConfigured", false);
    prefs.remove("client_id");
    prefs.remove("client_secret");
    prefs.end();

    WiFi.disconnect(true, true);
    delay(1000);
    ESP.restart(); // Neustart -> setup() öffnet Portal
    return false; // (unreachable)
  }

  if (httpCode != 200) {
    Serial.printf("❌ Fehler HTTP Token: %d\n", httpCode);
    http.end();
    return false;
  }

  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("❌ Fehler beim Parsen des Token-JSON: ");
    Serial.println(err.c_str());
    return false;
  }

  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"].as<int>();
  tokenExpiresAt = millis() + (unsigned long)(max(30, expiresIn - 60)) * 1000UL;
  Serial.println("✅ Access Token erhalten!");
  return true;
}

// ---------- Plane fetch ----------
// KOMPLETT ÜBERARBEITETE FUNKTION
void handleBox(Box box, PlaneState &state, String side, bool isNorth, float lat, float lon, float track, float altitude, String callsign, bool onGround) {
  if (callsign == "" || onGround) return;
  if (altitude >= 0 && altitude < 30) {
    return; // Zu niedrig, um relevant zu sein
  }
  if (wasRecentlyTriggered(callsign)) {
    return; // Kürzlich getriggert, um Dopplungen zu vermeiden
  }
  
  if (!isInside(box, lat, lon)) {
    return; // Flugzeug nicht in dieser Box
  }

  // Flugzeug ist in der Box, jetzt Richtung und Aktion prüfen
  Serial.printf("✈️ Check %s @ %sBahn %s | Lat: %.4f, Lon: %.4f, Alt: %.1f m, Track: %.1f\n",
                callsign.c_str(), isNorth ? "Nord" : "Süd", side.c_str(), lat, lon, altitude, track);

  if (state.pendingPlane == "") { // Nur triggern, wenn kein anderer Vorgang für diese Runway aktiv ist
    
    // Fall 1: Richtung Ost → West (Normal, Track ca. 260-280°)
    if (track >= 260 && track <= 280) {
      if (side == "OST") { // Flugzeug kommt aus dem Osten -> Landung
        state.pendingPlane = callsign;
        state.detectedAt = millis();
        state.landingTriggered = true;
        state.startTriggered = false;
        state.reversedDirection = false; // Normale Richtung
        addRecentPlane(callsign);
        Serial.printf("  ✅🛬 Landung erkannt: Ost → West (normal) auf %sBahn (%s)\n", isNorth ? "Nord" : "Süd", callsign.c_str());
      } else if (side == "WEST") { // Flugzeug fliegt nach Westen -> Start
        state.pendingPlane = callsign;
        state.detectedAt = millis();
        state.startTriggered = true;
        state.landingTriggered = false;
        state.reversedDirection = false; // Normale Richtung
        addRecentPlane(callsign);
        Serial.printf("  ✅🛫 Start erkannt: Ost → West (normal) auf %sBahn (%s)\n", isNorth ? "Nord" : "Süd", callsign.c_str());
      }
    } 
    // Fall 2: Richtung West → Ost (Reversed, Track ca. 80-100°)
    else if (track >= 80 && track <= 100) {
      if (side == "WEST") { // Flugzeug kommt aus dem Westen -> Landung
        state.pendingPlane = callsign;
        state.detectedAt = millis();
        state.landingTriggered = true;
        state.startTriggered = false;
        state.reversedDirection = true; // Umgekehrte Richtung
        addRecentPlane(callsign);
        Serial.printf("  ✅🛬 Landung erkannt: West → Ost (reversed) auf %sBahn (%s)\n", isNorth ? "Nord" : "Süd", callsign.c_str());
      } else if (side == "OST") { // Flugzeug fliegt nach Osten -> Start
        state.pendingPlane = callsign;
        state.detectedAt = millis();
        state.startTriggered = true;
        state.landingTriggered = false;
        state.reversedDirection = true; // Umgekehrte Richtung
        addRecentPlane(callsign);
        Serial.printf("  ✅🛫 Start erkannt: West → Ost (reversed) auf %sBahn (%s)\n", isNorth ? "Nord" : "Süd", callsign.c_str());
      }
    } else {
      Serial.printf("  ⏩ Track %.1f passt zu keiner definierten Richtung.\n", track);
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
  
  if (httpCode <= 0) {
    Serial.println("❌ HTTP GET Fehler: Verbindung fehlgeschlagen / Timeout.");
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  Serial.printf("Plane-Request -> HTTP Code: %d, Payload length: %u\n", httpCode, (unsigned int)payload.length());

  if (httpCode != 200) {
    Serial.printf("❌ HTTP GET returned non-200: %d\n", httpCode);
    return;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("❌ Fehler beim Parsen des Plane-JSON: ");
    Serial.println(err.c_str());
    return;
  }

  if (!doc.containsKey("states")) {
    Serial.println("❌ JSON enthält keinen 'states'-Eintrag.");
    return;
  }

  JsonArray states = doc["states"].as<JsonArray>();
  Serial.printf("🔎 Anzahl states: %u\n", (unsigned int)states.size());

  for (JsonArray f : states) {
    float lat = f[6].isNull() ? 0.0f : f[6].as<float>();
    float lon = f[5].isNull() ? 0.0f : f[5].as<float>();
    float track = f[10].isNull() ? -1.0f : f[10].as<float>();
    bool onGround = f[8].isNull() ? false : f[8].as<bool>();
    float altitude = f[13].isNull() ? (f[7].isNull() ? -1.0f : f[7].as<float>()) : f[13].as<float>();
    String callsign = f[1].isNull() ? "" : f[1].as<String>();
    callsign.trim();

    // alle Boxen abarbeiten
    handleBox(southRunwayEast, southEast, "OST", false, lat, lon, track, altitude, callsign, onGround);
    handleBox(southRunwayWest, southWest, "WEST", false, lat, lon, track, altitude, callsign, onGround);
    handleBox(northRunwayEast, northEast, "OST", true, lat, lon, track, altitude, callsign, onGround);
    handleBox(northRunwayWest, northWest, "WEST", true, lat, lon, track, altitude, callsign, onGround);
  }
}

// ---------- NTP / TZ Globals ----------
bool ntpConfigured = false;
int lastPrintedMinute = -1; // initial ungültig

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.clear(); strip.show();

  // Purple fade on boot (3 cycles)
  for (int i = 0; i < 3; ++i) purpleFadeOnce(40, 8);

  // WiFi basics
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("RunwayFrame"); // Hostname

  // load preferences (client creds + flag + config)
  loadConfigFromPrefs();

  // Wenn bereits als konfiguriert markiert -> versuche verbindung mit 30s Timeout
  prefs.begin("config", true);
  bool wifiConfigured = prefs.getBool("wifiConfigured", false);
  prefs.end();

  if (wifiConfigured) {
    Serial.println("Versuche Verbindung mit gespeichertem WLAN (30s Timeout)...");
    WiFi.begin(); // versucht letzte gespeicherte AP-Credentials
    unsigned long start = millis();
    while (millis() - start < 30000) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(200);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WLAN Connect Timeout -> öffne Captive Portal (blockierend)");
      // Für Fallback zur Erstkonfig nutzen wir die blockierende Variante
      ensureWiFiManagerParams();
      startConfigPortalLoop();
    } else {
      Serial.print("✅ WLAN verbunden: "); Serial.println(WiFi.SSID());
      // connected -> show green animation
      animateWiFiConnected();
      wifiAnimDone = true;

      // Param-Registrierung und dauerhaftes Web-Portal starten (non-blocking)
      ensureWiFiManagerParams();
      // Startet das nicht-blockierende Web-Portal (erreichbar über die lokale IP)
      wm.startWebPortal();
      Serial.print("Web-Portal gestartet — erreichbar unter IP: ");
      Serial.println(WiFi.localIP());
      // Hinweis: wir setzen portalActive nicht auf true, damit Animationen weiterlaufen
    }
  } else {
    // Erstkonfig -> portal forcieren (blockierend)
    Serial.println("Erstkonfiguration erkannt -> Captive Portal starten (blockierend)");
    ensureWiFiManagerParams();
    startConfigPortalLoop();
    // startConfigPortalLoop setzt wifiConfigured intern sobald verbunden + creds gesetzt
    if (WiFi.status() == WL_CONNECTED) {
      animateWiFiConnected();
      wifiAnimDone = true;
      // Web-Portal non-blocking zusätzlich starten, damit es später über IP erreichbar bleibt
      wm.startWebPortal();
      Serial.print("Web-Portal gestartet — erreichbar unter IP: ");
      Serial.println(WiFi.localIP());
    }
  }

  // Hinweis: NTP wird nach erfolgreichem WiFi-Connect in loop() initialisiert,
  // damit die gleiche Logik auch beim Reconnect greift.
}

// ---------- Quiet Hours (23–7 Uhr) ----------
bool isQuietHours(struct tm &timeinfo) {
  int h = timeinfo.tm_hour;
  return (h >= 23 || h < 7);
}

// ---------- Loop ----------
void loop() {
  unsigned long now = millis();

  // WiFiManager background processing (macht startWebPortal reaktionsfähig)
  if (wmParamsInitialized) {
    wm.process();
  }

  // Serial-Befehl abfangen: "wifi" -> erzwinge Portal beim nächsten Boot
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.equalsIgnoreCase("wifi")) {
      Serial.println("Resetting WiFi config flag. Reboot to open portal.");
      prefs.begin("config", false);
      prefs.putBool("wifiConfigured", false);
      prefs.end();
      delay(100);
      ESP.restart();
    }
  }
 
  // Zeit-Synchronisation & Quiet Hours
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 500)) { // 500ms timeout
    if (timeinfo.tm_min != lastPrintedMinute) {
      Serial.printf("Zeit: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      lastPrintedMinute = timeinfo.tm_min;

      if (isQuietHours(timeinfo)) {
        strip.setBrightness(0);
        strip.show();
      } else {
        strip.setBrightness(constrain(defaultBrightness, 0, 255));
      }
    }
  }

  // Animation triggers - nur ausführen, wenn keine andere Animation läuft und nicht in Quiet Hours
  if (!animationRunning && (lastPrintedMinute == -1 || !isQuietHours(timeinfo))) {
    // Priorität: Südbahn vor Nordbahn, Start vor Landung
    
    // --- Südbahn ---
    if (southEast.startTriggered || southWest.startTriggered) {
      PlaneState& s = southEast.startTriggered ? southEast : southWest;
      Serial.printf("➡️ Animation: Start Südbahn - %s\n", s.pendingPlane.c_str());
      if (s.reversedDirection) animateStartRangeReversed(SOUTH_START_LED, SOUTH_END_LED);
      else animateStartRange(SOUTH_START_LED, SOUTH_END_LED);
      s.lastPlane = s.pendingPlane; s.pendingPlane = ""; s.startTriggered = false;
    } 
    else if (southEast.landingTriggered && now - southEast.detectedAt > landingDelayMs) {
      Serial.printf("➡️ Animation: Landung Südbahn - %s\n", southEast.pendingPlane.c_str());
      if (southEast.reversedDirection) animateLandingRangeReversed(SOUTH_START_LED, SOUTH_END_LED);
      else animateLandingRange(SOUTH_START_LED, SOUTH_END_LED);
      southEast.lastPlane = southEast.pendingPlane; southEast.pendingPlane = ""; southEast.landingTriggered = false;
    }
    else if (southWest.landingTriggered && now - southWest.detectedAt > landingDelayMs) {
      Serial.printf("➡️ Animation: Landung Südbahn - %s\n", southWest.pendingPlane.c_str());
      if (southWest.reversedDirection) animateLandingRangeReversed(SOUTH_START_LED, SOUTH_END_LED);
      else animateLandingRange(SOUTH_START_LED, SOUTH_END_LED);
      southWest.lastPlane = southWest.pendingPlane; southWest.pendingPlane = ""; southWest.landingTriggered = false;
    }

    // --- Nordbahn (nur wenn Südbahn frei ist) ---
    else if (northEast.startTriggered || northWest.startTriggered) {
      PlaneState& n = northEast.startTriggered ? northEast : northWest;
      Serial.printf("➡️ Animation: Start Nordbahn - %s\n", n.pendingPlane.c_str());
      if (n.reversedDirection) animateStartRangeReversed(NORTH_START_LED, NORTH_END_LED);
      else animateStartRange(NORTH_START_LED, NORTH_END_LED);
      n.lastPlane = n.pendingPlane; n.pendingPlane = ""; n.startTriggered = false;
    }
    else if (northEast.landingTriggered && now - northEast.detectedAt > landingDelayMs) {
      Serial.printf("➡️ Animation: Landung Nordbahn - %s\n", northEast.pendingPlane.c_str());
      if (northEast.reversedDirection) animateLandingRangeReversed(NORTH_START_LED, NORTH_END_LED);
      else animateLandingRange(NORTH_START_LED, NORTH_END_LED);
      northEast.lastPlane = northEast.pendingPlane; northEast.pendingPlane = ""; northEast.landingTriggered = false;
    }
    else if (northWest.landingTriggered && now - northWest.detectedAt > landingDelayMs) {
      Serial.printf("➡️ Animation: Landung Nordbahn - %s\n", northWest.pendingPlane.c_str());
      if (northWest.reversedDirection) animateLandingRangeReversed(NORTH_START_LED, NORTH_END_LED);
      else animateLandingRange(NORTH_START_LED, NORTH_END_LED);
      northWest.lastPlane = northWest.pendingPlane; northWest.pendingPlane = ""; northWest.landingTriggered = false;
    }
  }

  // Check for expired pending states to prevent deadlocks (nach 90s)
  unsigned long timeout = 90000UL;
  if (southEast.pendingPlane != "" && now - southEast.detectedAt > timeout) { southEast.pendingPlane = ""; southEast.landingTriggered = false; southEast.startTriggered = false;}
  if (southWest.pendingPlane != "" && now - southWest.detectedAt > timeout) { southWest.pendingPlane = ""; southWest.landingTriggered = false; southWest.startTriggered = false;}
  if (northEast.pendingPlane != "" && now - northEast.detectedAt > timeout) { northEast.pendingPlane = ""; northEast.landingTriggered = false; northEast.startTriggered = false;}
  if (northWest.pendingPlane != "" && now - northWest.detectedAt > timeout) { northWest.pendingPlane = ""; northWest.landingTriggered = false; northWest.startTriggered = false;}


  // OpenSky Token Management
  if (accessToken == "" || now > tokenExpiresAt) {
    if (now - lastTokenAttempt > TOKEN_ATTEMPT_INTERVAL_MS) {
      Serial.println("Token abgelaufen oder nicht vorhanden, fordere neuen an...");
      lastTokenAttempt = now;
      if (!fetchAccessToken()) {
        Serial.println("Token-Anforderung fehlgeschlagen.");
      }
    }
  }

  // Fetch plane data periodically
  if (accessToken != "" && now - lastPlaneCheck > planePollInterval) {
    lastPlaneCheck = now;
    fetchPlanes();
  }

  // NTP/Time logic - wird einmalig nach WiFi-Connect ausgeführt
  if (WiFi.status() == WL_CONNECTED && !ntpConfigured) {
    Serial.printf("Konfiguriere NTP mit Server: %s / Offset: %ld s\n", ntpServer.c_str(), fixedTimeOffsetSeconds);
    configTime(fixedTimeOffsetSeconds, 3600, ntpServer.c_str()); // Offset, DaylightOffset, Server
    ntpConfigured = true;
  }
}
