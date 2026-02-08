/*
 * dBm-Now | ESP32 RF Probe & Path Loss Analyzer
 * Copyright (C) dBm-Now project. Licensed under GPL v2. See LICENSE file.
 *
 * ======================================================================================
 * ESP32 RF PROBE & PATH LOSS ANALYZER | v4.1 (1-way RF + built-in Serial–MQTT Bridge)
 * ======================================================================================
 * Mode at boot: GPIO12 (BRIDGE_PIN) LOW = Serial-MQTT Bridge (WiFi Manager, Serial1→MQTT).
 *               GPIO12 HIGH/floating = Master/Transponder (GPIO13 = ROLE_PIN: LOW=Master, HIGH=Transponder).
 */

#define FW_VERSION "4.1"
// Serial baud rate. Set your Serial Monitor to the same value. Higher = less blocking at fast ping rates.
// Common options: 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000.
#define SERIAL_BAUD 921600
#define SERIAL_BAUD_1WAY_RF 9600   // when 1-way RF mode ON: Serial switches to this for Serial-MQTT bridge (e.g. Lumy88)
#define SERIAL_BAUD_BRIDGE_USB 9600   // bridge mode: USB Serial (debug/config) baud; use 9600 for monitor

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <FS.h>
#include <SPIFFS.h>
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read(void);  // ESP32 internal temp (raw); 128 = invalid. Convert: (raw - 32) / 1.8 = °C
#ifdef __cplusplus
}
#endif

// --- HARDWARE ROLE CONFIG ---
const int ROLE_PIN = 13;       // LOW = Master, HIGH/floating = Transponder
const int BRIDGE_PIN = 12;     // LOW = Serial-MQTT Bridge mode (WiFi Manager, Serial1→MQTT); HIGH/floating = Master/Transponder
bool isBridgeMode = false;     // true when BRIDGE_PIN held LOW at boot
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
bool requestOneWayRF = false;   // master requests transponder 1-way RF (no pong; reply via JSON on Serial)
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

// Rolling missed-packets (master: last N pongs)
#define MISSED_HISTORY_LEN 10
uint8_t missedHistory[MISSED_HISTORY_LEN];
uint8_t missedHistoryIdx = 0;
uint8_t missedHistoryCount = 0;   // 0..MISSED_HISTORY_LEN

// Rolling Minute Counters
float lastKnownRSSI = -100.0;
uint32_t rangeMinCounter = 0, interfMinCounter = 0;
unsigned long minuteTimer = 0;
String linkCondition = "STABLE";

// RF channel 1–14. Both devices boot on 1 for quick sync. Master sets via Serial; transponder follows from payload.
uint8_t wifiChannel = 1;
// Master: send target channel in payload N times before switching, so transponder can switch first for quicker link re-establishment
#define PENDING_CHANNEL_PINGS 3
uint8_t pendingChannel = 0;       // 0 = none; else target channel to switch to after PENDING_CHANNEL_PINGS pings
uint8_t pendingChannelPingsSent = 0;
// Master: same for RF mode (STD / LR 250k / LR 500k)
#define PENDING_RF_PINGS 3
uint8_t pendingRFMode = 3;        // 3 = none; 0/1/2 = target mode to switch to after PENDING_RF_PINGS pings
uint8_t pendingRFModePingsSent = 0;

// Promiscuous test mode (master only): channel scan for occupancy / noise proxy
bool promiscuousMode = false;
uint8_t promScanChannel = 1;
unsigned long promDwellStart = 0;
const unsigned long promDwellMs = 2143;  // ~30 s total for 14 channels

// Serial command value limits (validation)
#define TX_POWER_MIN (-1.0f)
#define TX_POWER_MAX 20.0f
#define PING_INTERVAL_MIN_MS 10u
#define PING_INTERVAL_MIN_LR_MS 10u   // LR can work at 10 ms like STD
#define PING_INTERVAL_MAX_MS 86400000u   // 24 h
#define MAX_RECORD_TIME_SEC 31536000u    // 1 year
// Jitter: prime ms values 1–17 to avoid periodic alignment with WiFi beacons (e.g. 100 ms TU)
static const uint8_t JITTER_PRIME_MS[] = {1, 2, 3, 5, 7, 11, 13, 17};
#define JITTER_PRIME_COUNT (sizeof(JITTER_PRIME_MS) / sizeof(JITTER_PRIME_MS[0]))
volatile uint32_t promPktCount = 0;
volatile int32_t promRssiSum = 0;
volatile int promMinRssi = 0;   // 0 = no packet yet; valid RSSI is negative

// Transponder Specific
unsigned long lastPacketTime = 0;
uint32_t transponderTimeout = 5000;
bool oneWayRFMode = false;   // when true: no ESP-NOW pong; reply via JSON on Serial (for Serial-MQTT bridge to cloud)
bool transponderHuntOnTimeout = false;   // when true: cycle channel/mode after timeouts to find master (lab use); OFF by default to avoid link de-sync in open-air / lossy links
#define TRANSPONDER_CYCLE_AFTER_TIMEOUTS 3   // require this many consecutive timeouts before hunting (avoids cycling on brief fades)
uint32_t transponderConsecutiveTimeouts = 0;
uint32_t lastReceivedNonce = 0;   // for gap detection: transponder reports missed packets when nonce is not consecutive 

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
    uint8_t rfMode;   // 0=STD, 1=LR 250k, 2=LR 500k
    uint8_t missedCount;  // transponder: number of packets missed before this nonce (gap in sequence)
    uint8_t oneWayRF;     // master→transponder: 0=normal (pong), 1=1-way RF (reply via JSON on Serial only)
} Payload;
Payload myData, txData, rxData;

// Forward declarations (needed when built as C++ e.g. PlatformIO; Arduino .ino ignores)
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incoming, int len);
void handleLED();
void csvLogStart();
void csvLogStop();
void csvLogDump();
void csvLogErase();
void promiscuousRx(void *buf, wifi_promiscuous_pkt_type_t type);
void promiscuousEnter();
void promiscuousExit();
void promiscuousSweepStep();

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

/* Internal chip temperature °C (original ESP32). Returns -999.0f if invalid/unsupported. */
float getChipTempC(void) {
    uint8_t raw = temprature_sens_read();
    if (raw == 128) return -999.0f;  /* invalid on ESP32 */
    return ((float)((int)raw - 32)) / 1.8f;
}

/* Current effective max TX power in dBm (0.25 dBm units from PHY). Returns -999.0f if get fails. Used to detect thermal throttling. */
float getActualMaxTxPowerDbm(void) {
    int8_t power_quarter_dbm = 0;
    if (esp_wifi_get_max_tx_power(&power_quarter_dbm) != ESP_OK) return -999.0f;
    return (float)power_quarter_dbm * 0.25f;
}

void setESP32Time(int hr, int min, int sec) {
    struct tm tm = {0}; tm.tm_year = 2026 - 1900; tm.tm_mon = 0; tm.tm_mday = 1;
    tm.tm_hour = hr; tm.tm_min = min; tm.tm_sec = (sec >= 0 && sec <= 59) ? sec : 0;
    time_t t = mktime(&tm); struct timeval now = { .tv_sec = t }; settimeofday(&now, NULL);
}

void setPower(float pwr) {
    float clamped = (pwr < TX_POWER_MIN) ? TX_POWER_MIN : (pwr > TX_POWER_MAX) ? TX_POWER_MAX : pwr;
    if (clamped != pwr && Serial && !plotMode) Serial.printf(">> TX power clamped to %.0f dBm (valid %.0f–%.0f)\n", clamped, TX_POWER_MIN, TX_POWER_MAX);
    if (clamped == currentPower) return;
    currentPower = clamped;
    int8_t pwr_val = (clamped >= 19.5) ? 78 : (clamped >= 11 ? 44 : 8);
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
            csvFile.println("timestamp,nonce,fwdLoss,bwdLoss,symmetry,zeroed,masterRSSI,remoteRSSI,linkPct,lavg,chipTempC");
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

// --- PROMISCUOUS TEST MODE (master only) ---
void promiscuousRx(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    int8_t r = pkt->rx_ctrl.rssi;
    promPktCount++;
    promRssiSum += r;
    if (promMinRssi == 0 || r < promMinRssi) promMinRssi = r;
}

void promiscuousEnter() {
    if (!isMaster || promiscuousMode) return;
    esp_now_deinit();
    promiscuousMode = true;
    promScanChannel = 1;
    promPktCount = 0;
    promRssiSum = 0;
    promMinRssi = 0;
    promDwellStart = millis();
    esp_wifi_set_channel(promScanChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous_rx_cb(&promiscuousRx);
    esp_wifi_set_promiscuous(true);
    Serial.println(">> Promiscuous scan: channels 1-14, ~2.1s each. Keeps scanning until [E] exit.");
    Serial.println("   Ch | Pkts | AvgRSSI | MinRSSI | Busy%");
}

void promiscuousExit() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    promiscuousMode = false;
    applyRFSettings(currentRFMode);
    setPower(currentPower);
    Serial.println(">> Promiscuous scan done. ESP-NOW resumed.");
}

void promiscuousSweepStep() {
    if (!isMaster || !promiscuousMode) return;
    if (millis() - promDwellStart < promDwellMs) return;

    uint32_t n = promPktCount;
    float avgRssi = (n > 0) ? (float)promRssiSum / (float)n : 0.0f;
    int minRssi = promMinRssi;
    float busyPct = (n > 0) ? (n * 1.0f / (promDwellMs / 1000.0f)) * 0.001f * 100.0f : 0.0f;  // rough: 1 ms per packet
    if (busyPct > 100.0f) busyPct = 100.0f;
    if (n > 0)
        Serial.printf("  %2u  | %4lu | %6.1f | %6d | %5.1f\n", promScanChannel, n, avgRssi, minRssi, busyPct);
    else
        Serial.printf("  %2u  |    0 |   --   |   N/A  |  0.0\n", promScanChannel);

    promScanChannel++;
    if (promScanChannel > 14) {
        promScanChannel = 1;
        Serial.println("--- next sweep ---");
    }
    esp_wifi_set_channel(promScanChannel, WIFI_SECOND_CHAN_NONE);
    promPktCount = 0;
    promRssiSum = 0;
    promMinRssi = 0;
    promDwellStart = millis();
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incoming, int len) {
    if (len < (int)sizeof(Payload) - 1) return;   // accept old 25-byte payload (no missedCount)
    lastPacketTime = millis();
    if (isMaster) {
        Payload pong;
        memcpy(&pong, incoming, len >= (int)sizeof(Payload) ? sizeof(pong) : (size_t)len);
        if (len < (int)sizeof(Payload)) pong.missedCount = 0;
        waitingForPong = false; pendingRX = true; 
        lastKnownRSSI = (float)info->rx_ctrl->rssi;
        linkCondition = "STABLE"; 
        float mRSSI = lastKnownRSSI; 
        float tRSSI = pong.measuredRSSI;         
        float fwdLoss = currentPower - tRSSI;
        float t_tx_eff = (pong.txPower != 0) ? pong.txPower : remoteTargetPower;
        float bwdLoss = t_tx_eff - mRSSI;
        float symmetry = fwdLoss - bwdLoss;
        if (!calibrated && referenceRSSI == 1.0f) { referenceRSSI = mRSSI; calibrated = true; }
        float zeroed = calibrated ? (mRSSI - referenceRSSI) : 0;

        if (pong.missedCount > 0 && !plotMode)
            Serial.printf(">> Transponder missed %u packet(s) (nonce(s) %u-%u)\n",
                (unsigned)pong.missedCount, (unsigned)(pong.nonce - pong.missedCount), (unsigned)(pong.nonce - 1));
        // Rolling missed: last MISSED_HISTORY_LEN pongs
        missedHistory[missedHistoryIdx] = pong.missedCount;
        missedHistoryIdx = (missedHistoryIdx + 1) % MISSED_HISTORY_LEN;
        if (missedHistoryCount < MISSED_HISTORY_LEN) missedHistoryCount++;
        uint8_t n = missedHistoryCount;
        uint32_t sumMissed = 0;
        uint8_t countWithLoss = 0;
        for (uint8_t i = 0; i < n; i++) {
            uint8_t j = (n == MISSED_HISTORY_LEN) ? (uint8_t)((missedHistoryIdx + i) % MISSED_HISTORY_LEN) : i;
            sumMissed += missedHistory[j];
            if (missedHistory[j] > 0) countWithLoss++;
        }
        uint8_t linkPct = (n > 0) ? (uint8_t)(100u - (countWithLoss * 100u) / (uint32_t)n) : 100;  // 100% = no missed pings
        float avgMissed = (n > 0) ? (float)sumMissed / (float)n : 0.0f;
        if (!plotMode) {
            const uint8_t *txMac = info->src_addr;
            float chipTemp = getChipTempC();
            if (chipTemp > -100.0f)
                Serial.printf("[%s] N:%u | TX %02x:%02x:%02x:%02x:%02x:%02x | FWD Loss:%.1f (R:%.0f) | BWD Loss:%.1f (R:%.0f) | Sym:%.1f | Z:%.1f | T:%.1f C | Link%%:%u Lavg:%.1f\n",
                              getFastTimestamp().c_str(), nonceCounter,
                              txMac[0], txMac[1], txMac[2], txMac[3], txMac[4], txMac[5],
                              fwdLoss, tRSSI, bwdLoss, mRSSI, symmetry, zeroed, chipTemp, linkPct, avgMissed);
            else
                Serial.printf("[%s] N:%u | TX %02x:%02x:%02x:%02x:%02x:%02x | FWD Loss:%.1f (R:%.0f) | BWD Loss:%.1f (R:%.0f) | Sym:%.1f | Z:%.1f | T:N/A | Link%%:%u Lavg:%.1f\n",
                              getFastTimestamp().c_str(), nonceCounter,
                              txMac[0], txMac[1], txMac[2], txMac[3], txMac[4], txMac[5],
                              fwdLoss, tRSSI, bwdLoss, mRSSI, symmetry, zeroed, linkPct, avgMissed);
            float actualMax = getActualMaxTxPowerDbm();
            if (actualMax > -100.0f && actualMax < currentPower - 1.0f)
                Serial.printf(">> Thermal throttling: TX power reduced to %.1f dBm (requested %.1f dBm)\n", actualMax, currentPower);
        } else {
            Serial.printf("%u,%.1f,%.1f,%.1f,%.1f,%u,%.1f\n", (unsigned)wifiChannel, fwdLoss, bwdLoss, symmetry, zeroed, linkPct, avgMissed);
        }
        if (csvFileLogging && csvFile) {
            if (maxRecordingTimeSec > 0 && (millis() - csvLogStartTime) >= (unsigned long)maxRecordingTimeSec * 1000) {
                csvLogStop();
                Serial.println(">> CSV logging stopped (max time reached).");
            } else {
                float chipTemp = getChipTempC();
                csvFile.printf("%s,%u,%.1f,%.1f,%.1f,%.1f,%.0f,%.0f,%u,%.1f,%.1f\n",
                    getFastTimestamp().c_str(), nonceCounter, fwdLoss, bwdLoss, symmetry, zeroed, mRSSI, tRSSI, linkPct, avgMissed, chipTemp);
                csvFile.flush();
            }
        }
    } else {
        transponderConsecutiveTimeouts = 0;   // any received packet resets "lost" count
        if (len >= (int)sizeof(rxData))
            memcpy(&rxData, incoming, sizeof(rxData));
        else {
            memcpy(&rxData, incoming, (size_t)len);
            rxData.missedCount = 0;   // old 25-byte payload
            if (len < (int)sizeof(Payload)) rxData.oneWayRF = 0;   // old payload: no oneWayRF field
        }
        if (len >= (int)sizeof(Payload)) {
            if (rxData.oneWayRF != 0) {
                if (!oneWayRFMode) {
                    oneWayRFMode = true;
                    Serial.begin(SERIAL_BAUD_1WAY_RF);
                }
            } else {
                if (oneWayRFMode) {
                    oneWayRFMode = false;
                    Serial.begin(SERIAL_BAUD);
                }
            }
        }
        uint8_t missedCount = 0;
        if (lastReceivedNonce != 0 && rxData.nonce > lastReceivedNonce) {
            uint32_t gap = rxData.nonce - lastReceivedNonce - 1;
            if (gap <= 255) {
                missedCount = (uint8_t)gap;
                if (!oneWayRFMode && Serial) Serial.printf(">> Missed packet(s): nonce(s) %u-%u (gap %u)\n",
                    (unsigned)(lastReceivedNonce + 1), (unsigned)(rxData.nonce - 1), (unsigned)missedCount);
            }
        }
        lastReceivedNonce = rxData.nonce;

        float rssi = (float)info->rx_ctrl->rssi;
        float pathLoss = rxData.txPower - rssi;
        const char* rfModeStr = (currentRFMode == MODE_STD) ? "STD" : (currentRFMode == MODE_LR_250K) ? "LR 250k" : "LR 500k";
        const uint8_t *mstrMac = info->src_addr;
        if (oneWayRFMode) {
            // 1-way RF mode: reply via JSON on Serial (no ESP-NOW pong). For Serial-MQTT bridge to cloud.
            // Use master time from payload so ts increments with each ping (transponder RTC may not have seconds synced yet).
            char tsBuf[10];
            snprintf(tsBuf, sizeof(tsBuf), "%02d:%02d:%02d", rxData.hour, rxData.minute, rxData.second);
            if (Serial) Serial.printf("{\"pl\":%.1f,\"rssi\":%.0f,\"mp\":%.1f,\"tp\":%.1f,\"n\":%u,\"ch\":%u,\"m\":\"%s\",\"ts\":\"%s\"}\n",
                pathLoss, rssi, rxData.txPower, currentPower, (unsigned)rxData.nonce, (unsigned)wifiChannel, rfModeStr, tsBuf);
        } else {
            if (Serial) Serial.printf("[%s] RX N=%u | Mstr %02x:%02x:%02x:%02x:%02x:%02x | %s | RSSI:%.0f dBm | Mstr Pwr:%.1f dBm | Path Loss:%.1f dB | TX Pwr:%.1f dBm\n",
                getFastTimestamp().c_str(), rxData.nonce,
                mstrMac[0], mstrMac[1], mstrMac[2], mstrMac[3], mstrMac[4], mstrMac[5],
                rfModeStr, rssi, rxData.txPower, pathLoss, currentPower);
        }
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
        setESP32Time(rxData.hour, rxData.minute, rxData.second);
        if (rxData.rfMode <= 2 && rxData.rfMode != currentRFMode) {
            currentRFMode = rxData.rfMode;
            applyRFSettings(currentRFMode);
        }
        if (rxData.channel >= 1 && rxData.channel <= 14 && rxData.channel != wifiChannel) {
            wifiChannel = rxData.channel;
            esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
            // Update master peer to new channel so esp_now_send succeeds (peer channel must match home channel)
            if (esp_now_is_peer_exist(info->src_addr)) {
                esp_now_del_peer(info->src_addr);
            }
            esp_now_peer_info_t peerInfo = {};
            memcpy(peerInfo.peer_addr, info->src_addr, 6);
            peerInfo.channel = wifiChannel;
            peerInfo.encrypt = false;
            esp_now_add_peer(&peerInfo);
        } else if (!esp_now_is_peer_exist(info->src_addr)) {
            esp_now_peer_info_t peerInfo = {};
            memcpy(peerInfo.peer_addr, info->src_addr, 6);
            peerInfo.channel = wifiChannel;
            peerInfo.encrypt = false;
            esp_now_add_peer(&peerInfo);
        }
        setPower(rxData.targetPower);
        if (!oneWayRFMode) {
            txData.nonce = rxData.nonce;
            txData.missedCount = missedCount;
            txData.txPower = currentPower;
            txData.channel = wifiChannel;
            txData.rfMode = currentRFMode;
            txData.measuredRSSI = (float)info->rx_ctrl->rssi;
            txData.oneWayRF = 0;   // pong: transponder does not request 1-way
            esp_now_send(info->src_addr, (uint8_t *) &txData, sizeof(txData));
        }
        digitalWrite(ledPin, HIGH); ledTimer = millis() + 40;
    }
}

void printDetailedStatus() {
    if (plotMode) return;
    if (!isMaster && oneWayRFMode) return;   // transponder in 1-way mode: only JSON, no status
    String modeStr = "STANDARD (802.11)";
    if (currentRFMode == MODE_LR_250K) modeStr = "LR (250kbps)";
    if (currentRFMode == MODE_LR_500K) modeStr = "LR (500kbps)";

    Serial.println("\n==================================================");
    Serial.printf("   ESP32 RF PROBE | v%s | ROLE: %s\n", FW_VERSION, deviceRole.c_str());
    Serial.println("==================================================");
    Serial.printf("  RF PROTOCOL : %s\n", modeStr.c_str());
    Serial.printf("  RF CHANNEL  : %u (1-14)\n", wifiChannel);
    if (isMaster) {
        Serial.printf("  MAC ADDR   : %s\n", WiFi.macAddress().c_str());
        Serial.printf("  ESP-NOW    : TX: broadcast, RX: unicast\n");
        Serial.printf("  LINK STATUS : %s\n", linkCondition.c_str());
        Serial.printf("  MASTER PWR  : %.1f dBm | REMOTE: %.1f dBm\n", currentPower, remoteTargetPower);
        Serial.printf("  1-WAY RF    : %s (request transponder reply via JSON on Serial, no pong)\n", requestOneWayRF ? "REQUESTED" : "OFF");
        { float t = getChipTempC(); if (t > -100.0f) Serial.printf("  CHIP TEMP   : %.1f C\n", t); else Serial.println("  CHIP TEMP   : N/A"); }
        { float a = getActualMaxTxPowerDbm(); if (a > -100.0f && a < currentPower - 1.0f) Serial.printf("  THERMAL     : Throttled (actual %.1f dBm, requested %.1f dBm)\n", a, currentPower); else Serial.println("  THERMAL     : OK"); }
        Serial.printf("  CSV LOG     : %s\n", csvFileLogging ? "ON" : "OFF");
        Serial.printf("  CSV MAX    : %u s (0=no limit)\n", maxRecordingTimeSec);
        Serial.println("--------------------------------------------------");
        Serial.println("  [l] Toggle Mode    : Cycle STD -> 250k -> 500k");
        Serial.println("  [W] 1-way RF       : Request transponder reply via JSON on Serial (no pong)");
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
        Serial.println("  [P] Promiscuous    : Start channel scan (loops 1-14 until [E] exit)");
    } else {
        Serial.printf("  TX PWR    : %.1f dBm (transponder transmit power, set by master)\n", currentPower);
        { float a = getActualMaxTxPowerDbm(); if (a > -100.0f && a < currentPower - 1.0f) Serial.printf("  THERMAL   : Throttled (actual %.1f dBm)\n", a); else Serial.println("  THERMAL   : OK"); }
        Serial.printf("  RF CHANNEL: %u (follows master)\n", wifiChannel);
        Serial.printf("  1-WAY RF  : %s (reply via JSON on Serial; no ESP-NOW pong)\n", oneWayRFMode ? "ON" : "OFF");
        Serial.printf("  TIMEOUT   : %u ms\n", transponderTimeout);
        Serial.printf("  HUNT ON TIMEOUT : %s (cycle channel/mode if no ping; [H] toggle; OFF = stay on channel for open-air)\n", transponderHuntOnTimeout ? "ON" : "OFF");
        Serial.printf("  CSV LOG   : %s (master→TX reception)\n", csvFileLogging ? "ON" : "OFF");
        Serial.printf("  CSV MAX   : %u s (0=no limit)\n", maxRecordingTimeSec);
        Serial.printf("  MAC ADDR   : %s\n", WiFi.macAddress().c_str());
        Serial.printf("  ESP-NOW    : RX: broadcast, TX: unicast\n");
        Serial.println("  (RX lines: timestamp | N | Mstr MAC | mode | RSSI | Mstr Pwr | Path Loss | TX Pwr)");
        Serial.println("--------------------------------------------------");
        Serial.println("  [W] 1-way RF      : Reply via JSON on Serial (no pong); for Serial-MQTT bridge to cloud");
        Serial.println("  [H] Hunt on timeout: Toggle cycle channel/mode when no ping (OFF by default; use for lab)");
        Serial.println("  [0] Force STD     : Set RF to 802.11b and restart (resync with master)");
        Serial.printf("  [f] CSV file log  : Toggle logging to %s (SPIFFS)\n", CSV_PATH);
        Serial.println("  [d] Dump CSV      : Print log file to Serial (copy to save)");
        Serial.println("  [e] Erase CSV     : Delete log file for fresh start");
        Serial.println("  [m] Max record    : Set max record time in seconds (0=no limit), e.g. m300");
        Serial.println("  [h] Help          : Show this status");
    }
    Serial.println("==================================================\n");
}

void setup() {
    pinMode(BRIDGE_PIN, INPUT_PULLUP);
    delay(50);
    if (digitalRead(BRIDGE_PIN) == LOW) {
        isBridgeMode = true;
        Serial.begin(SERIAL_BAUD_BRIDGE_USB);
        Serial.println("Bridge mode (GPIO12=LOW). Serial-MQTT bridge starting.");
        setupBridge();
        return;
    }
    Serial.begin(SERIAL_BAUD);
    pinMode(ledPin, OUTPUT);
    pinMode(ROLE_PIN, INPUT_PULLUP);
    delay(50);
    isMaster = (digitalRead(ROLE_PIN) == LOW);
    deviceRole = isMaster ? "MASTER" : "TRANSPONDER";
    
    WiFi.mode(WIFI_STA);
    // Disable WiFi modem sleep for consistent ping/pong latency (use WIFI_PS_MIN_MODEM for battery if needed)
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (isMaster) {
        prefs.begin("probe", true); 
        currentRFMode = MODE_STD;   // always boot on standard rate for quick sync (not LR)
        // Master always boots on channel 1 (like transponder); channel still saved to NVS when user sets via 'n'
        prefs.end();
        minuteTimer = millis();
    } else {
        prefs.begin("probe", true);
        transponderHuntOnTimeout = prefs.getBool("huntT", false);   // default OFF for open-air / lossy links
        prefs.end();
    }
    
    applyRFSettings(currentRFMode);
    setPower(-1.0);
    lastPacketTime = millis();
    printDetailedStatus();
    if (!isMaster) Serial.println(">> Transponder ready. GPIO13 must be HIGH/floating (not GND).");
}

void loop() {
    if (isBridgeMode) {
        loopBridge();
        return;
    }
    if (!promiscuousMode && millis() - lastStatusPrint >= 10000) { lastStatusPrint = millis(); printDetailedStatus(); }

    if (isMaster) {
        if (promiscuousMode) {
            promiscuousSweepStep();
            if (Serial.available() > 0) { char c = Serial.read(); if (c == 'E' || c == 'e') promiscuousExit(); }
        } else {
        handleLED();
        if (millis() - minuteTimer >= 60000) {
            Serial.printf("\n>>> [MINUTE SUMMARY] Interference: %u | Signal too low: %u <<<\n\n", interfMinCounter, rangeMinCounter);
            interfMinCounter = 0; rangeMinCounter = 0; minuteTimer = millis();
        }

        if (Serial.available() > 0) {
            char cmd = Serial.read();
            if (cmd == 'k') {
                int val = Serial.parseInt();
                int hh = val / 100, mm = val % 100;
                if (val < 0 || val > 2359 || hh > 23 || mm > 59) {
                    if (Serial && !plotMode) Serial.println(">> Invalid time (use HHMM 0000–2359)");
                } else {
                    setESP32Time(hh, mm, 0);
                }
            }
            else {
                float val = Serial.parseFloat();
                switch (cmd) {
                    case 'P': promiscuousEnter(); break;
                    case 'l': {
                        uint8_t nextMode = (currentRFMode + 1) % 3;
                        pendingRFMode = nextMode;
                        pendingRFModePingsSent = 0;
                        const char* modeStr = (pendingRFMode == MODE_STD) ? "STD (802.11)" : (pendingRFMode == MODE_LR_250K) ? "LR 250k" : "LR 500k";
                        if (!plotMode) Serial.printf(">> Switching to %s after %u pings (transponder switches first)\n", modeStr, (unsigned)PENDING_RF_PINGS);
                        break;
                    }
                    case '0': 
                        currentRFMode = MODE_STD;
                        prefs.begin("probe", false); prefs.putUChar("rfm", currentRFMode); prefs.end(); 
                        Serial.println(">> RF forced to STD (802.11b), restarting...");
                        delay(100); ESP.restart(); 
                        break;
                    case 'W':
                    case 'w':
                        requestOneWayRF = !requestOneWayRF;
                        if (!plotMode) Serial.println(requestOneWayRF ? ">> 1-way RF requested: transponder will reply via JSON on Serial (no pong). Next pings send request." : ">> 1-way RF off: transponder will reply with pong over ESP-NOW.");
                        break;
                    case 'p': setPower(val); break;
                    case 't': {
                        float tClamp = (val < TX_POWER_MIN) ? TX_POWER_MIN : (val > TX_POWER_MAX) ? TX_POWER_MAX : val;
                        if (tClamp != val && Serial && !plotMode) Serial.printf(">> Remote target power clamped to %.0f dBm (valid %.0f–%.0f)\n", tClamp, TX_POWER_MIN, TX_POWER_MAX);
                        remoteTargetPower = tClamp;
                        break;
                    }
                    case 'v': plotMode = !plotMode; break;
                    case 'r': {
                        uint32_t minMs = (currentRFMode != MODE_STD) ? PING_INTERVAL_MIN_LR_MS : PING_INTERVAL_MIN_MS;
                        uint32_t v = (val <= 0) ? minMs : (val < minMs) ? minMs : (val > PING_INTERVAL_MAX_MS) ? PING_INTERVAL_MAX_MS : (uint32_t)val;
                        if ((val <= 0 || val < minMs || val > PING_INTERVAL_MAX_MS) && Serial && !plotMode) Serial.printf(">> Ping interval clamped to %u ms (LR min %u, max %u)\n", v, (unsigned)PING_INTERVAL_MIN_LR_MS, (unsigned)PING_INTERVAL_MAX_MS);
                        burstDelay = v;
                        break;
                    }
                    case 'h': printDetailedStatus(); break;
                    case 's': remoteTargetPower = currentPower; break;
                    case 'c': interfMinCounter = 0; rangeMinCounter = 0; Serial.println(">> Stats Reset."); break;
                    case 'z': {
                        if (lastKnownRSSI > -95.0f) {
                            referenceRSSI = lastKnownRSSI;
                            calibrated = true;
                            if (!plotMode) Serial.printf(">> Zero cal: reference set to %.1f dBm (Z = delta from now)\n", referenceRSSI);
                        } else {
                            calibrated = false;
                            referenceRSSI = 1.0f;
                            if (!plotMode) Serial.println(">> Zero cal: no pong yet; reference will be set on next pong.");
                        }
                        break;
                    }
                    case 'x':
                        prefs.begin("probe", false); prefs.clear(); prefs.end();
                        Serial.println(">> RF preference cleared. Restarting (next boot = STD)...");
                        delay(200); ESP.restart();
                        break;
                    case 'f': if (csvFileLogging) csvLogStop(); else csvLogStart(); break;
                    case 'd': csvLogDump(); break;
                    case 'e': csvLogErase(); break;
                    case 'm': {
                        uint32_t mVal = (uint32_t)(val < 0 ? 0 : (val > MAX_RECORD_TIME_SEC ? MAX_RECORD_TIME_SEC : val));
                        if ((val < 0 || val > MAX_RECORD_TIME_SEC) && Serial && !plotMode) Serial.printf(">> Max record time clamped to 0–%u s\n", (unsigned)MAX_RECORD_TIME_SEC);
                        maxRecordingTimeSec = mVal;
                        if (Serial && !plotMode) Serial.printf(">> CSV max record time: %u s (0=no limit)\n", maxRecordingTimeSec);
                        break;
                    }
                    case 'n': {
                        uint8_t ch = (uint8_t)constrain((int)val, 1, 14);
                        prefs.begin("probe", false); prefs.putUChar("wch", ch); prefs.end();
                        if (ch == wifiChannel) {
                            pendingChannel = 0;
                            pendingChannelPingsSent = 0;
                            Serial.printf(">> Already on channel %u\n", wifiChannel);
                        } else {
                            pendingChannel = ch;
                            pendingChannelPingsSent = 0;
                            Serial.printf(">> Switching to channel %u after %u pings (transponder switches first)\n", ch, (unsigned)PENDING_CHANNEL_PINGS);
                        }
                        break;
                    }
                }
            }
        }

        if (millis() >= nextPingTime) {
            nextPingTime = millis() + burstDelay + (unsigned long)JITTER_PRIME_MS[random(0, (int)JITTER_PRIME_COUNT)];
            if (waitingForPong && !plotMode) { 
                // High last RSSI → likely collision (interference); low last RSSI → likely range/signal too low
                if (requestOneWayRF) { linkCondition = "1-way mode"; }
                else if (lastKnownRSSI > -80.0) { linkCondition = "INTERFERENCE"; interfMinCounter++; }
                else { linkCondition = "SIGNAL TOO LOW"; rangeMinCounter++; }
                Serial.printf("[%s] N:%u | [NO REPLY] | %s\n", getFastTimestamp().c_str(), nonceCounter, linkCondition.c_str());
                if (nonceCounter == 3) Serial.println(">> Tip: Transponder GPIO13 must be HIGH/floating (not GND). Same sketch on both? Wait 15s for RF mode sync.");
            }
            lastPingTime = millis(); nonceCounter++; waitingForPong = true; 
            time_t now; time(&now); struct tm ti; localtime_r(&now, &ti);
            myData.nonce = nonceCounter; myData.txPower = currentPower; myData.targetPower = remoteTargetPower;
            myData.pingInterval = burstDelay; myData.hour = ti.tm_hour; myData.minute = ti.tm_min; myData.second = ti.tm_sec;
            myData.channel = (pendingChannel != 0) ? pendingChannel : wifiChannel;  // send target channel so transponder can switch first
            myData.rfMode = (pendingRFMode <= 2) ? pendingRFMode : currentRFMode;   // send target RF mode so transponder can switch first
            myData.missedCount = 0;   // master does not use; transponder echoes gap count in pong
            myData.oneWayRF = requestOneWayRF ? 1 : 0;   // transponder replies via JSON on Serial (no pong) when 1
            esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
            if (pendingChannel != 0) {
                pendingChannelPingsSent++;
                if (pendingChannelPingsSent >= PENDING_CHANNEL_PINGS) {
                    wifiChannel = pendingChannel;
                    pendingChannel = 0;
                    pendingChannelPingsSent = 0;
                    applyRFSettings(currentRFMode);
                    if (!plotMode) Serial.printf(">> Now on channel %u\n", wifiChannel);
                }
            }
            if (pendingRFMode <= 2) {
                pendingRFModePingsSent++;
                if (pendingRFModePingsSent >= PENDING_RF_PINGS) {
                    currentRFMode = pendingRFMode;
                    pendingRFMode = 3;
                    pendingRFModePingsSent = 0;
                    prefs.begin("probe", false); prefs.putUChar("rfm", currentRFMode); prefs.end();
                    applyRFSettings(currentRFMode);
                    if (!plotMode) Serial.printf(">> Now on %s\n", (currentRFMode == MODE_STD) ? "STD (802.11)" : (currentRFMode == MODE_LR_250K) ? "LR 250k" : "LR 500k");
                }
            }
            digitalWrite(ledPin, HIGH); ledTimer = millis() + 30; currentLEDState = TX_FLASH;
        }
        }
    } else {
        if (Serial.available() > 0) {
            char cmd = Serial.read();
            if (cmd == 'm') {
                long raw = Serial.parseInt();
                long mVal = (raw < 0) ? 0 : (raw > (long)MAX_RECORD_TIME_SEC) ? (long)MAX_RECORD_TIME_SEC : raw;
                if (mVal != raw && Serial) Serial.printf(">> Max record time clamped to 0–%u s\n", (unsigned)MAX_RECORD_TIME_SEC);
                maxRecordingTimeSec = (uint32_t)mVal;
                if (Serial) Serial.printf(">> CSV max record time: %u s (0=no limit)\n", maxRecordingTimeSec);
            } else
            switch (cmd) {
                case 'W':
                case 'w':
                    oneWayRFMode = !oneWayRFMode;
                    if (oneWayRFMode) {
                        Serial.begin(SERIAL_BAUD_1WAY_RF);
                        // no status message in 1-way mode: only JSON goes out (for MQTT bridge)
                    } else {
                        Serial.begin(SERIAL_BAUD);
                        if (Serial) Serial.println(">> 1-way RF mode OFF: Serial restored, replying with pong over ESP-NOW.");
                    }
                    break;
                case 'H':
                    transponderHuntOnTimeout = !transponderHuntOnTimeout;
                    prefs.begin("probe", false); prefs.putBool("huntT", transponderHuntOnTimeout); prefs.end();
                    if (Serial) Serial.printf(">> Hunt on timeout: %s (transponder %s cycle channel/mode when no ping)\n", transponderHuntOnTimeout ? "ON" : "OFF", transponderHuntOnTimeout ? "will" : "will not");
                    break;
                case '0':
                    currentRFMode = MODE_STD;
                    prefs.begin("probe", false); prefs.putUChar("rfm", currentRFMode); prefs.end();
                    if (Serial) Serial.println(">> RF forced to STD (802.11b), restarting...");
                    delay(100); ESP.restart();
                    break;
                case 'f': if (csvFileLogging) csvLogStop(); else csvLogStart(); break;
                case 'd': csvLogDump(); break;
                case 'e': csvLogErase(); break;
                case 'h': printDetailedStatus(); break;
            }
        }
        if (millis() - lastPacketTime > transponderTimeout) { 
            lastPacketTime = millis(); 
            if (transponderHuntOnTimeout) {
                transponderConsecutiveTimeouts++;
                if (transponderConsecutiveTimeouts >= TRANSPONDER_CYCLE_AFTER_TIMEOUTS) {
                    transponderConsecutiveTimeouts = 0;
                    cycleTransponderProtocol(); 
                }
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