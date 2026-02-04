/* * ======================================================================================
 * ESP32 RF PROBE & PATH LOSS ANALYZER | v2.5 (Bitrate Selectable)
 * ======================================================================================
 * [l] Toggle Mode : STD -> LR 250kbps -> LR 500kbps
 */

#define FW_VERSION "2.5"

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <FS.h>
#include <SPIFFS.h>
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

// CSV file logging (master + transponder; same path, role-specific columns)
const char* CSV_PATH = "/log.csv";
bool csvFileLogging = false;
File csvFile;
uint32_t maxRecordingTimeSec = 0;   // 0 = no limit; auto-stop after this many seconds
unsigned long csvLogStartTime = 0;   // set when logging starts 

// Rolling Minute Counters
float lastKnownRSSI = -100.0;
uint32_t rangeMinCounter = 0, interfMinCounter = 0;
unsigned long minuteTimer = 0;
String linkCondition = "STABLE";

// RF channel 1–14. Both devices boot on 1 for quick sync. Master sets via Serial; transponder follows from payload.
uint8_t wifiChannel = 1;

// Transponder Specific
unsigned long lastPacketTime = 0;
uint32_t transponderTimeout = 5000;
#define TRANSPONDER_CYCLE_AFTER_TIMEOUTS 3   // require this many consecutive timeouts before hunting (avoids cycling on brief fades)
uint32_t transponderConsecutiveTimeouts = 0; 

enum LEDState { IDLE, TX_FLASH, GAP, RX_FLASH };
LEDState currentLEDState = IDLE;

typedef struct {
    uint32_t nonce; 
    float txPower; 
    float measuredRSSI; 
    float targetPower;
    uint32_t pingInterval; 
    uint8_t hour; uint8_t minute; uint8_t second;
    uint8_t channel;  // RF channel 1–14
} Payload;
Payload myData, txData, rxData;

// Forward declarations (needed when built as C++ e.g. PlatformIO; Arduino .ino ignores)
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incoming, int len);
void handleLED();
void csvLogStart();
void csvLogStop();
void csvLogDump();
void csvLogErase();

// --- CORE RF LOGIC ---

void applyRFSettings(uint8_t mode) {
    esp_now_deinit();

    if (mode == MODE_STD) {
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        if (!plotMode) Serial.println(">> Protocol: STANDARD (802.11)");
    } else {
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
        wifi_phy_rate_t rate = (mode == MODE_LR_500K) ? WIFI_PHY_RATE_LORA_500K : WIFI_PHY_RATE_LORA_250K;
        esp_wifi_config_espnow_rate(WIFI_IF_STA, rate);
        if (!plotMode) Serial.printf(">> Protocol: LONG RANGE (%s)\n", (mode == MODE_LR_500K) ? "500kbps" : "250kbps");
    }

    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);

    esp_now_init();
    esp_now_register_recv_cb(onDataRecv);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = wifiChannel;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
}

// Transponder uses this to "hunt" for the master's channel and RF mode when no packet received
static uint32_t transponderTimeoutCount = 0;
void cycleTransponderProtocol() {
    transponderTimeoutCount++;
    wifiChannel = (wifiChannel % 14) + 1;  // cycle 1..14 so transponder can find master after channel change
    if (transponderTimeoutCount % 14 == 0)
        currentRFMode = (currentRFMode + 1) % 3;  // cycle RF mode after trying all channels
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

void csvLogStart() {
    if (csvFileLogging) return;
    if (!SPIFFS.begin(true)) {
        Serial.println(">> CSV: SPIFFS mount failed.");
        return;
    }
    csvFile = SPIFFS.open(CSV_PATH, "a");
    if (!csvFile) {
        Serial.println(">> CSV: open failed.");
        return;
    }
    if (csvFile.size() == 0) {
        if (isMaster)
            csvFile.println("timestamp,nonce,fwdLoss,bwdLoss,symmetry,zeroed,masterRSSI,remoteRSSI");
        else
            csvFile.println("timestamp,nonce,rfMode,rssi,masterPwr,pathLoss,transponderPwr");
    }
    csvFile.flush();
    csvFileLogging = true;
    csvLogStartTime = millis();
    Serial.printf(">> CSV logging ON -> %s", CSV_PATH);
    if (maxRecordingTimeSec > 0) Serial.printf(" (max %u s)", maxRecordingTimeSec);
    Serial.println();
}

void csvLogStop() {
    if (!csvFileLogging) return;
    csvFile.close();
    csvFileLogging = false;
    Serial.println(">> CSV logging OFF.");
}

void csvLogDump() {
    if (csvFileLogging) { Serial.println(">> Stop CSV log first (f)."); return; }
    if (!SPIFFS.begin(true)) { Serial.println(">> CSV: SPIFFS mount failed."); return; }
    File f = SPIFFS.open(CSV_PATH, "r");
    if (!f || f.isDirectory()) { Serial.println(">> CSV: no file or empty."); return; }
    Serial.printf(">> --- %s ---\n", CSV_PATH);
    while (f.available()) Serial.write(f.read());
    f.close();
    Serial.println(">> --- end ---");
}

void csvLogErase() {
    csvLogStop();
    if (SPIFFS.begin(true) && SPIFFS.exists(CSV_PATH)) {
        SPIFFS.remove(CSV_PATH);
        Serial.println(">> CSV file erased.");
    }
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incoming, int len) {
    if (len < (int)sizeof(Payload)) return;
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
        if (csvFileLogging && csvFile) {
            if (maxRecordingTimeSec > 0 && (millis() - csvLogStartTime) >= (unsigned long)maxRecordingTimeSec * 1000) {
                csvLogStop();
                Serial.println(">> CSV logging stopped (max time reached).");
            } else {
                csvFile.printf("%s,%u,%.1f,%.1f,%.1f,%.1f,%.0f,%.0f\n",
                    getFastTimestamp().c_str(), nonceCounter, fwdLoss, bwdLoss, symmetry, zeroed, mRSSI, tRSSI);
                csvFile.flush();
            }
        }
    } else {
        transponderConsecutiveTimeouts = 0;   // any received packet resets "lost" count
        memcpy(&rxData, incoming, sizeof(rxData));
        float rssi = (float)info->rx_ctrl->rssi;
        float pathLoss = rxData.txPower - rssi;
        const char* rfModeStr = (currentRFMode == MODE_STD) ? "STD" : (currentRFMode == MODE_LR_250K) ? "LR 250k" : "LR 500k";
        if (Serial) Serial.printf("[%s] RX N=%u | %s | RSSI:%.0f dBm | Mstr Pwr:%.1f dBm | Path Loss:%.1f dB\n",
            getFastTimestamp().c_str(), rxData.nonce, rfModeStr, rssi, rxData.txPower, pathLoss);
        if (csvFileLogging && csvFile) {
            if (maxRecordingTimeSec > 0 && (millis() - csvLogStartTime) >= (unsigned long)maxRecordingTimeSec * 1000) {
                csvLogStop();
                if (Serial) Serial.println(">> CSV logging stopped (max time reached).");
            } else {
                csvFile.printf("%s,%u,%s,%.0f,%.1f,%.1f,%.1f\n",
                    getFastTimestamp().c_str(), rxData.nonce, rfModeStr, rssi, rxData.txPower, pathLoss, currentPower);
                csvFile.flush();
            }
        }
        setESP32Time(rxData.hour, rxData.minute);
        if (rxData.channel >= 1 && rxData.channel <= 14 && rxData.channel != wifiChannel) {
            wifiChannel = rxData.channel;
            esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
        }
        uint32_t newTimeout = (rxData.pingInterval * 3 > 5000) ? (rxData.pingInterval * 3) : 5000;
        transponderTimeout = newTimeout;
        if (!esp_now_is_peer_exist(info->src_addr)) {
            esp_now_peer_info_t peerInfo = {};
            memcpy(peerInfo.peer_addr, info->src_addr, 6);
            peerInfo.channel = wifiChannel;
            peerInfo.encrypt = false;
            esp_now_add_peer(&peerInfo);
        }
        setPower(rxData.targetPower);
        txData.nonce = rxData.nonce;
        txData.txPower = currentPower;
        txData.channel = wifiChannel;
        txData.measuredRSSI = (float)info->rx_ctrl->rssi; 
        esp_now_send(info->src_addr, (uint8_t *) &txData, sizeof(txData));
        digitalWrite(ledPin, HIGH); ledTimer = millis() + 40;
    }
}

void printDetailedStatus() {
    if (plotMode) return;
    String modeStr = "STANDARD (802.11)";
    if (currentRFMode == MODE_LR_250K) modeStr = "LR (250kbps)";
    if (currentRFMode == MODE_LR_500K) modeStr = "LR (500kbps)";

    Serial.println("\n==================================================");
    Serial.printf("   ESP32 RF PROBE | v%s | ROLE: %s\n", FW_VERSION, deviceRole.c_str());
    Serial.println("==================================================");
    Serial.printf("  RF PROTOCOL : %s\n", modeStr.c_str());
    Serial.printf("  RF CHANNEL  : %u (1-14)\n", wifiChannel);

    if (isMaster) {
        Serial.printf("  LINK STATUS : %s\n", linkCondition.c_str());
        Serial.printf("  MASTER PWR  : %.1f dBm | REMOTE: %.1f dBm\n", currentPower, remoteTargetPower);
        Serial.printf("  CSV LOG     : %s\n", csvFileLogging ? "ON" : "OFF");
        Serial.printf("  CSV MAX    : %u s (0=no limit)\n", maxRecordingTimeSec);
        Serial.println("--------------------------------------------------");
        Serial.println("  [l] Toggle Mode    : Cycle STD -> 250k -> 500k");
        Serial.println("  [0] Force STD      : Set RF to 802.11b and restart (resync with transponder)");
        Serial.println("  [p] Master Power   : Set TX power dBm (e.g. p14)");
        Serial.println("  [t] Remote Power   : Set remote target dBm (e.g. t8)");
        Serial.println("  [s] Sync Remote    : Set remote target = current master power");
        Serial.println("  [r] Set Rate       : Ping interval ms (e.g. r1000)");
        Serial.println("  [v] Plotter Mode   : Toggle CSV output");
        Serial.println("  [h] Help           : Show this status");
        Serial.println("  [k] Set Time       : HHMM (e.g. k1430)");
        Serial.println("  [z] Zero Cal       : Reset calibration reference");
        Serial.println("  [c] Reset Stats    : Clear interference/range counters");
        Serial.println("  [x] Reset RF pref  : Clear saved RF mode (next boot = STD), restart");
        Serial.printf("  [f] CSV file log  : Toggle logging to %s (SPIFFS)\n", CSV_PATH);
        Serial.println("  [d] Dump CSV      : Print log file to Serial (copy to save)");
        Serial.println("  [e] Erase CSV     : Delete log file for fresh start");
        Serial.println("  [m] Max record    : Set max record time in seconds (0=no limit), e.g. m300");
        Serial.println("  [n] Set channel   : RF channel 1-14, e.g. n6 (transponder follows)");
    } else {
        Serial.printf("  TX PWR    : %.1f dBm (transponder transmit power, set by master)\n", currentPower);
        Serial.printf("  RF CHANNEL: %u (follows master)\n", wifiChannel);
        Serial.printf("  TIMEOUT   : %u ms (cycle RF mode if no ping)\n", transponderTimeout);
        Serial.printf("  CSV LOG   : %s (master→TX reception)\n", csvFileLogging ? "ON" : "OFF");
        Serial.printf("  CSV MAX   : %u s (0=no limit)\n", maxRecordingTimeSec);
        Serial.println("  (RX lines: timestamp | mode | RSSI | path loss)");
        Serial.println("--------------------------------------------------");
        Serial.printf("  [f] CSV file log  : Toggle logging to %s (SPIFFS)\n", CSV_PATH);
        Serial.println("  [d] Dump CSV      : Print log file to Serial (copy to save)");
        Serial.println("  [e] Erase CSV     : Delete log file for fresh start");
        Serial.println("  [m] Max record    : Set max record time in seconds (0=no limit), e.g. m300");
        Serial.println("  [h] Help          : Show this status");
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
        currentRFMode = MODE_STD;   // always boot on standard rate for quick sync (not LR)
        wifiChannel = prefs.getUChar("wch", 1);
        prefs.end();
        minuteTimer = millis();
    }
    
    applyRFSettings(currentRFMode);
    setPower(-1.0);
    lastPacketTime = millis();
    printDetailedStatus();
    if (!isMaster) Serial.println(">> Transponder ready. GPIO13 must be HIGH/floating (not GND).");
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
                    case '0': 
                        currentRFMode = MODE_STD;
                        prefs.begin("probe", false); prefs.putUChar("rfm", currentRFMode); prefs.end(); 
                        Serial.println(">> RF forced to STD (802.11b), restarting...");
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
                    case 'x':
                        prefs.begin("probe", false); prefs.clear(); prefs.end();
                        Serial.println(">> RF preference cleared. Restarting (next boot = STD)...");
                        delay(200); ESP.restart();
                        break;
                    case 'f': if (csvFileLogging) csvLogStop(); else csvLogStart(); break;
                    case 'd': csvLogDump(); break;
                    case 'e': csvLogErase(); break;
                    case 'm': maxRecordingTimeSec = (uint32_t)val; Serial.printf(">> CSV max record time: %u s (0=no limit)\n", maxRecordingTimeSec); break;
                    case 'n': {
                        uint8_t ch = (uint8_t)constrain((int)val, 1, 14);
                        wifiChannel = ch;
                        prefs.begin("probe", false); prefs.putUChar("wch", wifiChannel); prefs.end();
                        applyRFSettings(currentRFMode);
                        Serial.printf(">> Channel set to %u\n", wifiChannel);
                        break;
                    }
                }
            }
        }

        if (millis() >= nextPingTime) {
            nextPingTime = millis() + burstDelay + random(0, 50);  // +0..49 ms jitter to avoid syncing with WiFi beacons
            if (waitingForPong && !plotMode) { 
                if (lastKnownRSSI > -80.0) { linkCondition = "INTERFERENCE"; interfMinCounter++; }
                else { linkCondition = "RANGE LIMIT"; rangeMinCounter++; }
                Serial.printf("[%s] N:%u | [NO REPLY] | %s\n", getFastTimestamp().c_str(), nonceCounter, linkCondition.c_str());
                if (nonceCounter == 3) Serial.println(">> Tip: Transponder GPIO13 must be HIGH/floating (not GND). Same sketch on both? Wait 15s for RF mode sync.");
            }
            lastPingTime = millis(); nonceCounter++; waitingForPong = true; 
            time_t now; time(&now); struct tm ti; localtime_r(&now, &ti);
            myData.nonce = nonceCounter; myData.txPower = currentPower; myData.targetPower = remoteTargetPower;
            myData.pingInterval = burstDelay; myData.hour = ti.tm_hour; myData.minute = ti.tm_min; myData.second = ti.tm_sec;
            myData.channel = wifiChannel;
            esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
            digitalWrite(ledPin, HIGH); ledTimer = millis() + 30; currentLEDState = TX_FLASH;
        }
    } else {
        if (Serial.available() > 0) {
            char cmd = Serial.read();
            if (cmd == 'm') {
                maxRecordingTimeSec = (uint32_t)Serial.parseInt();
                if (Serial) Serial.printf(">> CSV max record time: %u s (0=no limit)\n", maxRecordingTimeSec);
            } else
            switch (cmd) {
                case 'f': if (csvFileLogging) csvLogStop(); else csvLogStart(); break;
                case 'd': csvLogDump(); break;
                case 'e': csvLogErase(); break;
                case 'h': printDetailedStatus(); break;
            }
        }
        if (millis() - lastPacketTime > transponderTimeout) { 
            lastPacketTime = millis(); 
            transponderConsecutiveTimeouts++;
            if (transponderConsecutiveTimeouts >= TRANSPONDER_CYCLE_AFTER_TIMEOUTS) {
                transponderConsecutiveTimeouts = 0;
                cycleTransponderProtocol(); 
            }
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