/**
 * ESP32-OpenSpool
 * NTAG21x Reader/Writer for OpenSpool Protocol
 *
 * Supported tags: NTAG213, NTAG215, NTAG216
 *
 * Hardware: ESP32-S3 Zero + PN532 RFID Module (SPI)
 *
 * Wiring (SPI):
 *   PN532 SCK  -> GPIO12
 *   PN532 MISO -> GPIO13
 *   PN532 MOSI -> GPIO11
 *   PN532 SS   -> GPIO10
 *   PN532 VCC  -> 3.3V
 *   PN532 GND  -> GND
 *
 * PN532 DIP Switch: Switch 1 = OFF, Switch 2 = ON (SPI mode)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <mutex>
#include <atomic>
#include <vector>

// Firmware version
const char* FIRMWARE_VERSION = "0.2.1";

// WiFi AP Settings (Fallback/Setup Mode)
const char* AP_SSID = "OpenSpool";
const char* AP_PASS = "openspool";
const char* MDNS_HOSTNAME = "openspool";

// SPI Pins for ESP32-S3 Zero
#define PN532_SCK  12
#define PN532_MISO 13
#define PN532_MOSI 11
#define PN532_SS   10

// Buzzer Pin (optional - connect piezo buzzer between GPIO6 and GND)
#define BUZZER_PIN 6
#define BUZZER_ENABLED true  // Set to false to disable buzzer

// WiFi connection timeout (ms)
#define WIFI_TIMEOUT 15000

// Task configuration
#define SCAN_TASK_STACK_SIZE 4096
#define SCAN_TASK_PRIORITY 1

// Timing constants (ms)
#define NFC_INIT_DELAY_MS 100
#define NFC_RETRY_DELAY_MS 300
#define WIFI_CHECK_INTERVAL_MS 30000
#define LOOP_DELAY_MS 100

// NFC configuration
#define NFC_INIT_ATTEMPTS 3
#define NFC_TAG_TIMEOUT_MS 1000

// Payload limits
#define MAX_WIFI_PAYLOAD_SIZE 512
#define MAX_WRITE_PAYLOAD_SIZE 1024

// Buzzer tone frequencies (Hz)
#define TONE_TAG_DETECTED 1000
#define TONE_SUCCESS 1500
#define TONE_ERROR 400

// PN532 Setup (Software SPI - more compatible)
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

// Web Server
AsyncWebServer server(80);

// Preferences for storing WiFi credentials
Preferences preferences;

// Global state (protected by mutex for thread safety)
std::mutex stateMutex;
bool nfcReady = false;
String lastError = "";

// WiFi state (protected by stateMutex)
enum WifiState { WIFI_MODE_AP_ONLY, WIFI_MODE_STA_ONLY, WIFI_MODE_AP_AND_STA };
WifiState currentWifiState = WIFI_MODE_AP_ONLY;
String currentSSID = "";
String currentIP = "";
bool wifiConnected = false;

// Deferred actions (to avoid blocking delay() in async handlers)
enum PendingAction { ACTION_NONE, ACTION_WIFI_CONNECT, ACTION_WIFI_DISCONNECT, ACTION_RESTART, ACTION_OTA_RESTART };
volatile PendingAction pendingAction = ACTION_NONE;
String pendingSSID = "";
String pendingPassword = "";
unsigned long pendingActionTime = 0;
const unsigned long ACTION_DELAY_MS = 500;

// Auto-write feature
bool autoWriteEnabled = false;
String autoWriteData = "";  // JSON data to write automatically
String lastWrittenTagUID = "";  // Prevent writing same tag multiple times
unsigned long lastWriteTime = 0;
const unsigned long AUTO_WRITE_COOLDOWN_MS = 3000;  // Cooldown between writes to same tag

// Auto-write result tracking (for web notifications)
volatile bool autoWriteResultPending = false;
volatile bool autoWriteResultSuccess = false;
String autoWriteResultTagUID = "";
String autoWriteResultError = "";
unsigned long autoWriteResultTime = 0;

// NTAG21x page configuration
#define NTAG_USER_START 4
#define NTAG_PAGE_SIZE 4

// Tag types and their user memory end pages
enum NtagType { NTAG_UNKNOWN, NTAG_213, NTAG_215, NTAG_216 };
// NTAG213: pages 4-39  (144 bytes user memory)
// NTAG215: pages 4-129 (504 bytes user memory)
// NTAG216: pages 4-225 (888 bytes user memory)
#define NTAG213_USER_END 39
#define NTAG215_USER_END 129
#define NTAG216_USER_END 225

// Forward declarations
void setupWebServer();
void setupNFC();
String getWiFiStatusJson();

// ============== Buzzer Functions ==============

void setupBuzzer() {
    if (BUZZER_ENABLED) {
        pinMode(BUZZER_PIN, OUTPUT);
        digitalWrite(BUZZER_PIN, LOW);
    }
}

// Play a tone (frequency in Hz, duration in ms)
void playTone(int frequency, int duration) {
    if (!BUZZER_ENABLED) return;
    tone(BUZZER_PIN, frequency, duration);
    delay(duration);
    noTone(BUZZER_PIN);
}

// Short beep - tag detected
void beepTagDetected() {
    playTone(TONE_TAG_DETECTED, 100);
}

// Double beep - success
void beepSuccess() {
    playTone(TONE_SUCCESS, 100);
    delay(50);
    playTone(TONE_SUCCESS, 100);
}

// Long low beep - error
void beepError() {
    playTone(TONE_ERROR, 300);
}

// ============== End Buzzer Functions ==============

// ============== Custom Filament Database ==============

// Storage structure:
// {
//   "brands": ["Custom Brand 1", "Custom Brand 2"],
//   "types": ["PLA+", "PETG-HT"],
//   "presets": {
//     "Brand|Type": { "nozzle": [min, max], "bed": [min, max] }
//   }
// }

// Load complete filament database as JSON
String loadFilamentDatabase() {
    preferences.begin("filaments", true);  // Read-only
    String data = preferences.getString("data", "{\"brands\":[],\"types\":[],\"presets\":{}}");
    preferences.end();
    return data;
}

// Save complete filament database from JSON
bool saveFilamentDatabase(const String& jsonData) {
    // Validate JSON and size
    if (jsonData.length() > 8000) {
        return false;  // Too large
    }
    
    JsonDocument doc;
    if (deserializeJson(doc, jsonData)) {
        return false;  // Invalid JSON
    }
    
    preferences.begin("filaments", false);  // Read-write
    preferences.putString("data", jsonData);
    preferences.end();
    return true;
}

// Helper: Load database into JsonDocument
bool loadFilamentDoc(JsonDocument& doc) {
    String data = loadFilamentDatabase();
    return deserializeJson(doc, data) == DeserializationError::Ok;
}

// Helper: Save JsonDocument to database
bool saveFilamentDoc(JsonDocument& doc) {
    String output;
    serializeJson(doc, output);
    return saveFilamentDatabase(output);
}

// Add a custom brand
bool addCustomBrand(const String& brand) {
    JsonDocument doc;
    loadFilamentDoc(doc);
    
    // Check if already exists
    JsonArray brands = doc["brands"].as<JsonArray>();
    for (JsonVariant v : brands) {
        if (v.as<String>() == brand) return true;  // Already exists
    }
    
    // Add new brand
    if (!doc["brands"].is<JsonArray>()) {
        doc["brands"].to<JsonArray>();
    }
    doc["brands"].add(brand);
    
    return saveFilamentDoc(doc);
}

// Delete a custom brand
bool deleteCustomBrand(const String& brand) {
    JsonDocument doc;
    loadFilamentDoc(doc);
    
    // Find the index of the brand to remove
    int indexToRemove = -1;
    JsonArray brands = doc["brands"].as<JsonArray>();
    
    int i = 0;
    for (JsonVariant v : brands) {
        if (v.as<String>() == brand) {
            indexToRemove = i;
            break;
        }
        i++;
    }
    
    if (indexToRemove >= 0) {
        doc["brands"].as<JsonArray>().remove(indexToRemove);
    }
    
    // Also remove any presets using this brand
    JsonObject presets = doc["presets"].as<JsonObject>();
    std::vector<String> keysToRemove;
    for (JsonPair p : presets) {
        String key = p.key().c_str();
        if (key.startsWith(brand + "|")) {
            keysToRemove.push_back(key);
        }
    }
    for (const String& key : keysToRemove) {
        doc["presets"].as<JsonObject>().remove(key);
    }
    
    return saveFilamentDoc(doc);
}

// Add a custom material type
bool addCustomType(const String& type) {
    JsonDocument doc;
    loadFilamentDoc(doc);
    
    // Check if already exists
    JsonArray types = doc["types"].as<JsonArray>();
    for (JsonVariant v : types) {
        if (v.as<String>() == type) return true;  // Already exists
    }
    
    // Add new type
    if (!doc["types"].is<JsonArray>()) {
        doc["types"].to<JsonArray>();
    }
    doc["types"].add(type);
    
    return saveFilamentDoc(doc);
}

// Delete a custom material type
bool deleteCustomType(const String& type) {
    JsonDocument doc;
    loadFilamentDoc(doc);
    
    // Find the index of the type to remove
    int indexToRemove = -1;
    JsonArray types = doc["types"].as<JsonArray>();
    
    int i = 0;
    for (JsonVariant v : types) {
        if (v.as<String>() == type) {
            indexToRemove = i;
            break;
        }
        i++;
    }
    
    if (indexToRemove >= 0) {
        doc["types"].as<JsonArray>().remove(indexToRemove);
    }
    
    // Also remove any presets using this type
    JsonObject presets = doc["presets"].as<JsonObject>();
    std::vector<String> keysToRemove;
    for (JsonPair p : presets) {
        String key = p.key().c_str();
        if (key.endsWith("|" + type)) {
            keysToRemove.push_back(key);
        }
    }
    for (const String& key : keysToRemove) {
        doc["presets"].as<JsonObject>().remove(key);
    }
    
    return saveFilamentDoc(doc);
}

// Save a single custom filament preset
bool saveCustomFilament(const String& brand, const String& type, int nozzleMin, int nozzleMax, int bedMin, int bedMax) {
    JsonDocument doc;
    loadFilamentDoc(doc);
    
    // Ensure presets object exists
    if (!doc["presets"].is<JsonObject>()) {
        doc["presets"].to<JsonObject>();
    }
    
    // Create key: "Brand|Type"
    String key = brand + "|" + type;
    
    // Add/update entry
    doc["presets"][key]["nozzle"][0] = nozzleMin;
    doc["presets"][key]["nozzle"][1] = nozzleMax;
    doc["presets"][key]["bed"][0] = bedMin;
    doc["presets"][key]["bed"][1] = bedMax;
    
    return saveFilamentDoc(doc);
}

// Delete a single custom filament preset
bool deleteCustomFilament(const String& brand, const String& type) {
    JsonDocument doc;
    loadFilamentDoc(doc);
    
    String key = brand + "|" + type;
    
    if (doc["presets"].is<JsonObject>()) {
        doc["presets"].remove(key);
    }
    
    return saveFilamentDoc(doc);
}

// Clear all custom filament data
void clearFilamentDatabase() {
    preferences.begin("filaments", false);
    preferences.remove("data");
    preferences.end();
    Serial.println("Filament database cleared");
}

// ============== End Custom Filament Database ==============

// Load saved WiFi credentials
bool loadWiFiCredentials(String &ssid, String &password) {
    preferences.begin("wifi", true);  // Read-only
    ssid = preferences.getString("ssid", "");
    password = preferences.getString("password", "");
    preferences.end();
    return ssid.length() > 0;
}

// Save WiFi credentials
void saveWiFiCredentials(const String &ssid, const String &password) {
    preferences.begin("wifi", false);  // Read-write
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();
    Serial.println("WiFi credentials saved");
}

// Clear saved WiFi credentials
void clearWiFiCredentials() {
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();
    Serial.println("WiFi credentials cleared");
}

// Start mDNS responder
void startMDNS() {
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.print("mDNS started: http://");
        Serial.print(MDNS_HOSTNAME);
        Serial.println(".local");
    } else {
        Serial.println("mDNS failed to start");
    }
}

// Start Access Point mode
void startAP() {
    // Ensure WiFi is properly initialized
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    
    WiFi.mode(WIFI_AP);
    delay(100);
    
    // Configure AP with explicit channel (1) and max connections (4)
    bool apStarted = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
    delay(500);  // Give AP time to fully initialize
    
    // Set TX power to maximum for better range
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    currentWifiState = WIFI_MODE_AP_ONLY;
    currentSSID = AP_SSID;
    currentIP = WiFi.softAPIP().toString();
    wifiConnected = false;

    startMDNS();

    Serial.println("\n=== Access Point Mode ===");
    Serial.print("AP Started: ");
    Serial.println(apStarted ? "YES" : "NO");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Password: ");
    Serial.println(AP_PASS);
    Serial.print("IP: ");
    Serial.println(currentIP);
    Serial.print("Channel: 1, TX Power: ");
    Serial.println(WiFi.getTxPower());
}

// Start Access Point + Station mode (for configuration while connected)
void startAPSTA(const String &ssid, const String &password) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    WiFi.begin(ssid.c_str(), password.c_str());

    Serial.print("Connecting to ");
    Serial.print(ssid);

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_TIMEOUT) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        currentWifiState = WIFI_MODE_AP_AND_STA;
        currentSSID = ssid;
        currentIP = WiFi.localIP().toString();
        wifiConnected = true;

        startMDNS();

        Serial.println("=== Connected to WiFi ===");
        Serial.print("Network: ");
        Serial.println(ssid);
        Serial.print("IP: ");
        Serial.println(currentIP);
        Serial.println("\n=== AP still active ===");
        Serial.print("AP SSID: ");
        Serial.println(AP_SSID);
        Serial.print("AP IP: ");
        Serial.println(WiFi.softAPIP());
    } else {
        Serial.println("Connection failed, staying in AP mode");
        startAP();
    }
}

// Connect to WiFi (Station mode only)
bool connectToWiFi(const String &ssid, const String &password) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    Serial.print("Connecting to ");
    Serial.print(ssid);

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_TIMEOUT) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        currentWifiState = WIFI_MODE_STA_ONLY;
        currentSSID = ssid;
        currentIP = WiFi.localIP().toString();
        wifiConnected = true;

        Serial.println("=== Connected to WiFi ===");
        Serial.print("Network: ");
        Serial.println(ssid);
        Serial.print("IP: ");
        Serial.println(currentIP);
        return true;
    }

    return false;
}

// Setup WiFi based on saved credentials
void setupWiFi() {
    String savedSSID, savedPassword;

    if (loadWiFiCredentials(savedSSID, savedPassword)) {
        Serial.println("Found saved WiFi credentials");
        // Try to connect, but keep AP available for configuration
        startAPSTA(savedSSID, savedPassword);
    } else {
        Serial.println("No saved WiFi credentials");
        startAP();
    }
}

// Cached scan results
String cachedScanResult = "{\"networks\":[]}";
std::atomic<bool> scanInProgress(false);

// Background scan task
void scanTask(void* parameter) {
    Serial.println("Scan task started");
    int n = WiFi.scanNetworks(false, false, false, 100);
    Serial.printf("Found %d networks\n", n);

    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    for (int i = 0; i < n; i++) {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }

    WiFi.scanDelete();
    serializeJson(doc, cachedScanResult);
    Serial.println("Scan complete");
    scanInProgress = false;
    vTaskDelete(NULL);
}

// Start scan and return cached results
String scanNetworks() {
    if (!scanInProgress) {
        scanInProgress = true;
        Serial.println("Starting WiFi scan task...");
        xTaskCreatePinnedToCore(scanTask, "scanTask", SCAN_TASK_STACK_SIZE, NULL, SCAN_TASK_PRIORITY, NULL, 0);
    }
    return cachedScanResult;
}

// Get current WiFi status as JSON
String getWiFiStatusJson() {
    JsonDocument doc;

    doc["mode"] = currentWifiState == WIFI_MODE_AP_ONLY ? "ap" : (currentWifiState == WIFI_MODE_STA_ONLY ? "sta" : "ap_sta");
    doc["connected"] = wifiConnected;
    doc["ssid"] = currentSSID;
    doc["ip"] = currentIP;
    doc["ap_ssid"] = AP_SSID;
    doc["ap_ip"] = WiFi.softAPIP().toString();

    // Check if we have saved credentials
    String savedSSID, savedPassword;
    doc["configured"] = loadWiFiCredentials(savedSSID, savedPassword);
    doc["configured_ssid"] = savedSSID;

    String result;
    serializeJson(doc, result);
    return result;
}

void setupNFC() {
    delay(NFC_INIT_DELAY_MS);  // Let PN532 power up

    // Try multiple times to connect
    uint32_t versiondata = 0;
    for (int attempt = 1; attempt <= NFC_INIT_ATTEMPTS; attempt++) {
        Serial.printf("NFC init attempt %d/%d...\n", attempt, NFC_INIT_ATTEMPTS);
        nfc.begin();
        delay(NFC_INIT_DELAY_MS);
        versiondata = nfc.getFirmwareVersion();
        if (versiondata) {
            break;  // Success!
        }
        delay(NFC_RETRY_DELAY_MS);  // Wait before retry
    }

    if (!versiondata) {
        Serial.printf("PN532 not found after %d attempts!\n", NFC_INIT_ATTEMPTS);
        nfcReady = false;
        return;
    }

    Serial.print("Found PN53x with firmware: ");
    Serial.print((versiondata >> 24) & 0xFF, HEX);
    Serial.print('.');
    Serial.println((versiondata >> 16) & 0xFF, HEX);

    nfc.SAMConfig();
    nfcReady = true;
    Serial.println("NFC ready!");
}

bool waitForTag(uint8_t* uid, uint8_t* uidLength, uint16_t timeout = NFC_TAG_TIMEOUT_MS) {
    nfc.setPassiveActivationRetries(timeout / 100);
    bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLength);

    // Validate UID length (should be 4 or 7 bytes for NTAG21x)
    if (success && (*uidLength != 4 && *uidLength != 7)) {
        Serial.printf("Warning: Unexpected UID length: %d bytes\n", *uidLength);
    }

    // Beep on tag detection
    if (success) {
        beepTagDetected();
    }

    return success;
}

// Detect NTAG type by reading the Capability Container (CC) at page 3
NtagType detectNtagType() {
    uint8_t ccPage[4];

    if (!nfc.ntag2xx_ReadPage(3, ccPage)) {
        return NTAG_UNKNOWN;
    }

    // CC byte 2 contains size info: size = (CC[2] * 8) bytes
    // NTAG213: CC[2] = 0x12 (144 bytes)
    // NTAG215: CC[2] = 0x3E (496 bytes)
    // NTAG216: CC[2] = 0x6D (872 bytes)
    uint8_t sizeIndicator = ccPage[2];

    if (sizeIndicator == 0x12) {
        Serial.println("Detected: NTAG213 (144 bytes)");
        return NTAG_213;
    } else if (sizeIndicator == 0x3E) {
        Serial.println("Detected: NTAG215 (504 bytes)");
        return NTAG_215;
    } else if (sizeIndicator == 0x6D) {
        Serial.println("Detected: NTAG216 (888 bytes)");
        return NTAG_216;
    }

    // Fallback: try to detect by reading higher pages
    uint8_t testPage[4];
    if (nfc.ntag2xx_ReadPage(130, testPage)) {
        Serial.println("Detected: NTAG216 (by page test)");
        return NTAG_216;
    }
    if (nfc.ntag2xx_ReadPage(40, testPage)) {
        Serial.println("Detected: NTAG215 (by page test)");
        return NTAG_215;
    }

    Serial.println("Detected: NTAG213 (default)");
    return NTAG_213;
}

int getNtagUserEnd(NtagType type) {
    switch (type) {
        case NTAG_213: return NTAG213_USER_END;
        case NTAG_215: return NTAG215_USER_END;
        case NTAG_216: return NTAG216_USER_END;
        default: return NTAG213_USER_END;
    }
}

int getNtagUserBytes(NtagType type) {
    return (getNtagUserEnd(type) - NTAG_USER_START + 1) * NTAG_PAGE_SIZE;
}

String getNtagTypeName(NtagType type) {
    switch (type) {
        case NTAG_213: return "NTAG213";
        case NTAG_215: return "NTAG215";
        case NTAG_216: return "NTAG216";
        default: return "Unknown";
    }
}

String uidToString(uint8_t* uid, uint8_t uidLength) {
    String result = "";
    for (uint8_t i = 0; i < uidLength; i++) {
        if (i > 0) result += ":";
        if (uid[i] < 0x10) result += "0";
        result += String(uid[i], HEX);
    }
    result.toUpperCase();
    return result;
}

// Get configuration page address for each NTAG type
int getNtagConfigPage(NtagType type) {
    switch (type) {
        case NTAG_213: return 41;   // Config pages 41-44
        case NTAG_215: return 131;  // Config pages 131-134
        case NTAG_216: return 227;  // Config pages 227-230
        default: return 41;
    }
}

// Read NTAG originality signature (32 bytes)
bool readNtagSignature(uint8_t* signature) {
    uint8_t cmd[1] = {0x3C};  // READ_SIG command
    uint8_t response[34];     // 32 bytes signature + status bytes
    uint8_t responseLength = sizeof(response);

    if (!nfc.inDataExchange(cmd, 1, response, &responseLength)) {
        return false;
    }

    if (responseLength >= 32) {
        memcpy(signature, response, 32);
        return true;
    }
    return false;
}

// Read NTAG password protection configuration
// Returns: 0 = no protection, 1-255 = protected from page N
uint8_t readPasswordProtectionStatus(NtagType type) {
    int configPage = getNtagConfigPage(type);
    uint8_t pageData[4];

    // AUTH0 is at config page + 0, byte 3
    if (nfc.ntag2xx_ReadPage(configPage, pageData)) {
        return pageData[3];  // AUTH0 value (first page requiring auth, 0xFF = disabled)
    }
    return 0xFF;  // Default: no protection
}

// Read NTAG access configuration
bool readAccessConfig(NtagType type, uint8_t* access) {
    int configPage = getNtagConfigPage(type);

    // ACCESS byte is at config page + 1, byte 0
    if (nfc.ntag2xx_ReadPage(configPage + 1, access)) {
        return true;
    }
    return false;
}

// Print comprehensive tag information to Serial
void printTagInfo(uint8_t* uid, uint8_t uidLength, NtagType tagType) {
    Serial.println("\n╔══════════════════════════════════════════════════════════════╗");
    Serial.println("║                    NFC TAG INFORMATION                       ║");
    Serial.println("╠══════════════════════════════════════════════════════════════╣");

    // UID / Serial Number
    Serial.print("║ UID:              ");
    String uidStr = uidToString(uid, uidLength);
    Serial.print(uidStr);
    for (int i = uidStr.length(); i < 42; i++) Serial.print(" ");
    Serial.println("║");

    // UID Length
    Serial.printf("║ UID Length:       %d bytes                                     ║\n", uidLength);

    // Tag Type
    String typeName = getNtagTypeName(tagType);
    Serial.print("║ Tag Type:         ");
    Serial.print(typeName);
    for (int i = typeName.length(); i < 42; i++) Serial.print(" ");
    Serial.println("║");

    // Memory Capacity
    int capacity = getNtagUserBytes(tagType);
    Serial.printf("║ User Memory:      %d bytes                                    ║\n", capacity);

    // ATQA (Answer To Request Type A) - Standard for NTAG21x
    Serial.println("║ ATQA:             0x0044 (NTAG21x standard)                    ║");

    // SAK (Select Acknowledge) - Standard for NTAG21x
    Serial.println("║ SAK:              0x00 (NTAG21x standard)                      ║");

    Serial.println("╠══════════════════════════════════════════════════════════════╣");
    Serial.println("║                    SECURITY INFORMATION                      ║");
    Serial.println("╠══════════════════════════════════════════════════════════════╣");

    // Password Protection Status
    uint8_t auth0 = readPasswordProtectionStatus(tagType);
    if (auth0 == 0xFF) {
        Serial.println("║ Password Prot.:   Disabled (no protection)                   ║");
    } else {
        Serial.printf("║ Password Prot.:   Enabled from page %d                        ║\n", auth0);
    }

    // Access Configuration
    uint8_t accessData[4];
    if (readAccessConfig(tagType, accessData)) {
        bool prot = (accessData[0] & 0x80) != 0;  // PROT bit
        bool cfglck = (accessData[0] & 0x40) != 0; // CFGLCK bit
        uint8_t authlim = accessData[0] & 0x07;   // AUTHLIM bits

        Serial.printf("║ Write Protect:    %s                                       ║\n", prot ? "Yes" : "No ");
        Serial.printf("║ Config Locked:    %s                                       ║\n", cfglck ? "Yes" : "No ");
        if (authlim == 0) {
            Serial.println("║ Auth Attempts:    Unlimited                                  ║");
        } else {
            Serial.printf("║ Auth Attempts:    %d remaining                                 ║\n", authlim);
        }
    }

    Serial.println("╠══════════════════════════════════════════════════════════════╣");
    Serial.println("║                    ORIGINALITY SIGNATURE                     ║");
    Serial.println("╠══════════════════════════════════════════════════════════════╣");

    // Try to read originality signature
    uint8_t signature[32];
    if (readNtagSignature(signature)) {
        Serial.print("║ ");
        for (int i = 0; i < 16; i++) {
            Serial.printf("%02X", signature[i]);
        }
        Serial.println("             ║");
        Serial.print("║ ");
        for (int i = 16; i < 32; i++) {
            Serial.printf("%02X", signature[i]);
        }
        Serial.println("             ║");
    } else {
        Serial.println("║ (Could not read signature)                                   ║");
    }

    Serial.println("╚══════════════════════════════════════════════════════════════╝");
}

String readNtagData() {
    uint8_t uid[7];
    uint8_t uidLength;

    Serial.println("readNtagData: waiting for tag...");
    if (!waitForTag(uid, &uidLength)) {
        Serial.println("readNtagData: no tag found");
        return "{\"success\": false, \"error\": \"No tag found\"}";
    }
    Serial.println("readNtagData: tag found, detecting type...");

    // Detect tag type
    NtagType tagType = detectNtagType();
    if (tagType == NTAG_UNKNOWN) {
        return "{\"success\": false, \"error\": \"Unknown tag type\"}";
    }

    // Print comprehensive tag information to Serial
    printTagInfo(uid, uidLength, tagType);

    int userEnd = getNtagUserEnd(tagType);
    int capacity = getNtagUserBytes(tagType);
    String uidStr = uidToString(uid, uidLength);
    String tagTypeName = getNtagTypeName(tagType);

    String data = "";
    uint8_t pageData[4];

    // Read raw data
    Serial.println("\n┌─────────────────────────────────────────────────────────────┐");
    Serial.println("│                       RAW TAG DATA                          │");
    Serial.println("├─────────────────────────────────────────────────────────────┤");

    for (int page = NTAG_USER_START; page <= userEnd; page++) {
        if (nfc.ntag2xx_ReadPage(page, pageData)) {
            bool foundEnd = false;
            for (int i = 0; i < 4; i++) {
                if (pageData[i] == 0x00 || pageData[i] == 0xFE) {
                    foundEnd = true;
                    break;
                }
                data += (char)pageData[i];
            }
            if (foundEnd) break;
        } else {
            break;
        }
    }

    // Print raw hex dump of first pages (header + data start)
    Serial.println("│ Page | Hex Data        | ASCII                              │");
    Serial.println("├──────┼─────────────────┼────────────────────────────────────┤");

    int maxPagesToShow = min(userEnd, NTAG_USER_START + 15);  // Show up to 16 pages
    for (int page = NTAG_USER_START; page <= maxPagesToShow; page++) {
        if (nfc.ntag2xx_ReadPage(page, pageData)) {
            Serial.printf("│ %3d  │ %02X %02X %02X %02X     │ ", page,
                pageData[0], pageData[1], pageData[2], pageData[3]);

            // ASCII representation
            for (int i = 0; i < 4; i++) {
                if (pageData[i] >= 32 && pageData[i] < 127) {
                    Serial.print((char)pageData[i]);
                } else {
                    Serial.print(".");
                }
            }
            Serial.println("                                │");
        }
    }
    if (maxPagesToShow < userEnd) {
        Serial.println("│ ...  │ (more data)     │                                    │");
    }
    Serial.println("└─────────────────────────────────────────────────────────────┘");

    JsonDocument responseDoc;
    responseDoc["tag"]["uid"] = uidStr;
    responseDoc["tag"]["type"] = tagTypeName;
    responseDoc["tag"]["capacity"] = capacity;

    int jsonStart = data.indexOf('{');
    int jsonEnd = data.lastIndexOf('}');

    if (jsonStart >= 0 && jsonEnd > jsonStart) {
        String jsonData = data.substring(jsonStart, jsonEnd + 1);
        responseDoc["tag"]["used"] = jsonData.length();

        // Print parsed JSON data
        Serial.println("\n┌─────────────────────────────────────────────────────────────┐");
        Serial.println("│                     PARSED OPENSPOOL DATA                   │");
        Serial.println("├─────────────────────────────────────────────────────────────┤");

        // Parse the data and include it
        JsonDocument dataDoc;
        if (deserializeJson(dataDoc, jsonData) == DeserializationError::Ok) {
            responseDoc["data"] = dataDoc;

            // Print each field
            if (dataDoc["protocol"].is<const char*>())
                Serial.printf("│ Protocol:         %-40s │\n", dataDoc["protocol"].as<const char*>());
            if (dataDoc["version"].is<const char*>())
                Serial.printf("│ Version:          %-40s │\n", dataDoc["version"].as<const char*>());
            if (dataDoc["brand"].is<const char*>())
                Serial.printf("│ Brand:            %-40s │\n", dataDoc["brand"].as<const char*>());
            if (dataDoc["type"].is<const char*>())
                Serial.printf("│ Material Type:    %-40s │\n", dataDoc["type"].as<const char*>());
            if (dataDoc["subtype"].is<const char*>())
                Serial.printf("│ Subtype:          %-40s │\n", dataDoc["subtype"].as<const char*>());
            if (dataDoc["color_hex"].is<const char*>())
                Serial.printf("│ Color:            %-40s │\n", dataDoc["color_hex"].as<const char*>());
            if (dataDoc["min_temp"].is<int>())
                Serial.printf("│ Nozzle Temp:      %d - %d °C                               │\n",
                    dataDoc["min_temp"].as<int>(), dataDoc["max_temp"].as<int>());
            if (dataDoc["bed_min_temp"].is<int>())
                Serial.printf("│ Bed Temp:         %d - %d °C                                │\n",
                    dataDoc["bed_min_temp"].as<int>(), dataDoc["bed_max_temp"].as<int>());
        } else {
            responseDoc["data"] = jsonData;
            Serial.println("│ (Could not parse JSON data)                                 │");
        }
        Serial.println("└─────────────────────────────────────────────────────────────┘\n");

    } else if (data.length() == 0) {
        responseDoc["tag"]["used"] = 0;
        responseDoc["data"] = nullptr;
        responseDoc["message"] = "Tag is empty";
        Serial.println("\n[Tag is empty - no data found]");
    } else {
        responseDoc["tag"]["used"] = 0;
        responseDoc["success"] = false;
        responseDoc["error"] = "No valid JSON found on tag";
        Serial.println("\n[No valid OpenSpool JSON found on tag]");
    }

    String result;
    serializeJson(responseDoc, result);
    return result;
}

bool writeNtagData(const String& jsonData) {
    uint8_t uid[7];
    uint8_t uidLength;

    if (!waitForTag(uid, &uidLength)) {
        lastError = "No tag found";
        beepError();
        return false;
    }

    // Detect tag type
    NtagType tagType = detectNtagType();
    if (tagType == NTAG_UNKNOWN) {
        lastError = "Unknown tag type";
        beepError();
        return false;
    }

    int userEnd = getNtagUserEnd(tagType);
    int maxBytes = getNtagUserBytes(tagType);

    String payload = jsonData;
    int dataLen = payload.length();

    // MIME type "application/json" = 16 bytes
    const char* mimeType = "application/json";
    int mimeTypeLen = 16;

    // Check if data fits on tag (with NDEF overhead)
    if (dataLen + mimeTypeLen + 10 > maxBytes) {
        lastError = "Data too long for tag (" + String(dataLen) + " bytes, max " + String(maxBytes - mimeTypeLen - 10) + ")";
        return false;
    }

    // Build NDEF message with MIME type record
    // Format: 03 [length] D2 10 [payload_len] "application/json" [JSON data] FE

    uint8_t header[32];  // Enough for header + mime type
    int headerLen = 0;

    // NDEF Message TLV
    header[headerLen++] = 0x03;  // NDEF Message TLV type

    // NDEF record: D2 = MB=1, ME=1, CF=0, SR=1, IL=0, TNF=0x02 (MIME media type)
    // Total record length = 1 (header) + 1 (type len) + 1 (payload len) + 16 (type) + dataLen
    int recordLen = 1 + 1 + 1 + mimeTypeLen + dataLen;  // For short record (payload < 256)

    if (dataLen < 256) {
        // Short record format
        header[headerLen++] = recordLen & 0xFF;  // NDEF Message length
        header[headerLen++] = 0xD2;              // NDEF record header (MIME type, short record)
        header[headerLen++] = mimeTypeLen;       // Type length (16)
        header[headerLen++] = dataLen;           // Payload length
    } else {
        // Long record format (payload >= 256 bytes)
        recordLen = 1 + 1 + 4 + mimeTypeLen + dataLen;  // 4-byte payload length
        header[headerLen++] = 0xFF;                      // Long format marker
        header[headerLen++] = (recordLen >> 8) & 0xFF;   // Length high byte
        header[headerLen++] = recordLen & 0xFF;          // Length low byte
        header[headerLen++] = 0xC2;                      // NDEF record header (MIME type, long record, SR=0)
        header[headerLen++] = mimeTypeLen;               // Type length (16)
        header[headerLen++] = (dataLen >> 24) & 0xFF;    // Payload length (4 bytes)
        header[headerLen++] = (dataLen >> 16) & 0xFF;
        header[headerLen++] = (dataLen >> 8) & 0xFF;
        header[headerLen++] = dataLen & 0xFF;
    }

    // Add MIME type string
    for (int i = 0; i < mimeTypeLen; i++) {
        header[headerLen++] = mimeType[i];
    }

    // Write header to tag
    int page = NTAG_USER_START;
    int byteIndex = 0;
    uint8_t pageData[4] = {0, 0, 0, 0};

    for (int i = 0; i < headerLen && page <= userEnd; i++) {
        pageData[byteIndex++] = header[i];
        if (byteIndex == 4) {
            if (!nfc.ntag2xx_WritePage(page, pageData)) {
                lastError = "Write failed at page " + String(page);
                return false;
            }
            page++;
            byteIndex = 0;
            memset(pageData, 0, 4);
        }
    }

    // Write JSON payload
    for (int i = 0; i < dataLen && page <= userEnd; i++) {
        pageData[byteIndex++] = payload[i];
        if (byteIndex == 4) {
            if (!nfc.ntag2xx_WritePage(page, pageData)) {
                lastError = "Write failed at page " + String(page);
                return false;
            }
            page++;
            byteIndex = 0;
            memset(pageData, 0, 4);
        }
    }

    // Write terminator TLV (0xFE)
    pageData[byteIndex++] = 0xFE;
    while (byteIndex < 4) {
        pageData[byteIndex++] = 0x00;
    }
    if (!nfc.ntag2xx_WritePage(page, pageData)) {
        lastError = "Write failed at terminator";
        return false;
    }

    Serial.println("Tag written successfully with MIME type: application/json");
    beepSuccess();
    return true;
}

// Erase tag by writing zeros to user memory
String eraseNtagData() {
    uint8_t uid[7];
    uint8_t uidLength;

    if (!waitForTag(uid, &uidLength)) {
        return "{\"success\": false, \"error\": \"No tag found\"}";
    }

    // Detect tag type
    NtagType tagType = detectNtagType();
    if (tagType == NTAG_UNKNOWN) {
        return "{\"success\": false, \"error\": \"Unknown tag type\"}";
    }

    int userEnd = getNtagUserEnd(tagType);
    String uidStr = uidToString(uid, uidLength);
    String tagTypeName = getNtagTypeName(tagType);

    uint8_t emptyPage[4] = {0x00, 0x00, 0x00, 0x00};

    // Write terminator to first user page, then zeros
    uint8_t terminatorPage[4] = {0x03, 0x00, 0xFE, 0x00};  // Empty NDEF message
    if (!nfc.ntag2xx_WritePage(NTAG_USER_START, terminatorPage)) {
        return "{\"success\": false, \"error\": \"Failed to write terminator\"}";
    }

    // Clear remaining pages
    for (int page = NTAG_USER_START + 1; page <= userEnd; page++) {
        if (!nfc.ntag2xx_WritePage(page, emptyPage)) {
            // Stop on error, but don't fail - we already wrote the terminator
            break;
        }
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Tag erased successfully";
    doc["tag"]["uid"] = uidStr;
    doc["tag"]["type"] = tagTypeName;

    beepSuccess();

    String result;
    serializeJson(doc, result);
    return result;
}

void setupWebServer() {
    // Add default headers for security
    DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
    DefaultHeaders::Instance().addHeader("X-Frame-Options", "DENY");
    DefaultHeaders::Instance().addHeader("X-XSS-Protection", "1; mode=block");

    // API: Get combined status (NFC + WiFi)
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["version"] = FIRMWARE_VERSION;
        doc["nfcReady"] = nfcReady;
        doc["lastError"] = lastError;
        doc["wifi"]["mode"] = currentWifiState == WIFI_MODE_AP_ONLY ? "ap" : (currentWifiState == WIFI_MODE_STA_ONLY ? "sta" : "ap_sta");
        doc["wifi"]["connected"] = wifiConnected;
        doc["wifi"]["ssid"] = currentSSID;
        doc["wifi"]["ip"] = currentIP;
        doc["wifi"]["ap_ip"] = WiFi.softAPIP().toString();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // API: Get WiFi status
    server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", getWiFiStatusJson());
    });

    // API: Scan WiFi networks
    server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", scanNetworks());
    });

    // API: Connect to WiFi
    server.on("/api/wifi/connect", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            // Limit payload size to prevent memory issues
            if (total > MAX_WIFI_PAYLOAD_SIZE) {
                request->send(413, "application/json", "{\"success\": false, \"error\": \"Payload too large\"}");
                return;
            }

            String body = String((char*)data).substring(0, len);

            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            String ssid = doc["ssid"] | "";
            String password = doc["password"] | "";

            if (ssid.length() == 0) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"SSID required\"}");
                return;
            }

            // Save credentials
            saveWiFiCredentials(ssid, password);

            request->send(200, "application/json", "{\"success\": true, \"message\": \"Credentials saved. Reconnecting...\"}");

            // Schedule reconnect (non-blocking)
            pendingSSID = ssid;
            pendingPassword = password;
            pendingAction = ACTION_WIFI_CONNECT;
            pendingActionTime = millis();
        }
    );

    // API: Disconnect and clear credentials
    server.on("/api/wifi/disconnect", HTTP_POST, [](AsyncWebServerRequest *request) {
        clearWiFiCredentials();
        request->send(200, "application/json", "{\"success\": true, \"message\": \"Credentials cleared. Restarting in AP mode...\"}");
        // Schedule AP mode switch (non-blocking)
        pendingAction = ACTION_WIFI_DISCONNECT;
        pendingActionTime = millis();
    });

    // API: Restart device
    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"success\": true, \"message\": \"Restarting...\"}");
        // Schedule restart (non-blocking)
        pendingAction = ACTION_RESTART;
        pendingActionTime = millis();
    });

    // ============== Filament Database API ==============

    // API: Get complete filament database (brands, types, presets)
    server.on("/api/filaments", HTTP_GET, [](AsyncWebServerRequest *request) {
        String data = loadFilamentDatabase();
        request->send(200, "application/json", data);
    });

    // API: Delete custom brand (register BEFORE /api/filaments/brand to avoid prefix matching)
    server.on("/api/filaments/brand/delete", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            String body = String((char*)data).substring(0, len);
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            String brand = doc["brand"] | "";
            if (brand.length() == 0) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Brand name required\"}");
                return;
            }

            if (deleteCustomBrand(brand)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                request->send(500, "application/json", "{\"success\": false, \"error\": \"Failed to delete\"}");
            }
        }
    );

    // API: Add custom brand
    server.on("/api/filaments/brand", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            String body = String((char*)data).substring(0, len);
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            String brand = doc["brand"] | "";
            if (brand.length() == 0 || brand.length() > 32) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Brand name required (max 32 chars)\"}");
                return;
            }

            if (addCustomBrand(brand)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                request->send(500, "application/json", "{\"success\": false, \"error\": \"Failed to save\"}");
            }
        }
    );

    // API: Delete custom material type (register BEFORE /api/filaments/type to avoid prefix matching)
    server.on("/api/filaments/type/delete", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            String body = String((char*)data).substring(0, len);
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            String type = doc["type"] | "";
            if (type.length() == 0) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Type name required\"}");
                return;
            }

            if (deleteCustomType(type)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                request->send(500, "application/json", "{\"success\": false, \"error\": \"Failed to delete\"}");
            }
        }
    );

    // API: Add custom material type
    server.on("/api/filaments/type", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            String body = String((char*)data).substring(0, len);
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            String type = doc["type"] | "";
            if (type.length() == 0 || type.length() > 16) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Type name required (max 16 chars)\"}");
                return;
            }

            if (addCustomType(type)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                request->send(500, "application/json", "{\"success\": false, \"error\": \"Failed to save\"}");
            }
        }
    );

    // API: Save/Update custom filament preset
    server.on("/api/filaments", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (total > 512) {
                request->send(413, "application/json", "{\"success\": false, \"error\": \"Payload too large\"}");
                return;
            }

            String body = String((char*)data).substring(0, len);
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            String brand = doc["brand"] | "";
            String type = doc["type"] | "";
            int nozzleMin = doc["nozzle_min"] | 200;
            int nozzleMax = doc["nozzle_max"] | 220;
            int bedMin = doc["bed_min"] | 50;
            int bedMax = doc["bed_max"] | 60;

            if (brand.length() == 0 || type.length() == 0) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Brand and type required\"}");
                return;
            }

            if (saveCustomFilament(brand, type, nozzleMin, nozzleMax, bedMin, bedMax)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                request->send(500, "application/json", "{\"success\": false, \"error\": \"Failed to save\"}");
            }
        }
    );

    // API: Delete a custom filament preset
    server.on("/api/filaments/delete", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            String body = String((char*)data).substring(0, len);
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            String brand = doc["brand"] | "";
            String type = doc["type"] | "";

            if (brand.length() == 0 || type.length() == 0) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Brand and type required\"}");
                return;
            }

            if (deleteCustomFilament(brand, type)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                request->send(500, "application/json", "{\"success\": false, \"error\": \"Failed to delete\"}");
            }
        }
    );

    // API: Clear all custom filament data
    server.on("/api/filaments/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        clearFilamentDatabase();
        request->send(200, "application/json", "{\"success\": true, \"message\": \"Filament database cleared\"}");
    });

    // API: Import complete filament database (replaces all)
    server.on("/api/filaments/import", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (total > 8000) {
                request->send(413, "application/json", "{\"success\": false, \"error\": \"Payload too large (max 8KB)\"}");
                return;
            }

            String body = String((char*)data).substring(0, len);
            
            if (saveFilamentDatabase(body)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON or too large\"}");
            }
        }
    );

    // API: Read tag
    server.on("/api/read", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!nfcReady) {
            request->send(503, "application/json", "{\"success\": false, \"error\": \"NFC not ready\"}");
            return;
        }

        String data = readNtagData();
        request->send(200, "application/json", data);
    });

    // API: Write tag
    server.on("/api/write", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            // Limit payload size (NTAG216 max is 888 bytes, add margin for NDEF overhead)
            if (total > MAX_WRITE_PAYLOAD_SIZE) {
                request->send(413, "application/json", "{\"success\": false, \"error\": \"Payload too large\"}");
                return;
            }

            if (!nfcReady) {
                request->send(503, "application/json", "{\"success\": false, \"error\": \"NFC not ready\"}");
                return;
            }

            String jsonData = String((char*)data).substring(0, len);

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, jsonData);
            if (error) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            if (!doc["protocol"].is<const char*>() || !doc["version"].is<const char*>()) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Missing required fields\"}");
                return;
            }

            if (writeNtagData(jsonData)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                String errorResponse = "{\"success\": false, \"error\": \"" + lastError + "\"}";
                request->send(500, "application/json", errorResponse);
            }
        }
    );

    // API: Erase tag
    server.on("/api/erase", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!nfcReady) {
            request->send(503, "application/json", "{\"success\": false, \"error\": \"NFC not ready\"}");
            return;
        }

        String result = eraseNtagData();
        int statusCode = result.indexOf("\"error\"") >= 0 ? 500 : 200;
        request->send(statusCode, "application/json", result);
    });

    // API: Get auto-write status (including last write result)
    server.on("/api/autowrite", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["enabled"] = autoWriteEnabled;
        doc["hasData"] = autoWriteData.length() > 0;
        if (autoWriteData.length() > 0) {
            JsonDocument dataDoc;
            deserializeJson(dataDoc, autoWriteData);
            doc["data"] = dataDoc;
        }
        doc["lastTagUID"] = lastWrittenTagUID;
        
        // Include pending write result if available
        if (autoWriteResultPending) {
            doc["writeResult"]["pending"] = true;
            doc["writeResult"]["success"] = autoWriteResultSuccess;
            doc["writeResult"]["tagUID"] = autoWriteResultTagUID;
            if (!autoWriteResultSuccess) {
                doc["writeResult"]["error"] = autoWriteResultError;
            }
            // Clear the pending flag after it's been read
            autoWriteResultPending = false;
        }
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // API: Enable/configure auto-write
    server.on("/api/autowrite", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (total > MAX_WRITE_PAYLOAD_SIZE) {
                request->send(413, "application/json", "{\"success\": false, \"error\": \"Payload too large\"}");
                return;
            }

            String jsonData = String((char*)data).substring(0, len);
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, jsonData);
            
            if (error) {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Invalid JSON\"}");
                return;
            }

            // Check if this is just an enable/disable toggle
            if (doc["enabled"].is<bool>()) {
                autoWriteEnabled = doc["enabled"].as<bool>();
                
                // If enabling, data must be provided (either now or previously)
                if (autoWriteEnabled && doc["data"].is<JsonObject>()) {
                    String dataStr;
                    serializeJson(doc["data"], dataStr);
                    autoWriteData = dataStr;
                    lastWrittenTagUID = "";  // Reset so new tag can be written
                }
                
                if (autoWriteEnabled && autoWriteData.length() == 0) {
                    autoWriteEnabled = false;
                    request->send(400, "application/json", "{\"success\": false, \"error\": \"No data configured for auto-write\"}");
                    return;
                }
                
                Serial.printf("Auto-write %s\n", autoWriteEnabled ? "ENABLED" : "DISABLED");
                
                JsonDocument response;
                response["success"] = true;
                response["enabled"] = autoWriteEnabled;
                response["hasData"] = autoWriteData.length() > 0;
                
                String responseStr;
                serializeJson(response, responseStr);
                request->send(200, "application/json", responseStr);
            } else {
                request->send(400, "application/json", "{\"success\": false, \"error\": \"Missing 'enabled' field\"}");
            }
        }
    );

    // API: Disable auto-write
    server.on("/api/autowrite", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        autoWriteEnabled = false;
        autoWriteData = "";
        lastWrittenTagUID = "";
        Serial.println("Auto-write DISABLED and data cleared");
        request->send(200, "application/json", "{\"success\": true, \"enabled\": false}");
    });

    // API: OTA Update
    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            bool success = !Update.hasError();
            AsyncWebServerResponse *response = request->beginResponse(200, "application/json",
                success ? "{\"success\": true, \"message\": \"Update successful. Restarting...\"}"
                        : "{\"success\": false, \"error\": \"Update failed\"}");
            response->addHeader("Connection", "close");
            request->send(response);
            if (success) {
                // Schedule restart after OTA (non-blocking)
                pendingAction = ACTION_OTA_RESTART;
                pendingActionTime = millis();
            }
        },
        [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                Serial.printf("OTA Update Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                }
            }
            if (Update.write(data, len) != len) {
                Update.printError(Serial);
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("OTA Update Success: %u bytes\n", index + len);
                } else {
                    Update.printError(Serial);
                }
            }
        }
    );

    // Handle 404
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });

    // Serve static files from LittleFS (MUST be after API routes!)
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.begin();
    Serial.println("Web server started");
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n=== ESP32-OpenSpool ===");
    Serial.println("NTAG21x Reader/Writer (213/215/216)");

    // Initialize LittleFS with recovery
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed, attempting format...");
        if (LittleFS.format()) {
            Serial.println("LittleFS formatted successfully");
            if (LittleFS.begin(false)) {
                Serial.println("LittleFS mounted after format");
            } else {
                Serial.println("LittleFS mount failed after format - web UI unavailable");
            }
        } else {
            Serial.println("LittleFS format failed - web UI unavailable");
        }
    } else {
        Serial.println("LittleFS mounted");
    }

    // Setup Buzzer
    setupBuzzer();

    // Setup WiFi (AP or connect to saved network)
    setupWiFi();

    // Setup NFC
    setupNFC();

    // Setup Web Server
    setupWebServer();

    Serial.println("\n=== Ready ===");
    beepSuccess();  // Ready beep
}

void loop() {
    // Process pending actions (deferred from async handlers)
    if (pendingAction != ACTION_NONE && millis() - pendingActionTime >= ACTION_DELAY_MS) {
        PendingAction action = pendingAction;
        pendingAction = ACTION_NONE;
        
        switch (action) {
            case ACTION_WIFI_CONNECT:
                Serial.println("Executing deferred WiFi connect...");
                startAPSTA(pendingSSID, pendingPassword);
                pendingSSID = "";
                pendingPassword = "";
                break;
            case ACTION_WIFI_DISCONNECT:
                Serial.println("Executing deferred WiFi disconnect...");
                startAP();
                break;
            case ACTION_RESTART:
            case ACTION_OTA_RESTART:
                Serial.println("Executing deferred restart...");
                ESP.restart();
                break;
            default:
                break;
        }
    }

    // Check WiFi connection status periodically
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > WIFI_CHECK_INTERVAL_MS) {
        lastCheck = millis();

        if (currentWifiState == WIFI_MODE_AP_AND_STA || currentWifiState == WIFI_MODE_STA_ONLY) {
            if (WiFi.status() != WL_CONNECTED && wifiConnected) {
                Serial.println("WiFi connection lost, attempting reconnect...");
                wifiConnected = false;

                String savedSSID, savedPassword;
                if (loadWiFiCredentials(savedSSID, savedPassword)) {
                    startAPSTA(savedSSID, savedPassword);
                }
            }
        }
    }

    // Auto-write feature: check for tags and write automatically
    if (autoWriteEnabled && nfcReady && autoWriteData.length() > 0) {
        static unsigned long lastAutoWriteCheck = 0;
        if (millis() - lastAutoWriteCheck > 500) {  // Check every 500ms
            lastAutoWriteCheck = millis();
            
            uint8_t uid[7];
            uint8_t uidLength;
            
            // Try to detect a tag (short timeout)
            nfc.setPassiveActivationRetries(2);
            if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
                // Convert UID to string for comparison
                String currentTagUID = "";
                for (uint8_t i = 0; i < uidLength; i++) {
                    if (uid[i] < 0x10) currentTagUID += "0";
                    currentTagUID += String(uid[i], HEX);
                }
                currentTagUID.toUpperCase();
                
                // Check if this is a new tag or cooldown has passed
                bool shouldWrite = (currentTagUID != lastWrittenTagUID) || 
                                   (millis() - lastWriteTime > AUTO_WRITE_COOLDOWN_MS);
                
                if (shouldWrite) {
                    Serial.println("\n=== Auto-Write Triggered ===");
                    Serial.print("Tag UID: ");
                    Serial.println(currentTagUID);
                    
                    beepTagDetected();
                    
                    if (writeNtagData(autoWriteData)) {
                        Serial.println("Auto-write SUCCESS!");
                        beepSuccess();
                        lastWrittenTagUID = currentTagUID;
                        lastWriteTime = millis();
                        
                        // Store result for web notification
                        autoWriteResultPending = true;
                        autoWriteResultSuccess = true;
                        autoWriteResultTagUID = currentTagUID;
                        autoWriteResultError = "";
                        autoWriteResultTime = millis();
                    } else {
                        Serial.print("Auto-write FAILED: ");
                        Serial.println(lastError);
                        beepError();
                        
                        // Store result for web notification
                        autoWriteResultPending = true;
                        autoWriteResultSuccess = false;
                        autoWriteResultTagUID = currentTagUID;
                        autoWriteResultError = lastError;
                        autoWriteResultTime = millis();
                    }
                }
            }
        }
    }

    delay(LOOP_DELAY_MS);
}
