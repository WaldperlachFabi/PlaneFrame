#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

// ---------- WLAN ----------
const char* ssid = "x";
const char* password = "x";

// ---------- OpenSky ----------
const char* client_id = "x";
const char* client_secret = "x";
String accessToken = "";
unsigned long tokenExpiresAt = 0;

// ---------- LED ----------
#define LED_PIN 2
#define NUM_LEDS 60
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------- Runway Boxen ----------
struct Box {
  float nwLat, nwLon;
  float swLat, swLon;
  float seLat, seLon;
  float neLat, neLon;
};

Box southRunway = {48.350591,11.823452, 48.341976,11.825173, 48.345327,11.853362, 48.352096,11.851771};
Box northRunway = {48.370236,11.820893, 48.362405,11.822028, 48.365147,11.852725, 48.372215,11.851543};

// ---------- Tracking ----------
bool southLandingTriggered = false;
bool northLandingTriggered = false;
bool southStartTriggered   = false;
bool northStartTriggered   = false;

unsigned long southDetectedAt = 0;
unsigned long northDetectedAt = 0;

String lastSouthPlane = "";
String lastNorthPlane = "";
String pendingSouthPlane = "";
String pendingNorthPlane = "";

// ---------- Funktionen ----------
void connectWiFi() {
  Serial.print("🔌 Verbinde mit WLAN: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    Serial.print(".");
    delay(500);
  }
  if(WiFi.status() == WL_CONNECTED){
    Serial.println();
    Serial.println("✅ WLAN verbunden!");
    Serial.print("📡 IP-Adresse: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("❌ WLAN Verbindung fehlgeschlagen, neustarten...");
    ESP.restart();
  }
}

bool isInside(Box box, float lat, float lon){
  return lat >= min(box.swLat, box.nwLat) && lat <= max(box.neLat, box.seLat) &&
         lon >= min(box.nwLon, box.neLon) && lon <= max(box.swLon, box.seLon);
}

bool fetchAccessToken() {
  if(WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin("https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData = "grant_type=client_credentials&client_id=" + String(client_id) + "&client_secret=" + String(client_secret);
  int httpCode = http.POST(postData);

  if(httpCode != 200){
    Serial.print("❌ Fehler HTTP Token: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, payload);
  if(err){
    Serial.print("❌ JSON Fehler Token: ");
    Serial.println(err.c_str());
    return false;
  }

  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"].as<int>();
  tokenExpiresAt = millis() + (expiresIn - 60) * 1000;
  Serial.println("✅ Access Token erhalten!");
  return true;
}

void fetchPlanes() {
  if(WiFi.status() != WL_CONNECTED) return;
  if(accessToken == "") return;

  HTTPClient http;
  String url = "https://opensky-network.org/api/states/all?lamin=48.33&lomin=11.72&lamax=48.37&lomax=11.85";
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + accessToken);

  int code = http.GET();
  if(code != 200){
    Serial.print("❌ Fehler beim Abrufen: ");
    Serial.println(code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, payload);
  if(err){
    Serial.print("❌ JSON Fehler: ");
    Serial.println(err.c_str());
    return;
  }

  JsonArray states = doc["states"].as<JsonArray>();
  for(JsonArray f : states){
    float lat = f[6].as<float>();
    float lon = f[5].as<float>();
    float track = f[10].as<float>(); // Richtung
    String callsign = f[1].as<String>();
    callsign.trim();

    if(callsign != "") Serial.println("📄 Flugzeug: " + callsign);

    // ---------- Südbahn ----------
    if(isInside(southRunway, lat, lon)) {
      if(callsign != lastSouthPlane && track >= 60 && track <= 120){ // Richtung Osten -> START
        Serial.println("🛫 Start Südbahn durch " + callsign + " Animation: jetzt");
        southStartTriggered = true;
        pendingSouthPlane = callsign;
      }
      else if(callsign != lastSouthPlane && (track < 60 || track > 120)){ // Richtung Westen -> LANDUNG
        Serial.println("🛬 Landung Südbahn durch " + callsign + " Animation: in 20000 ms");
        southLandingTriggered = true;
        southDetectedAt = millis();
        pendingSouthPlane = callsign;
      }
    }

    // ---------- Nordbahn ----------
    if(isInside(northRunway, lat, lon)) {
      if(callsign != lastNorthPlane && track >= 240 && track <= 300){ // Richtung Westen -> LANDUNG
        Serial.println("🛬 Landung Nordbahn durch " + callsign + " Animation: in 20000 ms");
        northLandingTriggered = true;
        northDetectedAt = millis();
        pendingNorthPlane = callsign;
      }
      else if(callsign != lastNorthPlane && (track < 240 || track > 300)){ // Richtung Osten -> START
        Serial.println("🛫 Start Nordbahn durch " + callsign + " Animation: jetzt");
        northStartTriggered = true;
        pendingNorthPlane = callsign;
      }
    }
  }
}

void animate(int startLed, int endLed, uint32_t color, bool forward=true){
  if(forward){
    for(int i=startLed; i<=endLed; i++){
      strip.setPixelColor(i, color);
      strip.show();
      delay(30);
    }
  } else {
    for(int i=endLed; i>=startLed; i--){
      strip.setPixelColor(i, color);
      strip.show();
      delay(30);
    }
  }
  // Kein sofortiges Löschen!
}


// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.clear();
  strip.show();

  connectWiFi();
  fetchAccessToken();
}

// ---------- Loop ----------
unsigned long lastPlaneCheck = 0;

void loop() {
  if(millis() > tokenExpiresAt){
    fetchAccessToken();
  }

  if(millis() - lastPlaneCheck > 10000){
    lastPlaneCheck = millis();
    fetchPlanes();
  }

  // Landung Süd (rot, verzögert)
  if(southLandingTriggered && millis() - southDetectedAt > 20000){
    animate(30, 59, strip.Color(255,0,0), true);
    lastSouthPlane = pendingSouthPlane;
    southLandingTriggered = false;
  }

  // Start Süd (blau, sofort)
  if(southStartTriggered){
    animate(59, 30, strip.Color(0,0,255), false);
    lastSouthPlane = pendingSouthPlane;
    southStartTriggered = false;
  }

  // Landung Nord (rot, verzögert)
  if(northLandingTriggered && millis() - northDetectedAt > 20000){
    animate(0, 29, strip.Color(255,0,0), true);
    lastNorthPlane = pendingNorthPlane;
    northLandingTriggered = false;
  }

  // Start Nord (blau, sofort)
  if(northStartTriggered){
    animate(29, 0, strip.Color(0,0,255), false);
    lastNorthPlane = pendingNorthPlane;
    northStartTriggered = false;
  }
}
