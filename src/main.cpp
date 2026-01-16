/**
 * ESP32-OpenSpool
 * NTAG21x Reader/Writer for OpenSpool Protocol
 *
 * Supported tags: NTAG213, NTAG215, NTAG216
 *
 * Hardware: ESP32-S3 Zero + PN532 RFID Module (I2C)
 *
 * Wiring (I2C):
 *   PN532 SDA -> GPIO8
 *   PN532 SCL -> GPIO9
 *   PN532 VCC -> 3.3V
 *   PN532 GND -> GND
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

// WiFi AP Settings (Fallback/Setup Mode)
const char* AP_SSID = "OpenSpool";
const char* AP_PASS = "openspool";
const char* MDNS_HOSTNAME = "openspool";

// SPI Pins for ESP32-S3 Zero
#define PN532_SCK  12
#define PN532_MISO 13
#define PN532_MOSI 11
#define PN532_SS   10

// WiFi connection timeout (ms)
#define WIFI_TIMEOUT 15000

// PN532 Setup (Software SPI - more compatible)
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

// Web Server
AsyncWebServer server(80);

// Preferences for storing WiFi credentials
Preferences preferences;

// Global state
bool nfcReady = false;
String lastError = "";
String lastTagData = "";

// WiFi state
enum WifiState { WIFI_MODE_AP_ONLY, WIFI_MODE_STA_ONLY, WIFI_MODE_AP_AND_STA };
WifiState currentWifiState = WIFI_MODE_AP_ONLY;
String currentSSID = "";
String currentIP = "";
bool wifiConnected = false;

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
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);

    currentWifiState = WIFI_MODE_AP_ONLY;
    currentSSID = AP_SSID;
    currentIP = WiFi.softAPIP().toString();
    wifiConnected = false;

    startMDNS();

    Serial.println("\n=== Access Point Mode ===");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Password: ");
    Serial.println(AP_PASS);
    Serial.print("IP: ");
    Serial.println(currentIP);
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
bool scanInProgress = false;

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
        xTaskCreatePinnedToCore(scanTask, "scanTask", 4096, NULL, 1, NULL, 0);
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
    delay(100);  // Let PN532 power up

    // Try multiple times to connect
    uint32_t versiondata = 0;
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("NFC init attempt %d/3...\n", attempt);
        nfc.begin();
        delay(100);
        versiondata = nfc.getFirmwareVersion();
        if (versiondata) {
            break;  // Success!
        }
        delay(300);  // Wait before retry
    }

    if (!versiondata) {
        Serial.println("PN532 not found after 5 attempts!");
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

bool waitForTag(uint8_t* uid, uint8_t* uidLength, uint16_t timeout = 1000) {
    nfc.setPassiveActivationRetries(timeout / 100);
    return nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLength);
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

String readNtagData() {
    uint8_t uid[7];
    uint8_t uidLength;

    Serial.println("readNtagData: waiting for tag...");
    if (!waitForTag(uid, &uidLength)) {
        Serial.println("readNtagData: no tag found");
        return "{\"error\": \"No tag found\"}";
    }
    Serial.println("readNtagData: tag found, detecting type...");

    // Detect tag type
    NtagType tagType = detectNtagType();
    Serial.printf("readNtagData: tag type = %d\n", tagType);
    if (tagType == NTAG_UNKNOWN) {
        return "{\"error\": \"Unknown tag type\"}";
    }

    int userEnd = getNtagUserEnd(tagType);
    int capacity = getNtagUserBytes(tagType);
    String uidStr = uidToString(uid, uidLength);
    String tagTypeName = getNtagTypeName(tagType);

    String data = "";
    uint8_t pageData[4];

    for (int page = NTAG_USER_START; page <= userEnd; page++) {
        if (nfc.ntag2xx_ReadPage(page, pageData)) {
            for (int i = 0; i < 4; i++) {
                if (pageData[i] == 0x00 || pageData[i] == 0xFE) {
                    goto done;
                }
                data += (char)pageData[i];
            }
        } else {
            break;
        }
    }

done:
    JsonDocument responseDoc;
    responseDoc["tag"]["uid"] = uidStr;
    responseDoc["tag"]["type"] = tagTypeName;
    responseDoc["tag"]["capacity"] = capacity;

    int jsonStart = data.indexOf('{');
    int jsonEnd = data.lastIndexOf('}');

    if (jsonStart >= 0 && jsonEnd > jsonStart) {
        String jsonData = data.substring(jsonStart, jsonEnd + 1);
        responseDoc["tag"]["used"] = jsonData.length();

        // Parse the data and include it
        JsonDocument dataDoc;
        if (deserializeJson(dataDoc, jsonData) == DeserializationError::Ok) {
            responseDoc["data"] = dataDoc;
        } else {
            responseDoc["data"] = jsonData;
        }
    } else if (data.length() == 0) {
        responseDoc["tag"]["used"] = 0;
        responseDoc["data"] = nullptr;
        responseDoc["message"] = "Tag is empty";
    } else {
        responseDoc["tag"]["used"] = 0;
        responseDoc["error"] = "No valid JSON found on tag";
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
        return false;
    }

    // Detect tag type
    NtagType tagType = detectNtagType();
    if (tagType == NTAG_UNKNOWN) {
        lastError = "Unknown tag type";
        return false;
    }

    int userEnd = getNtagUserEnd(tagType);
    int maxBytes = getNtagUserBytes(tagType);

    String payload = jsonData;
    int dataLen = payload.length();

    if (dataLen > maxBytes - 10) {
        lastError = "Data too long for tag (" + String(dataLen) + " bytes, max " + String(maxBytes - 10) + ")";
        return false;
    }

    uint8_t header[7];
    int headerLen;

    if (dataLen < 255) {
        header[0] = 0x03;
        header[1] = dataLen + 3;
        header[2] = 0xD1;
        header[3] = 0x01;
        header[4] = dataLen;
        header[5] = 'T';
        headerLen = 6;
    } else {
        header[0] = 0x03;
        header[1] = 0xFF;
        header[2] = ((dataLen + 3) >> 8) & 0xFF;
        header[3] = (dataLen + 3) & 0xFF;
        header[4] = 0xD1;
        header[5] = 0x01;
        header[6] = dataLen > 255 ? 255 : dataLen;
        headerLen = 7;
    }

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

    pageData[byteIndex++] = 0xFE;
    while (byteIndex < 4) {
        pageData[byteIndex++] = 0x00;
    }
    if (!nfc.ntag2xx_WritePage(page, pageData)) {
        lastError = "Write failed at terminator";
        return false;
    }

    return true;
}

// Erase tag by writing zeros to user memory
String eraseNtagData() {
    uint8_t uid[7];
    uint8_t uidLength;

    if (!waitForTag(uid, &uidLength)) {
        return "{\"error\": \"No tag found\"}";
    }

    // Detect tag type
    NtagType tagType = detectNtagType();
    if (tagType == NTAG_UNKNOWN) {
        return "{\"error\": \"Unknown tag type\"}";
    }

    int userEnd = getNtagUserEnd(tagType);
    String uidStr = uidToString(uid, uidLength);
    String tagTypeName = getNtagTypeName(tagType);

    uint8_t emptyPage[4] = {0x00, 0x00, 0x00, 0x00};

    // Write terminator to first user page, then zeros
    uint8_t terminatorPage[4] = {0x03, 0x00, 0xFE, 0x00};  // Empty NDEF message
    if (!nfc.ntag2xx_WritePage(NTAG_USER_START, terminatorPage)) {
        return "{\"error\": \"Failed to write terminator\"}";
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

    String result;
    serializeJson(doc, result);
    return result;
}

void setupWebServer() {
    // API: Get combined status (NFC + WiFi)
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
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
            String body = String((char*)data).substring(0, len);

            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                request->send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
                return;
            }

            String ssid = doc["ssid"] | "";
            String password = doc["password"] | "";

            if (ssid.length() == 0) {
                request->send(400, "application/json", "{\"error\": \"SSID required\"}");
                return;
            }

            // Save credentials
            saveWiFiCredentials(ssid, password);

            request->send(200, "application/json", "{\"success\": true, \"message\": \"Credentials saved. Reconnecting...\"}");

            // Reconnect with new credentials after response is sent
            delay(500);
            startAPSTA(ssid, password);
        }
    );

    // API: Disconnect and clear credentials
    server.on("/api/wifi/disconnect", HTTP_POST, [](AsyncWebServerRequest *request) {
        clearWiFiCredentials();
        request->send(200, "application/json", "{\"success\": true, \"message\": \"Credentials cleared. Restarting in AP mode...\"}");
        delay(500);
        startAP();
    });

    // API: Restart device
    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"success\": true, \"message\": \"Restarting...\"}");
        delay(500);
        ESP.restart();
    });

    // API: Read tag
    server.on("/api/read", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!nfcReady) {
            request->send(503, "application/json", "{\"error\": \"NFC not ready\"}");
            return;
        }

        String data = readNtagData();
        lastTagData = data;
        request->send(200, "application/json", data);
    });

    // API: Write tag
    server.on("/api/write", HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (!nfcReady) {
                request->send(503, "application/json", "{\"error\": \"NFC not ready\"}");
                return;
            }

            String jsonData = String((char*)data).substring(0, len);

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, jsonData);
            if (error) {
                request->send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
                return;
            }

            if (!doc["protocol"].is<const char*>() || !doc["version"].is<const char*>()) {
                request->send(400, "application/json", "{\"error\": \"Missing required fields\"}");
                return;
            }

            if (writeNtagData(jsonData)) {
                request->send(200, "application/json", "{\"success\": true}");
            } else {
                String errorResponse = "{\"error\": \"" + lastError + "\"}";
                request->send(500, "application/json", errorResponse);
            }
        }
    );

    // API: Erase tag
    server.on("/api/erase", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!nfcReady) {
            request->send(503, "application/json", "{\"error\": \"NFC not ready\"}");
            return;
        }

        String result = eraseNtagData();
        request->send(200, "application/json", result);
    });

    // API: OTA Update
    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            bool success = !Update.hasError();
            AsyncWebServerResponse *response = request->beginResponse(200, "application/json",
                success ? "{\"success\": true, \"message\": \"Update successful. Restarting...\"}"
                        : "{\"error\": \"Update failed\"}");
            response->addHeader("Connection", "close");
            request->send(response);
            if (success) {
                delay(500);
                ESP.restart();
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

    // Initialize LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");
    } else {
        Serial.println("LittleFS mounted");
    }

    // Setup WiFi (AP or connect to saved network)
    setupWiFi();

    // Setup NFC
    setupNFC();

    // Setup Web Server
    setupWebServer();

    Serial.println("\n=== Ready ===");
}

void loop() {
    // Check WiFi connection status periodically
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 30000) {  // Every 30 seconds
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

    delay(100);
}
