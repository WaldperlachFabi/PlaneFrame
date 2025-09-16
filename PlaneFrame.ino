#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>   // https://github.com/tzapu/WiFiManager (ESP32-kompatibler Fork)
#include <Preferences.h>   // Für persistenten Flag & Speicherung client_id/secret

// ---------- Globals ----------
Preferences prefs;
bool portalActive = false;

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

const int DEFAULT_BRIGHTNESS = 128;

// ---------- Timing ----------
const unsigned long PLANE_POLL_INTERVAL = 10000;
const unsigned long LANDING_DELAY_MS    = 20000;
const unsigned long LANDING_HOLD_MS     = 3000;
const unsigned long START_HOLD_MS       = 1500;
const unsigned long ANIM_STEP_MS        = 30;

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
  strip.setBrightness(DEFAULT_BRIGHTNESS);
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
  for (int b = 0; b <= DEFAULT_BRIGHTNESS; b += max(1, DEFAULT_BRIGHTNESS/steps)) {
    strip.setBrightness(b);
    strip.show();
    delay(stepDelay);
  }
  // fade out
  for (int b = DEFAULT_BRIGHTNESS; b >= 0; b -= max(1, DEFAULT_BRIGHTNESS/steps)) {
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
  strip.setBrightness(DEFAULT_BRIGHTNESS);
  for (int i = endLed; i >= startLed; --i) {
    strip.setPixelColor(i, strip.Color(255,0,0)); // rot
    strip.show();
    delay(ANIM_STEP_MS);
  }
  delay(LANDING_HOLD_MS);
  strip.clear(); strip.show();
  animationRunning = false;
}

void animateStartRange(int startLed, int endLed) {
  animationRunning = true;
  strip.setBrightness(DEFAULT_BRIGHTNESS);
  for (int i = endLed; i >= startLed; --i) {
    strip.setPixelColor(i, strip.Color(0,0,255)); // blau
    strip.show();
    delay(ANIM_STEP_MS);
  }
  delay(START_HOLD_MS);
  strip.clear(); strip.show();
  animationRunning = false;
}

void animateWiFiConnected() {
  animationRunning = true;
  strip.setBrightness(DEFAULT_BRIGHTNESS);
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(0,255,0)); // grün
    strip.show();
    delay(20);
  }
  delay(500);
  strip.clear(); strip.show();
  animationRunning = false;
}

// ---------- Captive-Portal / Config ----------
void startConfigPortalLoop() {
  // Diese Funktion blockiert solange, bis WiFi verbunden ist UND client_id + client_secret gesetzt sind.
  // Sie startet das Captive Portal wiederholt (neue WiFiManager-Instanz pro Durchlauf),
  // und zeigt währenddessen blaue LEDs.
  while (true) {
    // lokale WiFiManager-Instanz (vermeidet Duplikate an Parametern beim mehrfachen Aufruf)
    WiFiManager wm;
    wm.setDebugOutput(false);
    // kein Config-Portal Timeout -> Portal bleibt offen bis User handelt
    // Falls deine WiFiManager-Fork setTitle unterstützt, kannst du das nutzen (häufig vorhanden)
    #if defined(WIFIMANAGER_HAVE_TITLE)
      wm.setTitle("RunwayFrame");
    #endif

    // Parameterfelder (Default = aktuelle Werte, kann leer sein)
    WiFiManagerParameter custom_client_id("clientid", "OpenSky Client ID", client_id.c_str(), 64);
    WiFiManagerParameter custom_client_secret("clientsecret", "OpenSky Client Secret", client_secret.c_str(), 128);
    wm.addParameter(&custom_client_id);
    wm.addParameter(&custom_client_secret);

    // Zeige blau während Portal aktiv ist
    portalActive = true;
    setStripColorAll(0,0,255);
    Serial.println("=== Captive Portal (RunwayFrame) geöffnet - bitte SSID/Passwort und OpenSky-Daten eintragen ===");

    // Startet das Config-Portal (blockierend). SSID ist "RunwayFrame".
    // startConfigPortal kehrt zurück sobald verbunden wurde (oder Benutzer das Portal schliesst).
    bool connected = wm.startConfigPortal("RunwayFrame");
    Serial.printf("wm: startConfigPortal returned: %d\n", connected);

    // Falls der Benutzer Werte eingegeben hat: übernehmen und speichern
    String newClient = String(custom_client_id.getValue());
    String newSecret = String(custom_client_secret.getValue());

    prefs.begin("config", false);
    if (newClient.length() > 0) {
      prefs.putString("client_id", newClient);
      client_id = newClient;
      Serial.println("Client ID aus Portal übernommen.");
    }
    if (newSecret.length() > 0) {
      prefs.putString("client_secret", newSecret);
      client_secret = newSecret;
      Serial.println("Client Secret aus Portal übernommen.");
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
    Serial.print("❌ Fehler HTTP Token: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, payload)) {
    Serial.println("❌ Fehler beim Parsen des Token-JSON");
    return false;
  }

  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"].as<int>();
  tokenExpiresAt = millis() + (unsigned long)(max(30, expiresIn - 60)) * 1000UL;
  Serial.println("✅ Access Token erhalten!");
  return true;
}

// ---------- Plane fetch ----------
unsigned long lastPlaneCheck = 0;

void handleBox(Box box, PlaneState &state, String side, bool isNorth, float lat, float lon, float track, float altitude, String callsign, bool onGround) {
  if (callsign == "" || onGround) return;
  if (altitude >= 0 && altitude < 30) {
    Serial.printf("   ⛔ %s ignoriert (%.1f m Höhe < 30 m)\n", callsign.c_str(), altitude);
    return;
  }
  if (wasRecentlyTriggered(callsign)) {
    Serial.printf("   ⛔ %s übersprungen (bereits in den letzten 5)\n", callsign.c_str());
    return;
  }

  // Debug-Log
  Serial.printf("✈️ Check %sBahn %s | %s @ Lat: %.6f, Lon: %.6f, Alt: %.1f m, Track: %.1f\n",
                isNorth ? "Nord" : "Süd", side.c_str(), callsign.c_str(), lat, lon, altitude, track);

  if (!isInside(box, lat, lon)) {
    Serial.printf("   ➡️ %s NICHT in Box %sBahn %s\n", callsign.c_str(), isNorth ? "Nord" : "Süd", side.c_str());
    return;
  }

  Serial.printf("   ✅ %s in Box %sBahn %s erkannt!\n", callsign.c_str(), isNorth ? "Nord" : "Süd", side.c_str());

  if (state.pendingPlane == "" && callsign != state.lastPlane) {
    if (side == "WEST") {
      if (track >= 60 && track <= 100) { // Landung von Westen Richtung Osten
        state.pendingPlane = callsign;
        state.detectedAt = millis();
        state.landingTriggered = true;
        addRecentPlane(callsign);
        Serial.printf("   🛬 Landung getriggert auf %sBahn WEST (%s)\n", isNorth ? "Nord" : "Süd", callsign.c_str());
      } else if (track >= 240 && track <= 280) { // Start nach Westen
        state.pendingPlane = callsign;
        state.startTriggered = true;
        addRecentPlane(callsign);
        Serial.printf("   🛫 Start getriggert auf %sBahn WEST (%s)\n", isNorth ? "Nord" : "Süd", callsign.c_str());
      } else {
        Serial.printf("   ⏩ Track %.1f passt NICHT für Start/Landung WEST\n", track);
      }
    } else { // OST
      if (track >= 240 && track <= 280) { // Landung von Osten Richtung Westen
        state.pendingPlane = callsign;
        state.detectedAt = millis();
        state.landingTriggered = true;
        addRecentPlane(callsign);
        Serial.printf("   🛬 Landung getriggert auf %sBahn OST (%s)\n", isNorth ? "Nord" : "Süd", callsign.c_str());
      } else if (track >= 60 && track <= 100) { // Start nach Osten
        state.pendingPlane = callsign;
        state.startTriggered = true;
        addRecentPlane(callsign);
        Serial.printf("   🛫 Start getriggert auf %sBahn OST (%s)\n", isNorth ? "Nord" : "Süd", callsign.c_str());
      } else {
        Serial.printf("   ⏩ Track %.1f passt NICHT für Start/Landung OST\n", track);
      }
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
  if (httpCode != 200) { http.end(); return; }
  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, payload)) return;

  JsonArray states = doc["states"].as<JsonArray>();

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

  // load preferences (client creds + flag)
  prefs.begin("config", false);
  bool wifiConfigured = prefs.getBool("wifiConfigured", false);
  String savedClient = prefs.getString("client_id", "");
  String savedSecret = prefs.getString("client_secret", "");
  prefs.end();

  if (savedClient.length() > 0) client_id = savedClient;
  if (savedSecret.length() > 0) client_secret = savedSecret;

  // Wenn bereits als konfiguriert markiert -> versuche verbindung mit 30s Timeout
  if (wifiConfigured) {
    Serial.println("Versuche Verbindung mit gespeichertem WLAN (30s Timeout)...");
    WiFi.begin(); // versucht letzte gespeicherte AP-Credentials
    unsigned long start = millis();
    while (millis() - start < 30000) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(200);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WLAN Connect Timeout -> öffne Captive Portal");
      startConfigPortalLoop();
    } else {
      Serial.print("✅ WLAN verbunden: "); Serial.println(WiFi.SSID());
      // connected -> show green animation
      animateWiFiConnected();
      wifiAnimDone = true;
    }
  } else {
    // Erstkonfig -> portal forcieren
    Serial.println("Erstkonfiguration erkannt -> Captive Portal starten");
    startConfigPortalLoop();
    // startConfigPortalLoop setzt wifiConfigured intern sobald verbunden + creds gesetzt
    if (WiFi.status() == WL_CONNECTED) {
      animateWiFiConnected();
      wifiAnimDone = true;
    }
  }
}

// ---------- Loop ----------
void loop() {
  unsigned long now = millis();

  // Serial-Befehl abfangen: "wifi" -> erzwinge Portal beim nächsten Boot
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.equalsIgnoreCase("wifi")) {
      Serial.println("🔄 Befehl 'wifi' empfangen -> WLAN-Reset & Captive Portal beim Neustart");
      prefs.begin("config", false);
      prefs.putBool("wifiConfigured", false);
      prefs.remove("client_id");
      prefs.remove("client_secret");
      prefs.end();

      WiFi.disconnect(true, true);  // WLAN trennen und Credentials löschen
      delay(1000);
      ESP.restart();                // Neustart -> Captive Portal
    }
  }

  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  bool tokenOK = (accessToken != "");

  // Token holen falls nötig (wenn WiFi OK und kein Token vorhanden)
  if (wifiOK && !tokenOK && now - lastTokenAttempt > TOKEN_ATTEMPT_INTERVAL_MS) {
    lastTokenAttempt = now;
    fetchAccessToken();
  }

  // Plane fetch loop
  if (wifiOK && tokenOK && now - lastPlaneCheck > PLANE_POLL_INTERVAL) {
    lastPlaneCheck = now;
    if (millis() >= tokenExpiresAt) {
      Serial.println("Token abgelaufen -> neu anfragen");
      accessToken = "";
    } else {
      fetchPlanes();
    }
  }

  // Animationen nur laufen lassen, wenn Portal nicht aktiv ist und WiFi verbunden ist
  if (!animationRunning && !portalActive && wifiOK) {
    // Süd
    if (southEast.startTriggered) {
      animateStartRange(SOUTH_START_LED, SOUTH_END_LED);
      southEast.lastPlane = southEast.pendingPlane; southEast.pendingPlane = ""; southEast.startTriggered = false;
    } else if (southEast.landingTriggered && millis() - southEast.detectedAt >= LANDING_DELAY_MS) {
      animateLandingRange(SOUTH_START_LED, SOUTH_END_LED);
      southEast.lastPlane = southEast.pendingPlane; southEast.pendingPlane = ""; southEast.landingTriggered = false;
    }
    if (southWest.startTriggered) {
      animateStartRange(SOUTH_START_LED, SOUTH_END_LED);
      southWest.lastPlane = southWest.pendingPlane; southWest.pendingPlane = ""; southWest.startTriggered = false;
    } else if (southWest.landingTriggered && millis() - southWest.detectedAt >= LANDING_DELAY_MS) {
      animateLandingRange(SOUTH_START_LED, SOUTH_END_LED);
      southWest.lastPlane = southWest.pendingPlane; southWest.pendingPlane = ""; southWest.landingTriggered = false;
    }

    // Nord
    if (northEast.startTriggered) {
      animateStartRange(NORTH_START_LED, NORTH_END_LED);
      northEast.lastPlane = northEast.pendingPlane; northEast.pendingPlane = ""; northEast.startTriggered = false;
    } else if (northEast.landingTriggered && millis() - northEast.detectedAt >= LANDING_DELAY_MS) {
      animateLandingRange(NORTH_START_LED, NORTH_END_LED);
      northEast.lastPlane = northEast.pendingPlane; northEast.pendingPlane = ""; northEast.landingTriggered = false;
    }
    if (northWest.startTriggered) {
      animateStartRange(NORTH_START_LED, NORTH_END_LED);
      northWest.lastPlane = northWest.pendingPlane; northWest.pendingPlane = ""; northWest.startTriggered = false;
    } else if (northWest.landingTriggered && millis() - northWest.detectedAt >= LANDING_DELAY_MS) {
      animateLandingRange(NORTH_START_LED, NORTH_END_LED);
      northWest.lastPlane = northWest.pendingPlane; northWest.pendingPlane = ""; northWest.landingTriggered = false;
    }
  }

  delay(10);
}
