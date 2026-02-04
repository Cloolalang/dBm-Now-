/* * ======================================================================================
 * ESP32 RF PROBE & PATH LOSS ANALYZER | v4.18 (JP Channel 14 Mod)
 * ======================================================================================
 */

#define FW_VERSION "4.18"

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h> 
#include <time.h>
#include <sys/time.h>

// --- HARDWARE ROLE CONFIG ---
const int ROLE_PIN = 13;
bool isMaster = false;
String deviceRole = "UNKNOWN";

// --- RF MODES ---
enum RFMode { MODE_STD = 0, MODE_LR_250K = 1, MODE_LR_500K = 2 };
uint8_t currentRFMode = MODE_STD; 

// --- GLOBAL VARIABLES ---
Preferences prefs;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; 
const int ledPin = 2;

float currentPower = -99.0;
unsigned long ledTimer = 0;

// Master Specific
bool plotMode = false, calibrated = false;
uint32_t burstDelay = 1000, nonceCounter = 0;
float remoteTargetPower = -1.0, referenceRSSI = 0;
unsigned long lastStatusPrint = 0, lastPingTime = 0;
bool waitingForPong = false, pendingRX = false;
unsigned long nextPingTime = 0; 

// Rolling Minute Counters
float lastKnownRSSI = -100.0;
uint32_t rangeMinCounter = 0, interfMinCounter = 0;
unsigned long minuteTimer = 0;
String linkCondition = "STABLE";

// Transponder Specific
unsigned long lastPacketTime = 0;
uint32_t transponderTimeout = 5000; 

enum LEDState { IDLE, TX_FLASH, GAP, RX_FLASH };
LEDState currentLEDState = IDLE;

typedef struct {
    uint32_t nonce; 
    float txPower; 
    float measuredRSSI; 
    float targetPower;
    uint32_t pingInterval; 
    uint8_t hour; uint8_t minute; uint8_t second;
} Payload;
Payload myData, txData, rxData;

// --- CORE RF LOGIC ---

void applyRFSettings(uint8_t mode) {
    esp_now_deinit();
    
    // 1. SET COUNTRY CODE TO JAPAN
    wifi_country_t country = {
        .cc = "JP",
        .schan = 1,
        .nchan = 14,
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };
    esp_wifi_set_country(&country);

    if (mode == MODE_STD) {
        // Note: Channel 14 is technically 802.11b only. 
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B); 
        if (!plotMode) Serial.println(">> Protocol: STANDARD (802.11b) | Region: JP | CH: 14");
    } else {
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
        wifi_phy_rate_t rate = (mode == MODE_LR_500K) ? WIFI_PHY_RATE_LORA_500K : WIFI_PHY_RATE_LORA_250K;
        esp_wifi_config_espnow_rate(WIFI_IF_STA, rate);
        if (!plotMode) Serial.printf(">> Protocol: LONG RANGE (%s) | CH: 14\n", (mode == MODE_LR_500K) ? "500kbps" : "250kbps");
    }

    // 2. FORCE CHANNEL TO 14
    esp_wifi_set_channel(14, WIFI_SECOND_CHAN_NONE);

    esp_now_init();
    esp_now_register_recv_cb(onDataRecv);
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 14; // Set peer to channel 14
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
}

// Transponder uses this to "hunt" for the master's current setting
void cycleTransponderProtocol() {
    currentRFMode = (currentRFMode + 1) % 3; 
    applyRFSettings(currentRFMode);
}

// --- UTILITIES ---

String getFastTimestamp() {
    time_t now; time(&now); struct tm ti; localtime_r(&now, &ti);
    char buf[15]; snprintf(buf, sizeof(buf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
    return String(buf);
}

void setESP32Time(int hr, int min) {
    struct tm tm = {0}; tm.tm_year = 2026 - 1900; tm.tm_mon = 0; tm.tm_mday = 1;
    tm.tm_hour = hr; tm.tm_min = min; tm.tm_sec = 0;
    time_t t = mktime(&tm); struct timeval now = { .tv_sec = t }; settimeofday(&now, NULL);
}

void setPower(float pwr) {
    if (pwr == currentPower) return;
    currentPower = pwr;
    int8_t pwr_val = (pwr >= 19.5) ? 78 : (pwr >= 11 ? 44 : 8);
    esp_wifi_set_max_tx_power(pwr_val);
    delay(20); 
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incoming, int len) {
    lastPacketTime = millis(); 
    if (isMaster) {
        Payload pong; memcpy(&pong, incoming, sizeof(pong));
        waitingForPong = false; pendingRX = true; 
        lastKnownRSSI = (float)info->rx_ctrl->rssi;
        linkCondition = "STABLE"; 
        float mRSSI = lastKnownRSSI; 
        float tRSSI = pong.measuredRSSI;         
        float fwdLoss = currentPower - tRSSI;
        float t_tx_eff = (pong.txPower != 0) ? pong.txPower : remoteTargetPower;
        float bwdLoss = t_tx_eff - mRSSI;
        float symmetry = fwdLoss - bwdLoss;
        if (!calibrated && referenceRSSI == 1.0) { referenceRSSI = mRSSI; calibrated = true; }
        float zeroed = calibrated ? (mRSSI - referenceRSSI) : 0;

        if (!plotMode) {
            Serial.printf("[%s] N:%u | FWD Loss:%.1f (R:%.0f) | BWD Loss:%.1f (R:%.0f) | Sym:%.1f | Z:%.1f\n", 
                          getFastTimestamp().c_str(), nonceCounter, fwdLoss, tRSSI, bwdLoss, mRSSI, symmetry, zeroed);
        } else {
            Serial.printf("%.1f,%.1f,%.1f,%.1f\n", fwdLoss, bwdLoss, symmetry, zeroed);
        }
    } else {
        memcpy(&rxData, incoming, sizeof(rxData));
        setESP32Time(rxData.hour, rxData.minute);
        uint32_t newTimeout = (rxData.pingInterval * 3 > 5000) ? (rxData.pingInterval * 3) : 5000;
        transponderTimeout = newTimeout;
        if (!esp_now_is_peer_exist(info->src_addr)) {
            esp_now_peer_info_t peerInfo = {};
            memcpy(peerInfo.peer_addr, info->src_addr, 6);
            peerInfo.channel = 14; 
            peerInfo.encrypt = false;
            esp_now_add_peer(&peerInfo);
        }
        setPower(rxData.targetPower);
        txData.nonce = rxData.nonce;
        txData.txPower = currentPower;
        txData.measuredRSSI = (float)info->rx_ctrl->rssi; 
        esp_now_send(info->src_addr, (uint8_t *) &txData, sizeof(txData));
        digitalWrite(ledPin, HIGH); ledTimer = millis() + 40;
    }
}

void printDetailedStatus() {
    if (plotMode) return;
    String modeStr = "STANDARD (802.11b)";
    if (currentRFMode == MODE_LR_250K) modeStr = "LR (250kbps)";
    if (currentRFMode == MODE_LR_500K) modeStr = "LR (500kbps)";

    Serial.println("\n==================================================");
    Serial.printf("   ESP32 RF PROBE | v%s | ROLE: %s\n", FW_VERSION, deviceRole.c_str());
    Serial.println("==================================================");
    Serial.printf("  RF PROTOCOL : %s\n", modeStr.c_str());
    Serial.printf("  REGION/CH   : JAPAN / CHANNEL 14\n");
    
    if (isMaster) {
        Serial.printf("  LINK STATUS : %s\n", linkCondition.c_str());
        Serial.printf("  MASTER PWR  : %.1f dBm | REMOTE: %.1f dBm\n", currentPower, remoteTargetPower);
        Serial.println("--------------------------------------------------");
        Serial.println("  [l] Toggle Mode    : Cycle STD -> 250k -> 500k");
        Serial.println("  [p/t] Pwr Mstr/Rem : Set Power (e.g., 'p14')");
    }
    Serial.println("==================================================\n");
}

void setup() {
    Serial.begin(115200);
    pinMode(ledPin, OUTPUT);
    pinMode(ROLE_PIN, INPUT_PULLUP);
    delay(50);
    isMaster = (digitalRead(ROLE_PIN) == LOW);
    deviceRole = isMaster ? "MASTER" : "TRANSPONDER";
    
    WiFi.mode(WIFI_STA);
    if (isMaster) {
        prefs.begin("probe", true); 
        currentRFMode = prefs.getUChar("rfm", 0); 
        prefs.end();
        minuteTimer = millis();
    }
    
    applyRFSettings(currentRFMode);
    setPower(-1.0);
    lastPacketTime = millis();
    printDetailedStatus();
}

void loop() {
    if (millis() - lastStatusPrint >= 10000) { lastStatusPrint = millis(); printDetailedStatus(); }

    if (isMaster) {
        handleLED();
        if (millis() - minuteTimer >= 60000) {
            Serial.printf("\n>>> [MINUTE SUMMARY] Interference: %u | Range Limit: %u <<<\n\n", interfMinCounter, rangeMinCounter);
            interfMinCounter = 0; rangeMinCounter = 0; minuteTimer = millis();
        }

        if (Serial.available() > 0) {
            char cmd = Serial.read();
            if (cmd == 'k') { int val = Serial.parseInt(); setESP32Time(val / 100, val % 100); }
            else {
                float val = Serial.parseFloat();
                switch (cmd) {
                    case 'l': 
                        currentRFMode = (currentRFMode + 1) % 3;
                        prefs.begin("probe", false); prefs.putUChar("rfm", currentRFMode); prefs.end(); 
                        delay(100); ESP.restart(); 
                        break;
                    case 'p': setPower(val); break;
                    case 't': remoteTargetPower = val; break;
                    case 'v': plotMode = !plotMode; break;
                    case 'r': burstDelay = (uint32_t)val; break;
                    case 'h': printDetailedStatus(); break;
                    case 's': remoteTargetPower = currentPower; break;
                    case 'c': interfMinCounter = 0; rangeMinCounter = 0; Serial.println(">> Stats Reset."); break;
                    case 'z': calibrated = false; referenceRSSI = 1.0; break;                  
                }
            }
        }

        if (millis() >= nextPingTime) {
            nextPingTime = millis() + burstDelay + random(0, 50);
            if (waitingForPong && !plotMode) { 
                if (lastKnownRSSI > -80.0) { linkCondition = "INTERFERENCE"; interfMinCounter++; }
                else { linkCondition = "RANGE LIMIT"; rangeMinCounter++; }
                Serial.printf("[%s] N:%u | [NO REPLY] | %s\n", getFastTimestamp().c_str(), nonceCounter, linkCondition.c_str()); 
            }
            lastPingTime = millis(); nonceCounter++; waitingForPong = true; 
            time_t now; time(&now); struct tm ti; localtime_r(&now, &ti);
            myData.nonce = nonceCounter; myData.txPower = currentPower; myData.targetPower = remoteTargetPower;
            myData.pingInterval = burstDelay; myData.hour = ti.tm_hour; myData.minute = ti.tm_min; myData.second = ti.tm_sec;
            esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
            digitalWrite(ledPin, HIGH); ledTimer = millis() + 30; currentLEDState = TX_FLASH;
        }
    } else {
        if (millis() - lastPacketTime > transponderTimeout) { 
            lastPacketTime = millis(); 
            cycleTransponderProtocol(); 
        }
        if (ledTimer != 0 && millis() >= ledTimer) { digitalWrite(ledPin, LOW); ledTimer = 0; }
    }
}

void handleLED() {
    if (!isMaster || ledTimer == 0 || millis() < ledTimer) return;
    switch (currentLEDState) {
        case TX_FLASH: digitalWrite(ledPin, LOW); ledTimer = millis() + 40; currentLEDState = GAP; break;
        case GAP: if (pendingRX) { digitalWrite(ledPin, HIGH); ledTimer = millis() + 50; currentLEDState = RX_FLASH; pendingRX = false; } 
                  else { currentLEDState = IDLE; ledTimer = 0; } break;
        case RX_FLASH: digitalWrite(ledPin, LOW); currentLEDState = IDLE; ledTimer = 0; break;
        default: break;
    }
}