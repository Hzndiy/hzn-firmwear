#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Firebase_ESP_Client.h> 
#include <time.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// ==================== FIREBASE & OTA CONFIGURATION ====================
#define API_KEY "AIzaSyB0Qye1YAJPL2QOMdaeKbEINLCTVAXZmk4"
#define PROJECT_ID "smart-home001" 
#define FIRMWARE_VERSION "1.0.2"

// Default GitHub Raw URL (Firestore တွင် လင့်ခ်မထည့်ထားပါက ဤလင့်ခ်ကို အသုံးပြုမည်)
const String DEFAULT_OTA_URL = "https://raw.githubusercontent.com/Hzndiy/hzn-firmwear/refs/heads/main/firmwear.bin";

#define CHANNEL_COUNT 8
#define BUTTON_COUNT 9
#define RELAY_ACTIVE_LOW true 
#define AP_TIMEOUT_MS 60000 

// ==================== PIN DEFINITIONS (ESP32-S3 Safe Pins) ====================
const int relayPins[CHANNEL_COUNT] = {4, 5, 6, 7, 15, 16, 17, 18};
const int btnPins[8]              = {8, 9, 10, 11, 12, 13, 14, 21};
const int masterBtnPin            = 1; 
const int statusLedPin            = 2;

// ==================== SYSTEM VARIABLES ====================
bool relayState[CHANNEL_COUNT]   = {false, false, false, false, false, false, false, false};
String powerMode[CHANNEL_COUNT]  = {"last", "last", "last", "last", "last", "last", "last", "last"};
bool pendingCloudSync            = false;
bool firebaseRegistered          = false;
bool ntpInitialized              = false;

// OTA Update Variables
bool pendingOTA                  = false;
String targetOtaUrl              = "";

bool buttonLast[BUTTON_COUNT]        = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
bool buttonStable[BUTTON_COUNT]      = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
uint32_t buttonChangedAt[BUTTON_COUNT] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
const uint32_t DEBOUNCE_MS         = 30;

// Recovery State Variable
String powerRecoveryState = "last";
bool bootRecoveryApplied  = false;

Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

String savedSSID = "";
String savedPASS = "";
String deviceID  = "";

enum WifiModeState { WIFI_CONNECTING, WIFI_CONNECTED, WIFI_AP_MODE, WIFI_OFF_IDLE };
WifiModeState currentWifiState = WIFI_OFF_IDLE;

unsigned long stateStartTime = 0;
unsigned long lastBlinkTime  = 0;
bool ledState = LOW;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool firebaseInitialized = false;
bool deviceProvisioned   = false;
unsigned long lastFirestorePoll = 0;
bool pollStepTimers = false; // Main doc နဲ့ Timers ကို တစ်လှည့်စီ စစ်ရန်

// Multiple Schedules Struct
struct ScheduleItem {
  int switchNum;
  String startTime;
  String endTime;
  bool active;
};
ScheduleItem activeSchedules[10];
int scheduleCount = 0;

// Subcollection Active Timers Struct (Using 64-bit integer for Milliseconds)
struct ActiveTimer {
  String docPath;       // Timer document path ကို မှတ်ထားရန် (ပြန်ပိတ်ပေးဖို့အတွက်)
  int channels[8];
  int channelCount;
  uint64_t endTimeMs;   // ဂဏန်း ၁၃ လုံး ဆံ့စေရန် uint64_t သုံးထားပါသည်
  bool isActive;
};
ActiveTimer activeTimers[10];
int activeTimerCount = 0;

// Function Prototypes
void startAPMode();
void stopAPMode();
void turnOffWiFiIdle();
void startWiFiConnection();
void initFirebaseIfNeeded();
void checkAndProvisionDevice();
void pushLocalStatesToFirestore();
void parseFirestoreStates(String payload);
void parseFirestoreTimers(String payload);
void syncSystemTime();
void checkTimersAndSchedules();
void checkActiveTimers();
void handlePhysicalButtons();
void performOTAUpdate(String firmwareUrl);

String getUniqueDeviceID() {
  uint64_t chipid = ESP.getEfuseMac();
  uint32_t uniqueNum = (uint32_t)(chipid >> 16) % 1000000;
  if (uniqueNum == 0) {
    uniqueNum = (uint32_t)(chipid & 0xFFFFFF) % 1000000;
  }
  char idBuffer[15];
  sprintf(idBuffer, "SH-%06u", uniqueNum);
  return String(idBuffer);
}

void applyRelayOutput(int index, bool state) {
  if (index < 0 || index >= CHANNEL_COUNT) return;
  relayState[index] = state;
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(relayPins[index], state ? LOW : HIGH);
  } else {
    digitalWrite(relayPins[index], state ? HIGH : LOW);
  }
}

// ==================== WEB PORTAL HTML ====================
void handleRoot() {
  stateStartTime = millis();
  int n = WiFi.scanNetworks();
  String wifiListHtml = "";
  for (int i = 0; i < n; ++i) {
    String ssidStr = WiFi.SSID(i);
    wifiListHtml += "<div class='wifi-item' onclick=\"selectWifi('" + ssidStr + "')\">";
    wifiListHtml += "<span><b>" + ssidStr + "</b></span>";
    wifiListHtml += "<span style='font-size:12px; color:#555;'>&#128274; " + String(WiFi.RSSI(i)) + " dBm</span>";
    wifiListHtml += "</div>";
  }

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Sign in to Smart Home Setup</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;background:#fff;color:#222;margin:0;padding:15px;}";
  html += ".container{max-width:400px;margin:auto;text-align:left;}";
  html += ".header-title{font-size:16px;font-weight:500;margin-bottom:2px;}";
  html += ".sub-ip{font-size:14px;color:#555;margin-bottom:20px;}";
  html += ".copy-box{text-align:center;margin:15px 0 5px 0;}";
  html += ".copy-btn{background:#0099ff;color:#000;font-weight:bold;padding:6px 14px;font-size:18px;cursor:pointer;display:inline-block;border-radius:4px;}";
  html += ".id-display{font-size:24px;color:#1a73e8;text-align:center;margin:10px 0 25px 0;font-weight:bold;}";
  html += ".wifi-list{max-height:220px;overflow-y:auto;margin-bottom:20px;}";
  html += ".wifi-item{padding:12px 4px;border-bottom:1px solid #f0f0f0;cursor:pointer;display:flex;justify-content:space-between;align-items:center;font-size:15px;}";
  html += "label{font-size:14px;color:#444;display:block;margin-top:10px;}";
  html += "input[type='text'], input[type='password']{width:100%;padding:10px;margin:6px 0 12px 0;border-radius:4px;border:1px solid #888;font-size:15px;box-sizing:border-box;}";
  html += ".action-btn{background:#1a8cff;color:white;border:none;padding:12px;border-radius:6px;font-size:16px;width:100%;cursor:pointer;margin-top:12px;}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<div class='header-title'>Sign in to Smart Home Setup</div>";
  html += "<div class='sub-ip'>" + WiFi.softAPIP().toString() + "</div>";
  html += "<div class='copy-box'><span class='copy-btn' onclick='copyId()'>COPY ID</span></div>";
  html += "<div class='id-display' id='devId'>" + deviceID + "</div>";
  html += "<div class='wifi-list'>" + (n == 0 ? "<div style='padding:10px; text-align:center;'>No networks found</div>" : wifiListHtml) + "</div>";
  html += "<form action='/save' method='POST'>";
  html += "<label>SSID</label>";
  html += "<input type='text' id='ssidInput' name='ssid' required>";
  html += "<label>Password</label>";
  html += "<input type='password' id='passInput' name='pass'>";
  html += "<button type='submit' class='action-btn'>Save</button>";
  html += "</form>";
  html += "</div><script>";
  html += "function selectWifi(s){document.getElementById('ssidInput').value = s;}";
  html += "function copyId(){";
  html += "  var t = document.getElementById('devId').innerText;";
  html += "  if (navigator.clipboard && navigator.clipboard.writeText) {";
  html += "    navigator.clipboard.writeText(t).then(function() {";
  html += "      alert('Copied Device ID: ' + t);";
  html += "    }).catch(function() {";
  html += "      fallbackCopy(t);";
  html += "    });";
  html += "  } else {";
  html += "    fallbackCopy(t);";
  html += "  ";
  html += "}";
  html += "function fallbackCopy(text) {";
  html += "  var textArea = document.createElement('textarea');";
  html += "  textArea.value = text;";
  html += "  textArea.style.position = 'fixed';";
  html += "  document.body.appendChild(textArea);";
  html += "  textArea.focus();";
  html += "  textArea.select();";
  html += "  try {";
  html += "    var successful = document.execCommand('copy');";
  html += "    if (successful) { alert('Copied Device ID: ' + text); }";
  html += "    else { alert('Copy failed. Please select manually.'); }";
  html += "  } catch (err) {";
  html += "    alert('Browser not supported copy. Please select manually.');";
  html += "  }";
  html += "  document.body.removeChild(textArea);";
  html += "}";
  html += "</script></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String newSsid = server.arg("ssid");
  String newPass = server.arg("pass");
  if (newSsid.length() > 0) {
    preferences.begin("wifi_config", false);
    preferences.putString("ssid", newSsid);
    preferences.putString("pass", newPass);
    preferences.end();
    server.send(200, "text/html", "<h2>Wi-Fi Saved Successfully! Connecting...</h2>");
    delay(1000);
    stopAPMode();
    savedSSID = newSsid;
    savedPASS = newPass;
    startWiFiConnection();
  }
}

void startAPMode() {
  if (currentWifiState == WIFI_AP_MODE) return; 
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  String apName = "SmartHome_" + deviceID;
  WiFi.softAP(apName.c_str(), "12345678");
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();
  currentWifiState = WIFI_AP_MODE;
  stateStartTime = millis();
}

void stopAPMode() {
  dnsServer.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
}

void turnOffWiFiIdle() {
  stopAPMode();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  digitalWrite(statusLedPin, LOW);
  currentWifiState = WIFI_OFF_IDLE;
}

void startWiFiConnection() {
  if (savedSSID == "") { startAPMode(); return; }
  if (currentWifiState == WIFI_CONNECTING) return;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
  currentWifiState = WIFI_CONNECTING;
  stateStartTime = millis();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n[SYSTEM] Booting HZN Smart Home (Firmware v%s)...\n", FIRMWARE_VERSION);

  for (int i = 0; i < CHANNEL_COUNT; i++) {
    pinMode(relayPins[i], OUTPUT);
    applyRelayOutput(i, false);
  }

  for (int i = 0; i < 8; i++) {
    pinMode(btnPins[i], INPUT_PULLUP);
  }
  pinMode(masterBtnPin, INPUT_PULLUP);
  pinMode(statusLedPin, OUTPUT);
  digitalWrite(statusLedPin, LOW);

  deviceID = getUniqueDeviceID();

  preferences.begin("wifi_config", true);
  savedSSID = preferences.getString("ssid", "");
  savedPASS = preferences.getString("pass", "");
  preferences.end();

  if (savedSSID != "") {
    startWiFiConnection();
  } else {
    startAPMode();
  }
}

void handlePhysicalButtons() {
  uint32_t nowMs = millis();

  for (int i = 0; i < 8; i++) {
    bool raw = digitalRead(btnPins[i]);
    if (raw != buttonLast[i]) {
      buttonLast[i] = raw;
      buttonChangedAt[i] = nowMs;
    }
    if ((nowMs - buttonChangedAt[i]) > DEBOUNCE_MS) {
      if (raw != buttonStable[i]) {
        buttonStable[i] = raw;
        if (buttonStable[i] == LOW) { 
          bool newState = !relayState[i];
          applyRelayOutput(i, newState);
          pendingCloudSync = true;
          Serial.printf("[BUTTON] CH%d toggled locally to %s\n", i + 1, newState ? "ON" : "OFF");
        }
      }
    }
  }

  static uint32_t masterPressStartTime = 0;
  static bool masterLongPressTriggered = false;
  
  bool masterRaw = digitalRead(masterBtnPin);
  
  if (masterRaw != buttonLast[8]) {
    buttonLast[8] = masterRaw;
    buttonChangedAt[8] = nowMs;
  }

  if ((nowMs - buttonChangedAt[8]) > DEBOUNCE_MS) {
    if (masterRaw != buttonStable[8]) {
      buttonStable[8] = masterRaw;
      
      if (buttonStable[8] == LOW) {
        masterPressStartTime = nowMs;
        masterLongPressTriggered = false;
      } else {
        if (!masterLongPressTriggered) {
          bool anyOn = false;
          for (int i = 0; i < CHANNEL_COUNT; i++) {
            if (relayState[i]) { anyOn = true; break; }
          }
          bool target = !anyOn;
          for (int i = 0; i < CHANNEL_COUNT; i++) {
            applyRelayOutput(i, target);
          }
          pendingCloudSync = true;
          Serial.printf("[MASTER] All channels set to %s\n", target ? "ON" : "OFF");
        }
      }
    }

    if (buttonStable[8] == LOW && !masterLongPressTriggered) {
      if (nowMs - masterPressStartTime >= 5000) {
        masterLongPressTriggered = true;
        Serial.println("[SYSTEM] Master Button Long Pressed (5s)! Clearing Wi-Fi & Starting AP Mode...");
        
        preferences.begin("wifi_config", false);
        preferences.clear();
        preferences.end();
        
        savedSSID = "";
        savedPASS = "";
        
        startAPMode();
      }
    }
  }
}

void syncSystemTime() {
  Serial.println("[TIME] Synchronizing system clock with NTP...");
  configTime(23400, 0, "pool.ntp.org", "time.google.com");
  time_t now = time(nullptr);
  unsigned long start = millis();
  while (now < 1700000000 && (millis() - start) < 10000) {
    handlePhysicalButtons();
    delay(100);
    now = time(nullptr);
  }
  ntpInitialized = true;
  Serial.printf("[TIME] Synced Time: %s", ctime(&now));
}

void handleWifiAndLedState() {
  if (currentWifiState == WIFI_AP_MODE) {
    dnsServer.processNextRequest();
    server.handleClient();
    if ((millis() - lastBlinkTime) >= 300) {
      lastBlinkTime = millis();
      ledState = !ledState;
      digitalWrite(statusLedPin, ledState);
    }
    if (WiFi.softAPgetStationNum() == 0 && ((millis() - stateStartTime) >= AP_TIMEOUT_MS)) {
      turnOffWiFiIdle();
    }
  } 
  else if (currentWifiState == WIFI_CONNECTING) {
    if ((millis() - lastBlinkTime) >= 500) {
      lastBlinkTime = millis();
      ledState = !ledState;
      digitalWrite(statusLedPin, ledState);
    }
    if (WiFi.status() == WL_CONNECTED) {
      digitalWrite(statusLedPin, HIGH);
      currentWifiState = WIFI_CONNECTED;
      WiFi.setSleep(false);
      syncSystemTime();
      initFirebaseIfNeeded();
    } 
    else if ((millis() - stateStartTime) >= 20000) {
      startAPMode();
    }
  } 
  else if (currentWifiState == WIFI_CONNECTED) {
    digitalWrite(statusLedPin, HIGH);
    if (WiFi.status() != WL_CONNECTED) {
      startWiFiConnection();
    }
  } 
  else if (currentWifiState == WIFI_OFF_IDLE) {
    digitalWrite(statusLedPin, LOW);
  }
}

void initFirebaseIfNeeded() {
  if (!firebaseInitialized) {
    config.api_key = API_KEY;
    config.timeout.serverResponse = 3000;
    config.timeout.socketConnection = 3000;
    config.timeout.sslHandshake = 3000;
    config.signer.test_mode = true;
    fbdo.setBSSLBufferSize(4096, 4096);
    fbdo.setResponseSize(4096);
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    firebaseInitialized = true;
  }
  checkAndProvisionDevice();
}

void checkAndProvisionDevice() {
  if (!Firebase.ready()) return;
  String docPath = "devices/" + deviceID;

  if (Firebase.Firestore.getDocument(&fbdo, PROJECT_ID, "", docPath.c_str())) {
    parseFirestoreStates(fbdo.payload());
    deviceProvisioned = true;
    
    FirebaseJson updateVersion;
    updateVersion.set("fields/isOnline/booleanValue", true);
    updateVersion.set("fields/firmwareVersion/stringValue", FIRMWARE_VERSION);
    Firebase.Firestore.patchDocument(&fbdo, PROJECT_ID, "", docPath.c_str(), updateVersion.raw(), "isOnline,firmwareVersion");
  } else {
    FirebaseJson content;
    content.set("fields/isOnline/booleanValue", true);
    content.set("fields/firmwareVersion/stringValue", FIRMWARE_VERSION);
    content.set("fields/startOta/booleanValue", false);
    content.set("fields/firmwareUrl/stringValue", DEFAULT_OTA_URL);
    content.set("fields/resetWifi/booleanValue", false);
    for (int i = 1; i <= CHANNEL_COUNT; i++) {
      content.set("fields/ch" + String(i) + "/booleanValue", relayState[i - 1]);
      content.set("fields/name" + String(i) + "/stringValue", "Channel " + String(i));
    }
    content.set("fields/powerRecoveryState/stringValue", "last");
    Firebase.Firestore.createDocument(&fbdo, PROJECT_ID, "", docPath.c_str(), content.raw());
    deviceProvisioned = true;
  }
}

void pushLocalStatesToFirestore() {
  if (currentWifiState != WIFI_CONNECTED || !Firebase.ready()) return;
  String docPath = "devices/" + deviceID;
  FirebaseJson content;
  String updateMask = "isOnline,firmwareVersion";
  content.set("fields/isOnline/booleanValue", true);
  content.set("fields/firmwareVersion/stringValue", FIRMWARE_VERSION);

  for (int i = 1; i <= CHANNEL_COUNT; i++) {
    content.set("fields/ch" + String(i) + "/booleanValue", relayState[i - 1]);
    updateMask += ",ch" + String(i);
  }
  Firebase.Firestore.patchDocument(&fbdo, PROJECT_ID, "", docPath.c_str(), content.raw(), updateMask.c_str());
}

void parseFirestoreStates(String payload) {
  FirebaseJson json;
  FirebaseJsonData result;
  json.setJsonData(payload);
  String docPath = "devices/" + deviceID;

  for (int i = 1; i <= CHANNEL_COUNT; i++) {
    json.get(result, "fields/ch" + String(i) + "/booleanValue");
    if (result.success) {
      bool cloudState = result.to<bool>();
      if (cloudState != relayState[i - 1]) {
        Serial.printf("[Cloud] CH%d changed from cloud to: %s\n", i, cloudState ? "ON" : "OFF");
        applyRelayOutput(i - 1, cloudState);
      }
    }
  }

  // ==================== RESET WIFI CHECK ====================
  if (json.get(result, "fields/resetWifi/booleanValue") && result.success) {
    bool triggerResetWifi = result.to<bool>();
    if (triggerResetWifi) {
      Serial.println("[WIFI] Reset Wi-Fi requested from App! Clearing Wi-Fi & Starting AP Mode...");
      
      FirebaseJson wifiResetJson;
      wifiResetJson.set("fields/resetWifi/booleanValue", false);
      Firebase.Firestore.patchDocument(&fbdo, PROJECT_ID, "", docPath.c_str(), wifiResetJson.raw(), "resetWifi");
      
      preferences.begin("wifi_config", false);
      preferences.clear();
      preferences.end();
      
      savedSSID = "";
      savedPASS = "";
      
      startAPMode();
      return;
    }
  }

  // Parse Power-On Recovery State
  json.get(result, "fields/powerRecoveryState/stringValue");
  if (result.success) {
    powerRecoveryState = result.to<String>();
    if (!bootRecoveryApplied) {
      if (powerRecoveryState == "off") {
        for (int i = 0; i < CHANNEL_COUNT; i++) {
          applyRelayOutput(i, false);
        }
        pendingCloudSync = true;
      } else if (powerRecoveryState == "on") {
        for (int i = 0; i < CHANNEL_COUNT; i++) {
          applyRelayOutput(i, true);
        }
        pendingCloudSync = true;
      }
      bootRecoveryApplied = true;
    }
  }

  // ==================== OTA UPDATE CHECK ====================
  String cloudOtaUrl = "";
  if (json.get(result, "fields/firmwareUrl/stringValue") && result.success) {
    cloudOtaUrl = result.to<String>();
  } else if (json.get(result, "fields/firmwareUlr/stringValue") && result.success) {
    cloudOtaUrl = result.to<String>();
  }

  if (json.get(result, "fields/startOta/booleanValue") && result.success) {
    bool triggerOta = result.to<bool>();
    if (triggerOta) {
      targetOtaUrl = (cloudOtaUrl.length() > 10) ? cloudOtaUrl : DEFAULT_OTA_URL;
      pendingOTA = true;
    }
  }

  // Parse Multiple Schedules from Array (schedules)
  scheduleCount = 0;
  FirebaseJsonArray arr;
  if (json.get(result, "fields/schedules/arrayValue/values") && result.success) {
    result.get(arr);
    size_t len = arr.size();
    for (size_t i = 0; i < len && i < 10; i++) {
      FirebaseJsonData itemRes;
      if (arr.get(itemRes, i) && itemRes.success) {
        FirebaseJson obj;
        obj.setJsonData(itemRes.to<String>());
        
        FirebaseJsonData subRes;
        int swNum = 1;
        String sTime = "";
        String eTime = "";
        bool sActive = true;

        if (obj.get(subRes, "mapValue/fields/switch/integerValue")) swNum = subRes.to<int>();
        if (obj.get(subRes, "mapValue/fields/start/stringValue")) sTime = subRes.to<String>();
        if (obj.get(subRes, "mapValue/fields/end/stringValue")) eTime = subRes.to<String>();
        if (obj.get(subRes, "mapValue/fields/active/booleanValue")) sActive = subRes.to<bool>();

        if (sActive && sTime.length() > 0) {
          activeSchedules[scheduleCount++] = {swNum, sTime, eTime, sActive};
        }
      }
    }
  }
}

// Parse Subcollection Timers (`devices/{deviceId}/timers`)
void parseFirestoreTimers(String payload) {
  FirebaseJson json;
  FirebaseJsonData result;
  json.setJsonData(payload);

  int newTimerCount = 0;
  FirebaseJsonArray arr;
  if (json.get(result, "documents") && result.success) {
    result.get(arr);
    size_t docLen = arr.size();
    for (size_t i = 0; i < docLen && i < 10; i++) {
      FirebaseJsonData docRes;
      if (arr.get(docRes, i) && docRes.success) {
        FirebaseJson docObj;
        docObj.setJsonData(docRes.to<String>());

        FirebaseJsonData subRes;
        bool isActive = false;
        uint64_t endTimeMs = 0;
        String fullDocName = "";

        if (docObj.get(subRes, "name")) {
          fullDocName = subRes.to<String>();
          int idx = fullDocName.indexOf("devices/");
          if (idx != -1) {
            fullDocName = fullDocName.substring(idx);
          }
        }

        if (docObj.get(subRes, "fields/isActive/booleanValue")) {
          isActive = subRes.to<bool>();
        }
        if (docObj.get(subRes, "fields/endTimeMs/integerValue")) {
          String msStr = subRes.to<String>();
          endTimeMs = strtoull(msStr.c_str(), NULL, 10);
        }

        int chList[8];
        int chCount = 0;
        FirebaseJsonData chArrRes;
        if (docObj.get(chArrRes, "fields/channels/arrayValue/values")) {
          FirebaseJsonArray innerArr;
          chArrRes.get(innerArr);
          size_t cLen = innerArr.size();
          for (size_t c = 0; c < cLen && c < 8; c++) {
            FirebaseJsonData cValRes;
            if (innerArr.get(cValRes, c) && cValRes.success) {
              FirebaseJson cObj;
              cObj.setJsonData(cValRes.to<String>());
              FirebaseJsonData intRes;
              if (cObj.get(intRes, "integerValue")) {
                chList[chCount++] = intRes.to<int>();
              }
            }
          }
        }

        if (isActive && endTimeMs > 0 && chCount > 0) {
          activeTimers[newTimerCount].docPath = fullDocName;
          activeTimers[newTimerCount].isActive = isActive;
          activeTimers[newTimerCount].endTimeMs = endTimeMs;
          activeTimers[newTimerCount].channelCount = chCount;
          for (int c = 0; c < chCount; c++) {
            activeTimers[newTimerCount].channels[c] = chList[c];
          }
          newTimerCount++;
        }
      }
    }
  }
  
  if (newTimerCount != activeTimerCount) {
    Serial.printf("[Timer] Active timers count in cloud: %d\n", newTimerCount);
  }
  activeTimerCount = newTimerCount;
}

// ==================== TIMER & SCHEDULE CHECK LOGIC ====================
void checkTimersAndSchedules() {
  time_t now = time(nullptr);
  if (currentWifiState == WIFI_CONNECTED && now > 1700000000) {
    struct tm *ptm = gmtime(&now);
    int totalMinutes = (ptm->tm_hour * 60) + ptm->tm_min + 390; // Myanmar Time (+6:30)
    int currentHour = (totalMinutes / 60) % 24;
    int currentMin = totalMinutes % 60;
    
    char timeBuffer[6];
    sprintf(timeBuffer, "%02d:%02d", currentHour, currentMin);
    String currentTimeStr = String(timeBuffer);

    for (int idx = 0; idx < scheduleCount; idx++) {
      int targetSwitch = activeSchedules[idx].switchNum;
      String sTime = activeSchedules[idx].startTime;
      String eTime = activeSchedules[idx].endTime;
      int targetIndex = targetSwitch - 1;

      if (targetIndex >= 0 && targetIndex < CHANNEL_COUNT) {
        if (currentTimeStr == sTime) {
          if (!relayState[targetIndex]) {
            applyRelayOutput(targetIndex, true);
            pendingCloudSync = true;
            Serial.printf("[Schedule] CH%d turned ON by schedule (%s)\n", targetSwitch, sTime.c_str());
          }
        }
        if (currentTimeStr == eTime) {
          if (relayState[targetIndex]) {
            applyRelayOutput(targetIndex, false);
            pendingCloudSync = true;
            Serial.printf("[Schedule] CH%d turned OFF by schedule (%s)\n", targetSwitch, eTime.c_str());
          }
        }
      }
    }
  }
}

void checkActiveTimers() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  
  uint64_t currentEpochMs = (uint64_t)now * 1000ULL;

  for (int i = 0; i < activeTimerCount; i++) {
    if (activeTimers[i].isActive && currentEpochMs >= activeTimers[i].endTimeMs) {
      Serial.printf("[Timer] Countdown finished! Turning off assigned channels...\n");
      for (int c = 0; c < activeTimers[i].channelCount; c++) {
        int ch = activeTimers[i].channels[c];
        int idx = ch - 1;
        if (idx >= 0 && idx < CHANNEL_COUNT) {
          applyRelayOutput(idx, false);
          Serial.printf("[Timer] CH%d turned OFF by ESP32 independent timer!\n", ch);
        }
      }
      activeTimers[i].isActive = false;
      pendingCloudSync = true;

      if (activeTimers[i].docPath.length() > 0 && currentWifiState == WIFI_CONNECTED && Firebase.ready()) {
        FirebaseJson updateJson;
        updateJson.set("fields/isActive/booleanValue", false);
        if (Firebase.Firestore.patchDocument(&fbdo, PROJECT_ID, "", activeTimers[i].docPath.c_str(), updateJson.raw(), "isActive")) {
          Serial.printf("[Timer] Successfully updated timer isActive=false in Firestore (%s)\n", activeTimers[i].docPath.c_str());
        }
      }
    }
  }
}

// ==================== OTA FIRMWARE UPDATE EXECUTION ====================
void performOTAUpdate(String firmwareUrl) {
  Serial.println("\n==========================================");
  Serial.println("[OTA] Starting Firmware Update Process...");
  Serial.println("[OTA] Target URL: " + firmwareUrl);

  String docPath = "devices/" + deviceID;
  FirebaseJson otaResetJson;
  otaResetJson.set("fields/startOta/booleanValue", false);
  Firebase.Firestore.patchDocument(&fbdo, PROJECT_ID, "", docPath.c_str(), otaResetJson.raw(), "startOta");

  fbdo.clear();

  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.setLedPin(statusLedPin, LOW);
  httpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = httpUpdate.update(client, firmwareUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] Update Failed! Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] No updates available.");
      break;

    case HTTP_UPDATE_OK:
      Serial.println("[OTA] Update Success! Rebooting now...");
      break;
  }
  Serial.println("==========================================\n");
}

// ==================== MAIN LOOP ====================
void loop() {
  handlePhysicalButtons();
  handleWifiAndLedState();
  checkTimersAndSchedules();
  checkActiveTimers();

  if (currentWifiState == WIFI_CONNECTED && Firebase.ready()) {
    if (!deviceProvisioned) checkAndProvisionDevice();

    if (pendingOTA) {
      pendingOTA = false;
      performOTAUpdate(targetOtaUrl);
      return;
    }

    if (pendingCloudSync) {
      pendingCloudSync = false;
      pushLocalStatesToFirestore();
      handlePhysicalButtons();
    }

    if ((millis() - lastFirestorePoll) >= 600) {
      lastFirestorePoll = millis();
      
      if (!pollStepTimers) {
        String docPath = "devices/" + deviceID;
        if (Firebase.Firestore.getDocument(&fbdo, PROJECT_ID, "", docPath.c_str())) {
          parseFirestoreStates(fbdo.payload());
        } else {
          fbdo.clear();
        }
        pollStepTimers = true;
      } else {
        String timersPath = "devices/" + deviceID + "/timers";
        if (Firebase.Firestore.listDocuments(&fbdo, PROJECT_ID, "", timersPath.c_str(), 10, "", "", "", false)) {
          parseFirestoreTimers(fbdo.payload());
        } else {
          fbdo.clear();
        }
        pollStepTimers = false;
      }
      
      handlePhysicalButtons();
    }
  }
}
